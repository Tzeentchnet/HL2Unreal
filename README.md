# HL2 BSP Importer

A Half-Life 2 / Source Engine `.bsp` map importer for **Unreal Engine 5.6**. Drops a Source map onto Unreal as a single Static Mesh asset, optionally synthesizing materials and textures from the original `.vmt`/`.vtf` files, and emits an entity DataTable alongside.

> Status: editor-only plugin, opinionated import path. Best results with HL2 / EP1 / EP2 / CS:S maps (VBSP v19 / v20). CS:GO / L4D2 / Workshop maps with LZMA-compressed lumps are detected and rejected with a clear error rather than silently corrupted.

---

## What it does

For a given `mapname.bsp`, the importer:

1. Validates the file (extension, magic, version, lump bounds, sane sizes).
2. Parses the lumps it needs: vertices, edges, surfedges, faces, texinfo, texdata + string table, models, dispinfo, dispverts, entities.
3. Builds one `FMeshDescription` from the worldspawn brush faces and displacement grids, with:
   - Per-face polygon groups keyed by Source texture name.
   - Welded vertex instances per polygon (no fan-triangulation duplication).
   - Two UV channels — Source-projected UV0 and lightmap UV1.
   - Vertex colors carrying displacement `m_Alpha` for `WorldVertexTransition` blends.
4. Resolves a material for each polygon group, in this order:
   - Explicit JSON map (`MaterialJsonPath`).
   - Synthesized `UMaterialInstanceConstant` parented to a project-supplied master material, generated from the corresponding `.vmt` and `.vtf` files.
   - Engine default surface material as a final fallback.
5. Builds the `UStaticMesh` via the editor source-model path (so Nanite settings, MikkTSpace tangents, and auto-lightmap UV generation actually take effect).
6. Optionally adds Complex-As-Simple collision and creates `<MeshName>_Entities` (a `UDataTable` of `FHL2Entity` rows) when the BSP contains entities.

What it deliberately does **not** do (yet) is listed under [Limitations](#limitations).

---

## Install

1. Copy the `HL2BSPImporter` folder into your project's `Plugins/` directory.
2. Right-click the `.uproject` → **Generate Visual Studio project files**.
3. Build and open the project. The plugin auto-enables.

The module is gated to Editor builds via `WhitelistTargets` in the `.uplugin`, so it adds no runtime cost to packaged games.

---

## Quick start

1. Drag a `.bsp` file into the Content Browser (or use **File → Import Into Level / Asset**).
2. A Static Mesh and (if entities are present) a sibling DataTable are created in the destination folder.

By default every face will use the engine grey default material. To fix that, configure either the override JSON or the material-synthesis pipeline below.

---

## Settings

`Project Settings → Plugins → HL2 BSP Importer`. Stored in `Config/DefaultHL2BSPImporter.ini`.

### Coordinate system

| Setting       | Default | Notes |
|---------------|---------|-------|
| `WorldScale`  | `2.54`  | Source units (inches) → Unreal centimetres. |
| `bFlipYZ`     | `true`  | Swap Y/Z, then negate Y to convert Source's right-handed Z-up to Unreal's left-handed Z-up. Triangle winding is reversed automatically. |

### Mesh build

| Setting              | Default | Notes |
|----------------------|---------|-------|
| `bBuildNanite`       | `true`  | Sets `NaniteSettings.bEnabled` before `Build()`. |
| `bImportCollision`   | `true`  | Adds a `BodySetup` with `CTF_UseComplexAsSimple`. |
| `bImportPropsAsInstances` | `false` | Reserved — `prop_static` instancing is not implemented yet. |

### Materials — explicit override map

Use this when you want to point a Source texture at a hand-authored UE material.

| Setting             | Default | Notes |
|---------------------|---------|-------|
| `MaterialJsonPath`  | _empty_ | Empty falls back to `HL2BSPImporter/Resources/Materials.json`. May be a `/Game/...` path or an absolute filesystem path. |

```json
[
  { "TextureName": "concrete/concretefloor028a",
    "MaterialPath": "/Game/Materials/Concrete/M_ConcreteFloor_028a" }
]
```

`TextureName` matching is case-insensitive and slash-normalised. Anything not found here falls through to material synthesis.

### Materials — synthesis from VMT/VTF

| Setting                        | Default                | Notes |
|--------------------------------|------------------------|-------|
| `bSynthesizeMaterials`         | `true`                 | Master switch. |
| `SourceContentRoots`           | _empty_                | One or more directories containing a `materials/` subtree. Searched in order. |
| `SynthesizedAssetRoot`         | `/Game/HL2/Imported`   | `/Game/`-rooted package path for created textures + MICs. |
| `ParentMaterial_LightmappedGeneric`             | _empty_ | `UMaterial` (or `UMaterialInstance`) used as parent for opaque world brushes. |
| `ParentMaterial_LightmappedGeneric_Masked`      | _empty_ | Used when the VMT has `$alphatest 1`. |
| `ParentMaterial_LightmappedGeneric_Translucent` | _empty_ | Used when the VMT has `$translucent 1` or `$alpha`. |
| `ParentMaterial_WorldVertexTransition`          | _empty_ | Two-texture displacement blends (`$basetexture` ↔ `$basetexture2`, alpha from vertex color). |
| `ParentMaterial_VertexLitGeneric`               | _empty_ | Reserved for future prop importer. |
| `ParentMaterial_UnlitGeneric`                   | _empty_ | UI / decals / unlit detail. |

You author the parent materials in your project. The plugin reads from them by parameter name; see [`Resources/MasterMaterials/README.md`](HL2BSPImporter/Resources/MasterMaterials/README.md) for the parameter contract (`BaseColor`, `BaseColor2`, `Normal`, `Detail`, `AlphaTestRef`, `DetailScale`).

For each unmapped face slot the importer:

1. Locates `<root>/materials/<key>.vmt` under the configured roots.
2. Parses it and resolves any `Patch { include … replace { … } insert { … } }` indirection.
3. Decodes `$basetexture` / `$basetexture2` / `$bumpmap` / `$detail` `.vtf` files (DXT1/3/5, BGRA/BGRX/RGBA/ABGR/ARGB8888, BGR/RGB888, I8/A8/IA88; mip 0; cubemaps and volumes get face/slice 0) into `UTexture2D` assets.
4. Picks the parent material slot that matches the VMT shader and creates a `UMaterialInstanceConstant` next to the textures.

Per-import caches dedupe textures and MICs across slots; previously imported assets at the destination paths are reused, not overwritten.

---

## Output assets

For `mapname.bsp` imported into `/Game/HL2/Maps/`:

```
/Game/HL2/Maps/
  mapname                 (UStaticMesh)
  mapname_Entities        (UDataTable<FHL2Entity>) — only if the map has entities
/Game/HL2/Imported/       (configurable)
  Textures/<source/path>/<name>   (UTexture2D, one per unique .vtf)
  Materials/<source/path>/<name>  (UMaterialInstanceConstant, one per unique .vmt)
```

`FHL2Entity` rows expose `Name` (`targetname`), `Class`, `Origin`, `Rotation`, and `Model` and are `EditAnywhere / BlueprintReadWrite` so they're usable from Blueprints and editor scripts.

---

## How materials get resolved (decision tree)

```
For each polygon-group slot (= Source texture name, lower-case, '/' separators):
  1. JSON map hit?                          → use that UMaterialInterface
  2. bSynthesizeMaterials && .vmt found?    → synthesize a UMaterialInstanceConstant
       a. shader → parent material slot
       b. parse $basetexture / $basetexture2 / $bumpmap / $detail
       c. decode .vtf → UTexture2D, set MIC texture parameters
       d. set $alphatestreference, $detailscale if present
  3. otherwise                              → engine default surface material
```

Synth failures (no `.vmt` found, parse error, no parent configured for the shader, unsupported VTF format) fall through to the engine default and are logged at `Verbose` (or `Warning` for actual decode/parse failures).

---

## Diagnostics

- All log output goes to the `LogHL2BSPImporter` category. Filter by it in the Output Log.
- A `FScopedSlowTask` dialog reports the six import phases (Validate → Parse → Materials → Mesh build → StaticMesh build → Entities).
- After a successful import you'll see one summary line per major step, e.g.:
  - `BSP build: Faces=… Disps=… SkippedDisps=… V=… VI=… T=… PG=… Slots=…`
  - `Material assignment: explicit=… synthesized=… (created/cached/failed, tex created/cached/failed) default=…`
  - `StaticMesh built. LODs=… Materials=…`
- Common failure modes (file missing, `.bz2` compressed input, unsupported VBSP version, LZMA-compressed lumps, bad lump bounds) emit a single targeted error with enough context to act on.

---

## Limitations

Out of scope for the current code, in roughly the order they're likely to be tackled:

- Static prop instancing (`sprp` GameLump, MDL/VVD/VTX importer).
- Pakfile lump (40) extraction for community maps that embed assets.
- LZMA-compressed lump decompression (CS:GO / L4D2 / Workshop).
- `$envmap` cubemap synthesis; DXT5nm / RGTC normal recombine.
- Animated VMT proxies.
- Brush-entity sub-meshes (`func_door`, `func_brush`, …) attached to entity actors.
- Topology-aware displacement stitching for power-mismatched neighbours (cliff seams where one side is higher subdivision than the other). Same-power seam welding via spatial clustering ships in Phase 2b.
- HDR lightmap baking, light-style data, 3D skybox composition.
- Entity I/O graph parsing (the parser keeps last-wins values for duplicate keys).
- Big-endian (Xbox 360 / PS3) BSPs.
- `FReimportHandler` support / Interchange-framework refactor.

---

## Troubleshooting

**Import fails with "Unsupported VBSP version" or "Compressed lump"** — the map is from a post-HL2 game. The reader explicitly only accepts v19/v20 to avoid silently producing garbage geometry.

**Static mesh imports but every face is grey** — neither the JSON map nor synthesis produced a material. Check the `Material assignment:` log line. If `explicit=0` and `synthesized=0`, either `SourceContentRoots` is unset, no parent materials are configured, or the relevant `.vmt` files aren't under those roots.

**Synthesis creates textures but materials are grey** — your master material doesn't expose `BaseColor` (etc.) as a `Texture2D` parameter. See [`Resources/MasterMaterials/README.md`](HL2BSPImporter/Resources/MasterMaterials/README.md).

**Faces appear inside-out** — should not happen with current code (winding is reversed automatically), but if you've forked the coordinate transform, recheck `ShouldReverseWinding`.

**Wrong scale** — adjust `WorldScale`. Default is inches→cm.

**No entity DataTable** — the map has no entity lump or all entities were stripped. Check the parse summary in the log.

---

## Layout

```
HL2BSPImporter/
├─ HL2BSPImporter.uplugin
├─ Config/
│  └─ DefaultHL2BSPImporter.ini
├─ Resources/
│  ├─ Materials.json                    (fallback explicit-override map)
│  └─ MasterMaterials/README.md         (parent-material parameter contract)
└─ Source/HL2BSPImporter/
   ├─ HL2BSPImporter.Build.cs
   ├─ Public/
   │  ├─ HL2BSPImporter.h               module + log category
   │  ├─ HL2BSPImporterFactory.h        UFactory entry point
   │  ├─ HL2BSPImporterSettings.h       UDeveloperSettings
   │  ├─ HL2BSPImporterTypes.h          shared structs
   │  ├─ BspFile.h                      VBSP reader
   │  ├─ HL2EntityTable.h               entity DataTable
   │  ├─ HL2VmtParser.h                 KeyValues / Patch parser
   │  ├─ HL2VtfReader.h                 VTF header + decoders
   │  └─ HL2MaterialBuilder.h           texture + MIC synthesis
   └─ Private/                          (matching .cpp files)
```

---

## Versioning

Notable changes — including the four-phase parser/builder/factory hardening pass and the Phase 5 material synthesis work — are recorded in [`CHANGELOG.md`](CHANGELOG.md).

---

## License

See `LICENSE` (add one if missing).

## Credits

Created by TzeentchNET. UE 5.6 hardening, material synthesis, and packaging maintained in this fork.
