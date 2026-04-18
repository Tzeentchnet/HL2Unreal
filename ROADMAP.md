# Roadmap

Forward-looking plan for the HL2 BSP Importer plugin. Completed work lives in [CHANGELOG.md](CHANGELOG.md); this file tracks what's still open and the next steps to take. Update both when shipping a phase: move the entry from "Open" here into a dated section in the changelog, and add any newly-discovered follow-ups under "Open" here.

## Status legend

- **Open** — not started, scoped enough to pick up.
- **Partial** — core landed; specific gaps below.
- **Deferred** — intentionally out-of-scope until a prerequisite ships.

---

## Open

### MDL / VVD / VTX static-prop mesh importer

Phase 12 ships an MDL / VVD / VTX reader trio + `UStaticMesh` synthesis, gated by the new `bImportStaticPropMeshes` setting (default off). When enabled the factory walks every unique `prop_static` model, locates its `.mdl/.vvd/.dx90.vtx` triple under `SourceContentRoots` (and the per-import pakfile extract dir), parses LOD-0 only, and produces one `UStaticMesh` per unique model under `<SynthesizedAssetRoot>/Props/<model path>`. Each `FHL2StaticProp` DataTable row now carries a `StaticMeshAsset` `FSoftObjectPath`, so a Blueprint / editor utility can iterate the table and spawn one `AStaticMeshActor` per row at the recorded transform. Materials are resolved through the existing `HL2Mat::FBuilder`, so a prop sharing a VMT with worldspawn re-uses the same MIC asset.

Outstanding (this entry stays open until):

- LOD chain support — **dropped**: Nanite (enabled by default on synthesized prop meshes) supersedes hand-authored Source LOD chains for the static-mesh path. Re-evaluate only if a non-Nanite renderer target ships.
- Per-bodygroup variant build path. Phase 12a fixed the default-bodygroup superimposition bug (now emits only `Models[0]` per bodypart). Multi-bodygroup props (`combine_intmonitor02`, citizens, broken/unbroken pairs, etc.) still need a way to bake the alternate variants — likely driven from `prop_dynamic` keyvalues once Phase 12c lands, since `prop_static` carries no bodygroup mask.
- Animated / dynamic prop classes (`prop_dynamic`, `prop_physics`) — the BSP only emits `prop_static` rows; the entity table carries the others as raw `FHL2Entity` rows whose `Model` field still requires the user's external pipeline to resolve to a Skeletal / animated asset. **Next code task** under this entry: extend the studio loader pipeline to entity-table rows with `Class` starting `prop_` and a `.mdl` `Model`, back-assigning a new `FHL2Entity::PropMesh` field. Skeletal / animation support remains out of scope.
- Validate against more community maps (Ravenholm, Citadel, Half-Life: Source ports). The pack-1 strip-group struct gained a `numTopologyIndices` field in newer Source SDK; we currently treat the HL2-era 25-byte layout as canonical and skip any trailing topology data, which has held on every test map so far.

### Displacement power-mismatch stitching

Phase 2b shipped spatial-cluster seam welding (catches float noise on coincident edges). Phase 2c shipped the geometric T-junction snap (catches power-mismatch cases where a power-3 disp meeting a power-2 disp leaves stranded intermediate vertices on the higher-power side).

Open follow-up:

- Validate against a real power-mismatch test map. The current pass is geometric (lonely-vertex onto closest neighbour-disp edge sub-segment) and does *not* parse `CDispNeighbor` / `CDispCornerNeighbors`. If a map is found where the neighbour's perimeter sequence is genuinely ambiguous (e.g. four-way disp corner meet with three different powers), revisit and consider parsing the neighbour topology lump as originally planned.

### Master-material assets ship

Phase 11a wired `"CanContainContent": true` in the `.uplugin`, defaulted every `ParentMaterial_*` settings slot to `/HL2BSPImporter/MasterMaterials/M_HL2_*`, added the new `ParentMaterial_LightmappedGeneric_Decal` slot, and rewrote `Resources/MasterMaterials/README.md` to document the full Phase 5/5b + Phase 11b parameter contract. Phase 11b extended the MIC binder to write 19 additional parameters (selfillum, color tints, alpha, phong, UV transforms, detail blend mode, envmap tint) and added a per-shader-family histogram log line.

Outstanding (this entry stays open until):

- **WIP** — authoring the seven `.uasset` master materials in the editor under `HL2BSPImporter/Content/MasterMaterials/` (`M_HL2_Lit`, `M_HL2_LitMasked`, `M_HL2_LitTranslucent`, `M_HL2_LitDecal`, `M_HL2_WorldVertexBlend`, `M_HL2_VertexLit`, `M_HL2_Unlit`) against the documented parameter contract. Step-by-step authoring guide: [`HL2BSPImporter/Content/MasterMaterials/MaterialBuilderInstructions.md`](HL2BSPImporter/Content/MasterMaterials/MaterialBuilderInstructions.md). Until the assets are saved, the importer logs `No parent material configured for shader '<x>'` and skips synthesis (settings + bindings are in place; the assets are the only missing piece). Tracked under `Content/MasterMaterials/PLACEHOLDER.md`.
- Phase 11c investigations (cubemap reflections under Lumen, Source water → UE5 Water plugin, material proxies). The `Cubemap` `TextureCube` parameter is currently bound only via `EnvmapTint`; the per-leaf baking decision is pending. See "Phase 11c — Material investigations" below.

### Phase 11c — Material investigations (no code)

Pending decisions that gate any further material work:

1. **Cubemap reflections under Lumen** — Lumen's screen-space + reflection-probe path generally subsumes Source's per-leaf baked cubemaps. Investigate whether `LUMP_CUBEMAPS` baking adds anything on top of Lumen, or whether the shipped masters can use a single neutral grey default cubemap and skip per-leaf baking permanently.
2. **Source `Water` / `Refract` shaders → UE5 Water plugin** — `func_water` brush sub-meshes already extract via Phase 8b. Prototype mapping them to `WaterBodyCustom` actors vs. shipping a translucent `M_HL2_Water` material. If Water plugin wins, file Phase 12 to swap brush emission to spawn `WaterBody*` actors at the brush position.
3. **Material proxies** — Source materials carry a `Proxies { … }` block for animated parameters (texture scrolling, pulse, light-style wired). Survey the top 5 proxy types in HL2 maps and define a static-bake interpretation for each (e.g. `TextureScroll` → `Panner` rate baked into the MIC).

Outputs are roadmap entries / new phases — no code lands directly under 11c.

### v21+ BSP support (CS:GO / L4D2)

Currently rejected at the version gate. v21 changes several lump struct sizes (notably `dgamelump_t` adds an 8-byte `_unknown` field on some branches) and bumps `dface_t`.

Next steps:
- Branch struct definitions on header version: keep v19/v20 as-is, add v21 variants behind compile-time-selected layouts.
- Extend the static-prop version table beyond v11 (CS:GO ships v10/v11; L4D2 ships v7/v10).
- Re-run the existing v19/v20 regression maps to confirm no regression on the supported path.

### HDR lightmap / light-style / 3D skybox

All currently dropped. These are large independent features; ordered roughly by user impact:

1. `LUMP_LIGHTING` (8) → bake lightmap UV atlas → `UTexture2D` + secondary UV channel already present on the mesh. Map this through a master-material `Lightmap` parameter slot.
2. `LUMP_LIGHTING_HDR` (53) preferred when present; fall back to LDR.
3. Light-style data: per-style intensity ramps drive flicker/strobe lights; would require a runtime component.
4. 3D skybox (`sky_camera` entity + `skybox/` BSP region): scale, transform, and composite the skybox geometry as a separate sub-mesh anchored to the sky_camera origin.

### `FReimportHandler` + Interchange refactor

Right now re-importing a BSP creates a fresh asset set rather than updating the existing one.

Next steps:
- Implement `FReimportHandler::CanReimport` / `Reimport` on `UHL2BSPImporterFactory` so right-click → Reimport works on the worldspawn `UStaticMesh` and propagates back to the BSP source.
- Stretch goal: rewrite the factory as an Interchange `UInterchangeTranslatorBase` + pipeline once UE 5.6's Interchange API for static meshes is stable enough.

### Big-endian (Xbox 360 / PS3) BSPs

`'PSBV'` magic is currently rejected with a clear error. Scope: byteswap every header / lump-struct field on read. Low priority — community maps are PC-only in practice.

---

## Partial

### Pakfile DEFLATE entries

Phase 8a added zlib-backed raw-deflate. Outstanding: encrypted ZIP entries (rare in BSP pakfiles, but `bspzip` can produce them with `-encrypt`) — currently fail with a per-entry warning. Decision pending on whether to support; most community maps don't use it.

### Compressed lump support

Phase 6 + 9b cover whole-lump LZMA and per-sublump LZMA on `sprp`. Other game-lumps that may be individually compressed in v21+ branches will need the same treatment when v21 support lands.

### Entity I/O graph

Phase 10 captures `On*` outputs as `FHL2EntityIO` rows. Non-`On*` duplicate keyvalues (rare — mostly `spawnflags` style or `OnMapSpawn` aliases on `logic_auto`) are still last-wins. Open if a real map is found that depends on a non-`On*` duplicate key.

---

## Deferred

- **Material parameter coverage beyond Phase 11b**: per-leaf cubemap baking from `LUMP_CUBEMAPS` (decision pending Phase 11c), water/refract shaders (decision pending Phase 11c — likely UE5 Water plugin remap), material proxies as runtime-dynamic parameters (would require a runtime component).
- **Brush-entity actor placement**: Phase 8b emits the per-brush-model `UStaticMesh` assets and links them via `FHL2Entity::BrushMesh`. Auto-spawning `AStaticMeshActor`s into a Level from the entity table is left to a Blueprint / editor utility — no factory-level "import as level" pass is planned until the static-prop mesh importer lands (so a single import can populate the whole level at once).
