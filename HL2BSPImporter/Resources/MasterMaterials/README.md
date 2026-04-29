# HL2 Importer – Master Material Contract

As of **Phase 11a** the plugin ships authored `M_HL2_*` master materials under
`/HL2BSPImporter/MasterMaterials/` (the plugin's own Content folder, mounted
automatically because `HL2BSPImporter.uplugin` sets `"CanContainContent": true`).
A fresh install therefore needs no master-material setup — `.vmt` files found
under your configured `SourceContentRoots` are turned into
`UMaterialInstanceConstant` assets parented to those shipped masters.

To **override** any slot per project, point the corresponding entry under
**Project Settings → HL2 BSP Importer → Materials | Synthesis | Parents** at
your own `UMaterialInterface` asset. Any parameter listed below that your
custom parent does not declare is silently skipped at MIC build time, so
overrides only need to expose the parameters they actually use.

## Parameter contract

The shipped masters declare the parameters below; the importer writes each one
only when the `.vmt` carries the corresponding key.

### Phase 5 / 5b parameters (current)

| Parameter name  | Type    | VMT key                        | Notes |
|-----------------|---------|--------------------------------|-------|
| `BaseColor`     | Texture | `$basetexture`                 | sRGB |
| `BaseColor2`    | Texture | `$basetexture2`                | sRGB · WorldVertexTransition |
| `Normal`        | Texture | `$bumpmap`                     | `TC_Normalmap`, sRGB=false |
| `Normal2`       | Texture | `$bumpmap2`                    | `TC_Normalmap`, sRGB=false · WorldVertexTransition |
| `NormalAlpha`   | Texture | `$bumpmap` (alpha channel)     | Sibling `<basename>_a` `TC_Grayscale` mask, sRGB=false. Bound only when source VTF carries non-uniform alpha. Master should route through `$normalmapalphaenvmapmask` / `$basemapalphaphongmask` semantics — typically the phong/envmap specular mask. |
| `Normal2Alpha`  | Texture | `$bumpmap2` (alpha channel)    | Sibling `<basename>_a` `TC_Grayscale` mask, sRGB=false. WorldVertexTransition counterpart of `NormalAlpha`. |
| `Detail`        | Texture | `$detail`                      | |
| `BlendModulate` | Texture | `$blendmodulatetexture`        | R = blend midpoint, G = blend softness |
| `AlphaTestRef`  | Scalar  | `$alphatestreference`          | default 0.5 |
| `DetailScale`   | Scalar  | `$detailscale`                 | default 1.0 |

### Phase 11b parameters (extended)

| Parameter name        | Type        | VMT key(s)                                   | Notes |
|-----------------------|-------------|----------------------------------------------|-------|
| `EmissiveColor`       | Texture     | `$selfillummask`                             | sRGB; falls back to `BaseColor` alpha when mask absent |
| `EmissiveTint`        | Vector      | `$selfillumtint`                             | default `(1,1,1,1)` |
| `EmissiveStrength`    | Scalar      | `$selfillum` flag                            | 0 or 1 |
| `BaseColorTint`       | Vector      | `$color`                                     | default `(1,1,1,1)` |
| `BaseColor2Tint`      | Vector      | `$color2`                                    | default `(1,1,1,1)` |
| `OpacityScalar`       | Scalar      | `$alpha`                                     | default 1.0; only meaningful on translucent parents |
| `Cubemap`             | TextureCube | `$envmap` (when not the literal `env_cubemap`) | per Phase 11c investigation; default neutral grey |
| `EnvmapTint`          | Vector      | `$envmaptint`                                | default `(1,1,1,1)` |
| `Phong`               | Scalar      | `$phong` flag                                | 0 or 1 |
| `PhongExponent`       | Scalar      | `$phongexponent`                             | default 5.0 |
| `PhongBoost`          | Scalar      | `$phongboost`                                | default 1.0 |
| `BaseColorUVScale`    | Vector      | `$basetexturetransform` `scale`              | default `(1,1,0,0)` (xy) |
| `BaseColorUVOffset`   | Vector      | `$basetexturetransform` `translate`          | default `(0,0,0,0)` (xy) |
| `BaseColorUVRotate`   | Scalar      | `$basetexturetransform` `rotate`             | degrees · default 0 |
| `NormalUVScale`       | Vector      | `$bumptransform` `scale`                     | default `(1,1,0,0)` |
| `NormalUVOffset`      | Vector      | `$bumptransform` `translate`                 | default `(0,0,0,0)` |
| `NormalUVRotate`      | Scalar      | `$bumptransform` `rotate`                    | default 0 |
| `DetailBlendMode`     | Scalar      | `$detailblendmode`                           | 0..12, see Source detail blend modes |
| `DetailBlendFactor`   | Scalar      | `$detailblendfactor`                         | default 1.0 |

## Shader → parent slot mapping

| Source shader (lower-case)         | Settings slot                                    | Suggested asset       |
|------------------------------------|--------------------------------------------------|-----------------------|
| `lightmappedgeneric` (opaque)      | `ParentMaterial_LightmappedGeneric`              | `M_HL2_Lit`           |
| `lightmappedgeneric` (`$alphatest`)| `ParentMaterial_LightmappedGeneric_Masked`       | `M_HL2_LitMasked`     |
| `lightmappedgeneric` (`$translucent`/`$alpha`) | `ParentMaterial_LightmappedGeneric_Translucent` | `M_HL2_LitTranslucent` |
| `lightmappedgeneric_decalgroup`    | `ParentMaterial_LightmappedGeneric_Decal`        | `M_HL2_LitDecal`      |
| `worldvertextransition`            | `ParentMaterial_WorldVertexTransition`           | `M_HL2_WorldVertexBlend` |
| `vertexlitgeneric`                 | `ParentMaterial_VertexLitGeneric`                | `M_HL2_VertexLit`     |
| `unlitgeneric`                     | `ParentMaterial_UnlitGeneric`                    | `M_HL2_Unlit`         |

Unknown shader names fall through to `LightmappedGeneric` (opaque/masked/translucent
selected by `$alphatest` / `$translucent` flags).

## WorldVertexTransition graph

`M_HL2_WorldVertexBlend` is expected to compute its blend mask as:

```
edge   = BlendModulate.r          // remapped blend midpoint
soft   = BlendModulate.g * 0.5    // half-width of the soft edge
mask   = saturate( (VertexColor.a - (edge - soft)) / max(soft * 2.0, 1e-4) )
albedo = lerp(BaseColor * BaseColorTint, BaseColor2 * BaseColor2Tint, mask)
normal = lerp(Normal,    Normal2,    mask)
```

`Normal2` and `BlendModulate` are optional — when their textures are not bound
the master should fall back to `Normal` and the raw `VertexColor.a` respectively.

## Lighting

The shipped masters target **Lumen-friendly Lit** shading; they do not consume
a baked lightmap texture. The static-mesh build still emits lightmap UVs into
channel 1 (`SrcLightmapIndex / DstLightmapIndex / MinLightmapResolution = 128`)
so a future `LUMP_LIGHTING` phase can light the geometry without re-importing.

