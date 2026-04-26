> **Status: Work In Progress (Phase 11a)** — the seven `M_HL2_*` master
> materials still need to be generated or authored in the Unreal editor.
> Settings, defaults, and the MIC binder are already shipped; only the
> `.uasset` files are missing.

# Authoring the HL2 master materials

This guide walks an author through generating or creating the seven `UMaterial`
parents the HL2 BSP Importer expects under `/HL2BSPImporter/MasterMaterials/`. The
parameter contract (names, types, VMT keys) lives in
[../../Resources/MasterMaterials/README.md](../../Resources/MasterMaterials/README.md);
this file covers **how** to build the assets in the editor.

## Preferred path: run the generator

The plugin includes an Unreal Editor Python generator at
[`../../Scripts/GenerateMasterMaterials.py`](../../Scripts/GenerateMasterMaterials.py).
Run it from an editor-capable Unreal session so the Material Editor APIs and
shader compiler are available.

Python console / Editor Utility example:

```python
exec(open(r"C:/GitHub/HL2Unreal/HL2BSPImporter/Scripts/GenerateMasterMaterials.py", "r", encoding="utf-8").read())
```

Commandlet-style example:

```bat
UnrealEditor-Cmd.exe YourProject.uproject -run=PythonScript -script="C:/GitHub/HL2Unreal/HL2BSPImporter/Scripts/GenerateMasterMaterials.py"
```

By default the script skips existing materials. To regenerate an existing set,
edit `OVERWRITE_EXISTING = True` in the script or call:

```python
import sys
sys.path.append(r"C:/GitHub/HL2Unreal/HL2BSPImporter/Scripts")
import GenerateMasterMaterials
GenerateMasterMaterials.generate_all(overwrite_existing=True)
```

After the script succeeds, save/submit the generated `.uasset` files under
`HL2BSPImporter/Content/MasterMaterials/`. The roadmap item is not complete
until those generated assets are actually shipped with the plugin.

## Why this is editor-only (not CLI)

1. **`UMaterial` is a graph asset, not text.** It serialises a
   `UMaterialExpression` node graph plus compiled shader maps in the
   `FMaterialResource` cooked-data section. There is no canonical text format
   and no headless `UFactory` path that builds graphs.
2. **Shader compilation requires the editor's `MaterialShaderMap` pipeline.**
   Saving a `UMaterial` triggers `UMaterial::PostEditChangeProperty` →
   `FMaterial::CacheShaders`, which needs the running editor's shader-compiler
   workers and a live RHI for cooked validation.
3. **Domain / blend / shading-model flags persist via `UMaterialEditorOnlyData`**
   (UE5.1+). Setting `MaterialDomain=MD_DeferredDecal`,
   `BlendMode=BLEND_Translucent` / `BLEND_Masked`, `ShadingModel=MSM_Unlit`,
   etc. is editor-only API.
4. **Unreal ships no `UMaterial` text/JSON importer.** Unlike textures
   (`UTextureFactory`), materials have no equivalent file factory in shipped
   engine code.

A `UnrealEditor-Cmd -run=PythonScript` approach using
`unreal.MaterialEditingLibrary` still requires a running editor process. The
generator script automates the graph construction, but if an engine-version API
change breaks the script, the manual checklist below remains the fallback.

## Manual fallback: per-asset checklist

Right-click in the Content Browser at `/HL2BSPImporter/MasterMaterials/` →
**Material** → name the asset **exactly** as listed (the importer matches by
soft-object path; see
[HL2BSPImporterSettings.h](../../Source/HL2BSPImporter/Public/HL2BSPImporterSettings.h)).

| Asset                     | Material Domain  | Blend Mode  | Shading Model | Notes                                                                 |
|---------------------------|------------------|-------------|---------------|-----------------------------------------------------------------------|
| `M_HL2_Lit`               | Surface          | Opaque      | Default Lit   | Baseline opaque world surface.                                        |
| `M_HL2_LitMasked`         | Surface          | Masked      | Default Lit   | Wire `AlphaTestRef` to **Opacity Mask Clip Value**.                   |
| `M_HL2_LitTranslucent`    | Surface          | Translucent | Default Lit   | Wire `OpacityScalar` (× `BaseColor.a`) into **Opacity**.              |
| `M_HL2_LitDecal`          | **Deferred Decal** | Translucent | Default Lit   | Decal Blend Mode = `Translucent` (Color/Normal/Roughness as desired). |
| `M_HL2_WorldVertexBlend`  | Surface          | Opaque      | Default Lit   | Build the lerp graph in the next section.                             |
| `M_HL2_VertexLit`         | Surface          | Opaque      | Default Lit   | No lightmap input; lit by Lumen direct + sky.                         |
| `M_HL2_Unlit`             | Surface          | Opaque      | **Unlit**     | Pipe `BaseColor × BaseColorTint` into **Emissive Color**.             |

## Shared parameter wiring

Add every parameter from the **Phase 5 / 5b** and **Phase 11b** tables in
[../../Resources/MasterMaterials/README.md](../../Resources/MasterMaterials/README.md)
that the asset can usefully consume. Rules:

- **Names are case-sensitive `FName` matches** — the MIC binder
  ([`HL2MaterialBuilder.cpp`](../../Source/HL2BSPImporter/Private/HL2MaterialBuilder.cpp))
  silently skips parameters the parent doesn't declare, so unused entries are
  fine but typos break binding.
- **`EmissiveColor` defaults to a 1×1 white texture.** When `$selfillummask`
  is absent the binder leaves `EmissiveColor` unbound and relies on
  `EmissiveStrength = 1` + `BaseColor.a` for the mask; the master should
  multiply `EmissiveColor` × `BaseColor.a` × `EmissiveTint × EmissiveStrength`
  into Emissive.
- **UV transforms** (`BaseColorUVScale/Offset/Rotate`,
  `NormalUVScale/Offset/Rotate`) feed a `CustomRotator` → `Multiply` (Scale)
  → `Add` (Offset) chain on the corresponding sampler's UVs. `Scale` and
  `Offset` are stored in `xy` of a Vector parameter; `z`/`w` are unused.
- **`Cubemap` (TextureCube)** defaults to a neutral grey cube; sample with
  the reflected world-space view direction and tint by `EnvmapTint`. Phase
  11c will decide whether to bake per-leaf cubemaps; until then this is a
  cheap reflection approximation.
- **Phong** (`Phong`, `PhongExponent`, `PhongBoost`) drives a `lerp` between
  the engine default specular response and a boosted highlight; `Phong=0`
  collapses to the engine default.
- **Detail blend** (`DetailBlendMode`, `DetailBlendFactor`) — implement
  modes 0–2 (mod2x, additive, blend) at minimum; higher Source modes are
  rare in HL2 maps and can be stubbed to mode 0.

### `M_HL2_WorldVertexBlend` graph

```
edge   = BlendModulate.r          // remapped blend midpoint
soft   = BlendModulate.g * 0.5    // half-width of the soft edge
mask   = saturate( (VertexColor.a - (edge - soft)) / max(soft * 2.0, 1e-4) )
albedo = lerp(BaseColor * BaseColorTint, BaseColor2 * BaseColor2Tint, mask)
normal = lerp(Normal,    Normal2,    mask)
```

When `BlendModulate` is unbound, fall back to `mask = VertexColor.a`. When
`Normal2` is unbound, fall back to `Normal`.

## Save & cleanup

1. Save each asset (`Ctrl+S`) so the `.uasset` lands under
   `HL2BSPImporter/Content/MasterMaterials/`.
2. Verify the soft-object paths match
   **Project Settings → HL2 BSP Importer → Materials | Synthesis | Parents**.
3. Delete `PLACEHOLDER.md` once all seven assets exist.

## Verification

1. Import a small HL2 map (e.g. `d1_trainstation_01`).
2. Confirm the import log no longer contains
   `No parent material configured for shader '<x>'`.
3. Spot-check a synthesised `UMaterialInstanceConstant` under the configured
   `SynthesizedAssetRoot` — the expected texture/scalar/vector parameters
   should be bound, and inherited parents should match the shader-to-slot
   table in [../../Resources/MasterMaterials/README.md](../../Resources/MasterMaterials/README.md).
