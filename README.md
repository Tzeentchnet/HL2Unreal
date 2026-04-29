# HL2 BSP Importer

A Half-Life 2 / Source Engine `.bsp` map importer for **Unreal Engine 5.7**. Drops a Source map onto Unreal as a worldspawn `UStaticMesh` plus a sibling set of brush sub-meshes, prop meshes, and entity / static-prop DataTables, optionally synthesizing materials and textures from the original `.vmt` / `.vtf` files and extracting any assets embedded in the map's pakfile lump.

> Status: editor-only plugin, opinionated import path. Validated on HL2 / EP1 / EP2 / CS:S maps (VBSP v19 / v20). LZMA-compressed lumps and embedded pakfiles (STORE + DEFLATE) are handled, so HL2-era community maps and Source SDK 2013 output that previously read as "compressed lump" no longer fail. CS:GO / L4D2 (v21+) and big-endian (Xbox 360 / PS3) BSPs are still rejected at the version gate with a clear error.

---

## What it does

For a given `mapname.bsp`, the importer:

1. Validates the file (extension, magic, version, lump bounds, sane sizes). v21+ and big-endian (`'PSBV'`) headers are rejected with a targeted error rather than producing garbage.
2. Decompresses any per-lump LZMA payloads in place (whole-lump on `lump_t.fourCC == 'LZMA'`, and per-sub-lump on the `sprp` GameLump entry when its `flags & 0x1` is set).
3. Extracts the embedded pakfile (`LUMP_PAKFILE`, 40) into a per-import temp directory under `<Project>/Intermediate/HL2BSPImporter/Pak/<map>_<guid>/`. Supports STORE (method 0) and raw DEFLATE (method 8) ZIP entries; encrypted entries are reported and skipped. The extract dir is added as the highest-priority content root passed to the material builder.
4. Parses the lumps it needs: vertices, edges, surfedges, faces, texinfo, texdata + string table, models (worldspawn + every brush sub-model), dispinfo, dispverts, entities, and the `sprp` static-prop GameLump.
5. Builds one `FMeshDescription` per emitted mesh — the worldspawn brush faces + displacement grids, plus one sub-mesh per brush entity (`func_door`, `func_brush`, `func_water`, renderable `trigger_*`, …) pivoted around its entity origin so spawning at `origin` is correct. Each mesh has:
   - Per-face polygon groups keyed by Source texture name.
   - Welded vertex instances per polygon (no fan-triangulation duplication).
   - Two UV channels — Source-projected UV0 and lightmap UV1.
   - Vertex colors carrying displacement `m_Alpha` for `WorldVertexTransition` blends.
   - Cross-displacement seam stitching (Phase 2b spatial-cluster centroid weld + Phase 2c geometric T-junction snap for power-mismatched neighbours).
6. Resolves a material for each polygon group, in this order:
   - Explicit JSON map (`MaterialJsonPath`).
   - Synthesized `UMaterialInstanceConstant` parented to a project-supplied master material, generated from the corresponding `.vmt` and `.vtf` files.
   - Engine default surface material as a final fallback.
7. Builds each `UStaticMesh` via the editor source-model path (so Nanite settings, MikkTSpace tangents, and auto-lightmap UV generation actually take effect), and optionally adds Complex-As-Simple collision.
8. Walks the entity lump and emits `<Map>_Entities` (a `UDataTable` of `FHL2Entity` rows). Source I/O `On*` outputs are preserved as a `TArray<FHL2EntityIO>` per row instead of being collapsed by the per-entity keyvalue map; remaining non-structured keyvalues are retained on each row for editor utilities.
9. Walks the `sprp` GameLump and emits `<Map>_StaticProps` (a `UDataTable` of `FHL2StaticProp` rows) carrying per-instance origin / rotation / scale / skin / model in Unreal coordinates.
10. *(Optional, gated by `bImportStaticPropMeshes`)* Synthesises one `UStaticMesh` per unique `(model, skin, body)` referenced by the static-prop instance table or by `prop_*` point-entity rows, by parsing the corresponding `.mdl / .vvd / .dx90.vtx` triple (`.vtx` and `.dx80.vtx` are fallback vertex-index names). Each `FHL2StaticProp::StaticMeshAsset` and `FHL2Entity::PropMesh` is back-assigned to the produced asset.

What it deliberately does **not** do (yet) is listed under [Limitations](#limitations).

---

## Install

1. Copy the `HL2BSPImporter` folder into your project's `Plugins/` directory.
2. Right-click the `.uproject` → **Generate Visual Studio project files**.
3. Build and open the project. The plugin auto-enables.

The module is gated to Editor builds via `WhitelistTargets` in the `.uplugin`, so it adds no runtime cost to packaged games. The plugin sets `"CanContainContent": true` so its own `Content/MasterMaterials/` folder mounts at `/HL2BSPImporter/MasterMaterials/`.

---

## Quick start

1. Drag a `.bsp` file into the Content Browser (or use **File → Import Into Level / Asset**).
2. The factory creates, in the destination folder:
   - `<MapName>` — worldspawn `UStaticMesh`.
   - `<MapName>_BModel_<N>` — one `UStaticMesh` per brush sub-model that has renderable faces (only created when the BSP declares more than the worldspawn model).
   - `<MapName>_Entities` — `UDataTable<FHL2Entity>` (only when the map has entities).
   - `<MapName>_StaticProps` — `UDataTable<FHL2StaticProp>` (only when the `sprp` GameLump has any entries).
3. Material synthesis is on by default but requires either (a) the in-plugin master materials to be authored — see [Master materials](#master-materials) — or (b) project-supplied parents pointed at via the settings. Until parents resolve, every face will use the engine default surface material.
4. To materialise the imported tables into level actors, call `UHL2LevelSpawnLibrary::SpawnImportedActors` from an Editor Utility Blueprint / Widget and pass the generated `_Entities` and `_StaticProps` tables. The helper spawns brush meshes, entity-prop meshes, static props, and optional basic light actors into editor folders under `HL2Imported`.
5. Imported worldspawn meshes store their `.bsp` source path, so Content Browser right-click → **Reimport** reruns the factory against the original file.

---

## Settings

`Project Settings → Plugins → HL2 BSP Importer`. Stored in `Config/DefaultHL2BSPImporter.ini`.

### Coordinate system

| Setting       | Default | Notes |
|---------------|---------|-------|
| `WorldScale`  | `2.54`  | Source units (inches) → Unreal centimetres. |
| `bFlipYZ`     | `true`  | Swap Y/Z, then negate Y to convert Source's right-handed Z-up to Unreal's left-handed Z-up. Triangle winding is reversed automatically. |

### Mesh build

| Setting            | Default | Notes |
|--------------------|---------|-------|
| `bBuildNanite`     | `true`  | Sets `NaniteSettings.bEnabled` before `Build()`. Applied to worldspawn, brush sub-meshes, and synthesised prop meshes. |
| `bImportCollision` | `true`  | Adds a `BodySetup` with `CTF_UseComplexAsSimple`. |

### Displacements

| Setting                          | Default | Notes |
|----------------------------------|---------|-------|
| `bStitchDisplacementSeams`       | `true`  | Welds coincident perimeter vertices across neighbouring displacement grids (spatial-hash centroid pass) and snaps lonely high-power perimeter vertices onto the closest low-power neighbour edge sub-segment. Eliminates hairline T-junction cracks on terrain seams that weren't perfectly "Sew"-ed in Hammer. |
| `DisplacementSeamWeldDistance`   | `1.0`   | Weld distance in Unreal cm. Clamp 0.0001..16. The default covers float-precision noise on a 2.54 cm/inch map without distorting intentionally-separated edges. |

### Props

| Setting                     | Default | Notes |
|-----------------------------|---------|-------|
| `bImportPropsAsInstances`   | `false` | Reserved — direct ISM/HISM emission for `prop_static` is not implemented yet (planned to be subsumed by the Phase 13 PCG point-data path). The instance table is always emitted as a DataTable regardless. |
| `bImportStaticPropMeshes`   | `false` | When `true` (and `bSynthesizeMaterials` is on), synthesise one `UStaticMesh` per unique `(model, skin, body)` referenced by the `sprp` instance table or by `prop_*` entity rows. Requires `SourceContentRoots` to point at directories where the `.mdl/.vvd/.dx90.vtx` triples can be located (`.vtx` and `.dx80.vtx` are tried as fallbacks). Default off until validated against more community maps; per-model failures leave the row's mesh field empty. |

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

| Setting                                          | Default                                                     | Notes |
|--------------------------------------------------|-------------------------------------------------------------|-------|
| `bSynthesizeMaterials`                           | `true`                                                      | Master switch. |
| `SourceContentRoots`                             | _empty_                                                     | One or more directories containing a `materials/` subtree (and `models/` subtree if `bImportStaticPropMeshes` is on). Searched in order, after the per-import pakfile extract dir. |
| `SynthesizedAssetRoot`                           | `/Game/HL2/Imported`                                        | `/Game/`-rooted package path for created textures, MICs, and prop meshes. |
| `ParentMaterial_LightmappedGeneric`              | `/HL2BSPImporter/MasterMaterials/M_HL2_Lit`                 | Opaque world brushes. |
| `ParentMaterial_LightmappedGeneric_Masked`       | `/HL2BSPImporter/MasterMaterials/M_HL2_LitMasked`           | When the VMT has `$alphatest 1`. |
| `ParentMaterial_LightmappedGeneric_Translucent`  | `/HL2BSPImporter/MasterMaterials/M_HL2_LitTranslucent`      | When the VMT has `$translucent 1` or `$alpha`. |
| `ParentMaterial_LightmappedGeneric_Decal`        | `/HL2BSPImporter/MasterMaterials/M_HL2_LitDecal`            | Source decal family (`LightmappedGeneric_DecalGroup`, `decalmodulate`); typically a Deferred Decal-domain parent. |
| `ParentMaterial_WorldVertexTransition`           | `/HL2BSPImporter/MasterMaterials/M_HL2_WorldVertexBlend`    | Two-texture displacement blends (`$basetexture` ↔ `$basetexture2`, alpha from vertex color). |
| `ParentMaterial_VertexLitGeneric`                | `/HL2BSPImporter/MasterMaterials/M_HL2_VertexLit`           | Used by synthesised prop meshes. |
| `ParentMaterial_UnlitGeneric`                    | `/HL2BSPImporter/MasterMaterials/M_HL2_Unlit`               | UI / decals / unlit detail. |

### Master materials

The `ParentMaterial_*` defaults point at assets that ship in the plugin's mounted content folder (`/HL2BSPImporter/MasterMaterials/`). **Authoring of those `.uasset` masters is still in progress** — the slots, parameter contract, settings UI, and MIC binding code all ship today, but until the seven master assets are saved into `HL2BSPImporter/Content/MasterMaterials/` the synthesis pass logs `No parent material configured for shader '<x>'` and falls through to the engine default. Two ways forward:

- **Author your own.** Point any `ParentMaterial_*` slot at your own `UMaterialInterface`. The parameter contract is documented in [`HL2BSPImporter/Resources/MasterMaterials/README.md`](HL2BSPImporter/Resources/MasterMaterials/README.md). Parameters not exposed by your parent are silently skipped.
- **Use the in-plugin authoring guide.** Step-by-step instructions for building the seven shipped masters against the documented contract live in [`HL2BSPImporter/Content/MasterMaterials/MaterialBuilderInstructions.md`](HL2BSPImporter/Content/MasterMaterials/MaterialBuilderInstructions.md). Tracked under `Content/MasterMaterials/PLACEHOLDER.md`.

For each unmapped face slot the importer:

1. Locates `<root>/materials/<key>.vmt` under the configured roots (pakfile extract dir first).
2. Parses it and resolves any `Patch { include … replace { … } insert { … } }` indirection.
3. Decodes referenced `.vtf` files (DXT1/3/5, BGRA/BGRX/RGBA/ABGR/ARGB8888, BGR/RGB888, I8/A8/IA88; mip 0; cubemaps and volumes get face/slice 0) into `UTexture2D` assets.
4. Picks the parent material slot that matches the VMT shader and creates a `UMaterialInstanceConstant` next to the textures, binding the full Phase 5 / 5b / 11b parameter set documented below.

Per-import caches dedupe textures and MICs across slots; previously imported assets at the destination paths are reused, not overwritten.

---

## Output assets

For `mapname.bsp` imported into `/Game/HL2/Maps/`:

```
/Game/HL2/Maps/
  mapname                          (UStaticMesh — worldspawn)
  mapname_BModel_<N>               (UStaticMesh — one per brush sub-model with renderable faces)
  mapname_Entities                 (UDataTable<FHL2Entity>)
  mapname_StaticProps              (UDataTable<FHL2StaticProp>)
/Game/HL2/Imported/                (configurable via SynthesizedAssetRoot)
  Textures/<source/path>/<name>             (UTexture2D, one per unique .vtf)
  Materials/<source/path>/<name>            (UMaterialInstanceConstant, one per unique .vmt)
  Props/<source/path>/<model>(_skin{N})?(_bg{M})?  (UStaticMesh, one per unique (model, skin, body))
```

Row contracts:

- `FHL2Entity` exposes `Name` (`targetname`), `Class`, `Origin`, `Rotation`, `Model`, `Skin`, `BodyGroup`, `BrushMesh` (`FSoftObjectPath` for brush sub-meshes), `PropMesh` (`FSoftObjectPath` for synthesised prop meshes), `Outputs` (`TArray<FHL2EntityIO>` carrying parsed Source I/O connections — one row per `On*` keyvalue, split on the modern ESC (`0x1B`) or HL2-era comma separator), and `KeyValues` (remaining non-structured entity keyvalues, last-wins for duplicate non-output keys). All fields are `EditAnywhere / BlueprintReadWrite`.
- `FHL2EntityIO` exposes `OutputName`, `TargetName`, `InputName`, `Parameter`, `Delay`, `TimesToFire` (-1 = infinite).
- `FHL2StaticProp` exposes `ModelName`, `Origin`, `Rotation`, `UniformScale`, `Skin`, `Solid`, and `StaticMeshAsset` (populated when `bImportStaticPropMeshes` resolves the model). Origin / Rotation are stored in **Unreal coordinates** so a Blueprint can spawn `AStaticMeshActor`s straight from the table.

---

## How materials get resolved (decision tree)

```
For each polygon-group slot (= Source texture name, lower-case, '/' separators):
  1. JSON map hit?                          → use that UMaterialInterface
  2. bSynthesizeMaterials && .vmt found?    → synthesize a UMaterialInstanceConstant
       a. shader → parent material slot:
            decal*                    → ParentMaterial_LightmappedGeneric_Decal
            *Translucent / $alpha     → ParentMaterial_LightmappedGeneric_Translucent
            $alphatest                → ParentMaterial_LightmappedGeneric_Masked
            WorldVertexTransition     → ParentMaterial_WorldVertexTransition
            VertexLitGeneric          → ParentMaterial_VertexLitGeneric
            UnlitGeneric              → ParentMaterial_UnlitGeneric
            otherwise                 → ParentMaterial_LightmappedGeneric
       b. parse + decode textures:
            $basetexture, $basetexture2  → BaseColor, BaseColor2
            $bumpmap, $bumpmap2          → Normal, Normal2 (TC_Normalmap, sRGB=false)
            $detail, $blendmodulatetexture → Detail, BlendModulate
            $selfillummask               → EmissiveColor
       c. set scalar / vector parameters when present:
            $alphatestreference          → AlphaTestRef
            $detailscale                 → DetailScale
            $detailblendmode/factor      → DetailBlendMode / DetailBlendFactor
            $alpha                       → OpacityScalar
            $selfillum + $selfillumtint  → EmissiveStrength + EmissiveTint
            $phong / $phongexponent / $phongboost → Phong / PhongExponent / PhongBoost
            $color, $color2              → BaseColorTint, BaseColor2Tint
            $envmaptint                  → EnvmapTint
            $basetexturetransform        → BaseColorUVScale / BaseColorUVOffset / BaseColorUVRotate
            $bumptransform               → NormalUVScale / NormalUVOffset / NormalUVRotate
  3. otherwise                              → engine default surface material
```

Synth failures (no `.vmt` found, parse error, no parent configured for the shader, unsupported VTF format) fall through to the engine default and are logged at `Verbose` (or `Warning` for actual decode/parse failures).

---

## How props get resolved (when `bImportStaticPropMeshes = true`)

```
Sources:
  - sprp GameLump entries  → (model,        skin, body=0)
  - prop_* entity rows     → (entity.Model, entity.Skin, entity.BodyGroup)
                             skipped when Model starts with '*' (brush ref)

For each unique key:
  1. Resolve <root>/<model>.mdl + .vvd + .dx90.vtx under content roots
     (falling back to .vtx, then .dx80.vtx)
     (pakfile extract dir first, then SourceContentRoots in order).
  2. Cross-validate studio checksum / bodypart count / per-model mesh count.
  3. Walk LOD 0:
        - VVD: position/normal/UV/tangent (post-fixup-table) at stride 48 + 16
        - VTX: BodyPart → Model → ModelLOD → Mesh → StripGroup → Strip
               (LIST + TRISTRIP both honoured)
        - MDL: per-bodypart Models[(body / base[bp]) % count] is selected;
               per-mesh material ref-slot remapped through SkinFamilies[skin]
  4. Build one UStaticMesh under
        <SynthesizedAssetRoot>/Props/<model>(_skin{N})?(_bg{M})?
     using the same editor source-model + Nanite + MikkTSpace path as the
     worldspawn mesh, sharing the HL2Mat::FBuilder material cache.
  5. Back-assign FHL2StaticProp::StaticMeshAsset / FHL2Entity::PropMesh.
```

Re-imports re-use existing per-key assets at the destination path instead of overwriting them.

---

## Spawning actors from tables

The importer remains asset-first: a BSP import produces meshes and DataTables, then `UHL2LevelSpawnLibrary::SpawnImportedActors` can place those rows into the active editor world when you want a concrete level layout.

`FHL2LevelSpawnOptions` controls which row families are spawned:

- `bSpawnStaticProps` — reads `_StaticProps` rows whose `StaticMeshAsset` resolved during the prop synthesis pass.
- `bSpawnBrushEntities` — reads `_Entities` rows whose `BrushMesh` points at a generated `_BModel_<N>` mesh.
- `bSpawnEntityProps` — reads `_Entities` rows whose `PropMesh` points at a synthesized point-entity prop mesh.
- `bSpawnBasicLights` — optionally creates `PointLight`, `SpotLight`, or `DirectionalLight` actors for `light`, `light_spot`, and `light_environment` rows, applying `_light` / `_lightHDR` colour and brightness when those keyvalues are present. This is still a lightweight placement pass, not a full Source lighting conversion with attenuation, styles, or MegaLights tuning.
- `FolderRoot` / `bSelectSpawnedActors` — controls editor organisation and selection after spawn.

The returned `FHL2LevelSpawnResult` reports spawned counts for static props, brush entities, entity props, lights, skipped rows, and the actor list. The operation is wrapped in a single editor transaction, so Undo removes the spawned actors.

---

## Diagnostics

- All log output goes to the `LogHL2BSPImporter` category. Filter by it in the Output Log.
- A `FScopedSlowTask` dialog reports the import phases (Validate → Parse → Pakfile → Materials → Mesh build → Props → Entities).
- After a successful import you'll see one summary line per major step, e.g.:
  - `BSP build: Faces=… Disps=… SkippedDisps=… V=… VI=… T=… PG=… Slots=… SeamClusters=… SeamVertsWelded=… DispEdgeSnapsPowerMismatch=…`
  - `Brush sub-models: declared=… meshes-built=…`
  - `Material assignment: explicit=… synthesized=… (created/cached/failed, tex created/cached/failed) default=…`
  - `Material shaders: lit=… masked=… translucent=… wvt=… vlit=… unlit=… decal=… other=…`
  - `Static props: dict=… entries=… v=… stride=… badDictRefs=…`
  - `Static prop meshes: built=… cached=… failed=… (unique=… instances=…)`
  - `Entity prop meshes: built=… cached=… failed=… (unique=… candidates=…)`
  - `Entities: N parsed, M I/O outputs.`
  - `StaticMesh built. LODs=… Materials=…`
- Common failure modes (file missing, `.bz2` compressed input, unsupported VBSP version, bad lump bounds, missing master material) emit a single targeted error or warning with enough context to act on.

---

## Limitations

Out of scope for the current code, in roughly the order they're likely to be tackled (full forward-looking detail in [ROADMAP.md](ROADMAP.md)):

- **Master-material `.uassets`** — the seven shipped slots, parameter contract, and settings defaults are in place; the actual `.uasset` files under `HL2BSPImporter/Content/MasterMaterials/` are still being authored. Until they're saved, point each `ParentMaterial_*` slot at your own asset.
- **Animated / skeletal prop synthesis** — `prop_dynamic` with non-trivial `DefaultAnim`, per-instance physics bodies for `prop_physics`, and the underlying MDL bone hierarchy / `mstudioseqdesc_t` / `mstudioanimdesc_t` parsing.
- **Encrypted ZIP entries in pakfiles** (rare; produced by `bspzip -encrypt`).
- **`func_water` → UE5 Water plugin** mapping and `$envmap` per-leaf cubemap baking from `LUMP_CUBEMAPS` (Phase 11c investigations — only `EnvmapTint` is bound today; `Cubemap` falls back to the master-material default).
- **Animated VMT proxies** (`Proxies { TextureScroll, Pulse, … }`) as runtime-dynamic parameters.
- **HDR lightmap baking, light-style data, 3D skybox composition.**
- **Interchange / partial reimport refactor** — worldspawn `UStaticMesh` assets now support right-click → Reimport through `FReimportHandler`, but there is no Interchange translator yet, no per-import parameter override UI, and no partial mode for only rebuilding materials, tables, or generated sibling assets.
- **v21+ BSPs (CS:GO / L4D2 / Workshop)** — rejected at the version gate; v21 changes several lump struct sizes and bumps `dface_t`. Static-prop versions beyond v11 (CS:GO ships v10/v11; L4D2 ships v7/v10) similarly need a branch table.
- **Big-endian (Xbox 360 / PS3) BSPs** — `'PSBV'` magic is currently rejected with a clear error; would require byteswapping every header / lump-struct field on read.
- **Full World Partition / One-File-Per-Actor level import** — `UHL2LevelSpawnLibrary::SpawnImportedActors` can materialise the current DataTables into actors, but it does not yet create a dedicated `UWorld`, Data Layers, OFPA assets, HLOD layers, calibrated Source lighting, or runtime Source I/O behaviour. The larger Phase 13 route is tracked in [ROADMAP.md](ROADMAP.md).

---

## Troubleshooting

**Import fails with "Unsupported VBSP version"** — the map is from a post-HL2 game (CS:GO / L4D2 / TF2 / Source 2). The reader explicitly only accepts v19/v20 to avoid silently producing garbage geometry. v21 support is tracked in [ROADMAP.md](ROADMAP.md).

**Import fails with "big-endian BSP"** — the file is an Xbox 360 / PS3 build (`'PSBV'` magic). Not supported.

**Static mesh imports but every face is grey** — neither the JSON map nor synthesis produced a material. Check the `Material assignment:` log line. If `explicit=0` and `synthesized=0`, the most likely cause is one of (a) the in-plugin master `.uassets` haven't been authored yet (see [Master materials](#master-materials)), (b) `SourceContentRoots` is unset and the map ships no pakfile resources, or (c) the relevant `.vmt` files aren't reachable under those roots.

**Synthesis logs `No parent material configured for shader '<x>'`** — the `ParentMaterial_*` slot for that shader family is empty or its asset failed to load. Either author the in-plugin masters per the authoring guide, or repoint the slot at your own `UMaterialInterface` in `Project Settings → HL2 BSP Importer`.

**Synthesis creates textures but materials are grey** — your master material doesn't expose the parameters the binder writes. See [`HL2BSPImporter/Resources/MasterMaterials/README.md`](HL2BSPImporter/Resources/MasterMaterials/README.md) for the full contract.

**Brush entities are present in `_Entities` but no `_BModel_*` assets were created** — entities without a `model "*N"` keyvalue have no brush geometry by design (point entities, lights, logic nodes). Check the `Brush sub-models:` log line: `declared=N` is the number of `LUMP_MODELS` entries past worldspawn, `meshes-built=M` is the number that had renderable faces after surf-flag filtering.

**Static-prop instances are in `_StaticProps` but `StaticMeshAsset` is empty on every row** — `bImportStaticPropMeshes` is off (default), or it's on but the `.mdl/.vvd/.dx90.vtx` triples can't be located under `SourceContentRoots` / the pakfile (`.vtx` and `.dx80.vtx` are fallback names). Check the `Static prop meshes:` line — `failed=N` with `built=0` means lookups never succeeded; `built=0 cached=N` means the destination assets already existed from a prior import.

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
├─ Content/
│  └─ MasterMaterials/                  (mounted at /HL2BSPImporter/MasterMaterials/)
│     ├─ MaterialBuilderInstructions.md (authoring guide for the seven shipped masters)
│     └─ PLACEHOLDER.md
├─ Resources/
│  ├─ Materials.json                    (fallback explicit-override map)
│  └─ MasterMaterials/README.md         (parent-material parameter contract)
└─ Source/HL2BSPImporter/
   ├─ HL2BSPImporter.Build.cs
   ├─ Public/
   │  ├─ HL2BSPImporter.h               module + log category
   │  ├─ HL2BSPImporterFactory.h        UFactory entry point
   │  ├─ HL2BSPImporterSettings.h       UDeveloperSettings
   │  ├─ HL2BSPImporterTypes.h          shared structs (FHL2Entity, FHL2EntityIO, FHL2StaticProp)
    │  ├─ HL2LevelSpawnLibrary.h         editor helper: DataTables → actors
   │  ├─ BspFile.h                      VBSP reader (worldspawn + brush sub-models + sprp + entities)
   │  ├─ HL2EntityTable.h               entity DataTable
   │  ├─ HL2Lzma.h                      Source LZMA lump decoder
   │  ├─ HL2PakFile.h                   pakfile-lump (40) ZIP extractor (STORE + raw DEFLATE)
   │  ├─ HL2VmtParser.h                 KeyValues / Patch parser
   │  ├─ HL2VtfReader.h                 VTF header + decoders
   │  ├─ HL2MaterialBuilder.h           texture + MIC synthesis
   │  ├─ HL2MdlReader.h                 .mdl studio header + skin table parser
   │  ├─ HL2VvdReader.h                 .vvd vertex stream + fixup-table parser
   │  ├─ HL2VtxReader.h                 .vtx strip-group parser
   │  ├─ HL2StudioLoader.h              .mdl/.vvd/.vtx orchestrator
   │  ├─ HL2StudioTypes.h               studio loader output structs
   │  └─ HL2StaticPropMeshBuilder.h     studio → UStaticMesh
   └─ Private/                          (matching .cpp files, plus ThirdParty/Lzma/)
```

---

## Versioning

Notable changes are recorded in [`CHANGELOG.md`](CHANGELOG.md). Phases 1 → 12d have shipped, covering the parser/builder/factory hardening pass, material synthesis, LZMA + pakfile support, brush sub-meshes, static-prop instance table + mesh synthesis (including skin families and per-bodygroup variants), entity I/O graph, and the extended VMT parameter coverage.

Forward-looking work — open follow-ups, partial features, and the Phase 13 UE 5.7 modernization candidates (Nanite displacement, PCG point-data props, MegaLights pass, Interchange refactor, World Partition / OFPA emission, Substrate masters, async batched build, Texture Graph / TextureSet investigation) — lives in [`ROADMAP.md`](ROADMAP.md).

---

## License

This plugin's source code is released under the [MIT License](LICENSE).

## Asset rights disclaimer

The MIT license covers **only the importer source code in this repository**. It does **not** grant any rights to Half-Life 2, Source Engine, Valve-shipped assets, or any third-party content you point the importer at.

`.bsp`, `.vmt`, `.vtf`, `.mdl`, `.vvd`, `.vtx`, `.wav`, `.mp3`, embedded pakfile payloads, soundscapes, and any other game content are owned by their respective copyright holders (Valve Corporation and/or third-party authors). You are solely responsible for ensuring you have the legal right to extract, convert, redistribute, or use any asset processed by this tool.

In particular:

- Valve's HL2 / EP1 / EP2 / CS:S / Portal / etc. content is licensed to end users under the Steam Subscriber Agreement and the Source SDK / Authoring Tools license — neither permits redistribution of converted assets or shipping them inside a non-Source product without explicit permission from Valve.
- Community / mod / map-pack content (custom maps, custom models, custom textures, custom sounds) is owned by its individual authors and is typically governed by the terms of the mod's own readme or license. Read it before importing.
- Importing assets into an Unreal project for **personal, offline study or interoperability research** is the only use case this tool is intended for. Shipping a packaged Unreal game, a commercial product, a public demo, a YouTube monetised video, or any other public distribution that contains the converted assets is **your** legal problem, not the plugin author's.

If you do not own a legitimate Steam copy of the game whose `.bsp` you are importing, or you do not have explicit written permission from the copyright holder of a custom asset, **do not use this tool on that content.** The maintainers of this plugin accept no liability for misuse.

## Credits

Created by TzeentchNET. UE 5.7 hardening, material synthesis, LZMA + pakfile support, brush sub-meshes, static-prop instance + mesh importer, displacement seam stitching, and entity I/O graph maintained in this fork.
