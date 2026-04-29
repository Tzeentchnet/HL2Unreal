# Roadmap

Forward-looking plan for the HL2 BSP Importer plugin. Completed work lives in [CHANGELOG.md](CHANGELOG.md); this file tracks what's still open and the next steps to take. Update both when shipping a phase: move the entry from "Open" here into a dated section in the changelog, and add any newly-discovered follow-ups under "Open" here.

## Status legend

- **Open** — not started, scoped enough to pick up.
- **Partial** — core landed; specific gaps below.
- **Deferred** — intentionally out-of-scope until a prerequisite ships.

---

## Open

### MDL / VVD / VTX static-prop mesh importer

Phase 12 shipped the MDL / VVD / VTX reader trio + `UStaticMesh` synthesis, gated by the `bImportStaticPropMeshes` setting (default off). When enabled the factory walks every unique `prop_static` model, locates its `.mdl/.vvd/.dx90.vtx` triple under `SourceContentRoots` (and the per-import pakfile extract dir), parses LOD-0 only, and produces one `UStaticMesh` per unique model under `<SynthesizedAssetRoot>/Props/<model path>`. Each `FHL2StaticProp` DataTable row carries a `StaticMeshAsset` `FSoftObjectPath`. Materials are resolved through the existing `HL2Mat::FBuilder`, so a prop sharing a VMT with worldspawn re-uses the same MIC asset.

Phase 12a fixed the default-bodygroup superimposition bug (now emits only `Models[0]` per bodypart) and added skin-family selection: `HL2MdlReader` parses the `studiohdr_t` skin table into `FStudioFile::Skin`, and `BuildStaticMesh` takes a `SkinIndex` so each `mstudiomesh_t::material` ref-slot is remapped through `SkinFamilies[Skin]` before material resolution. Factory dedup keys on `(model, skin)` and emits `_skin{N}` sibling assets per non-zero skin actually referenced.

Phase 12c extended synthesis to point-entity props (`prop_dynamic` / `prop_physics` / `prop_ragdoll` / etc.). `FHL2Entity` gained `Skin` and `PropMesh` fields; the entity parser captures the `skin` keyvalue; the factory runs a parallel synthesis pass over `prop_*` rows whose `Model` resolves to a `.mdl`, sharing the on-disk asset cache with the `prop_static` pass. Entity-table creation is deferred until after synthesis so `_Entities` rows carry the resolved `PropMesh` paths.

Outstanding (this entry stays open until):

- LOD chain support — **dropped**: Nanite (enabled by default on synthesized prop meshes) supersedes hand-authored Source LOD chains for the static-mesh path. Re-evaluate only if a non-Nanite renderer target ships.
- Skeletal / animated synthesis for animated `prop_dynamic` (non-trivial `DefaultAnim`) and per-instance physics bodies for `prop_physics`. Both require parsing the MDL bone hierarchy / `mstudioseqdesc_t` / `mstudioanimdesc_t` blocks the current reader walks past — substantial scope, deferred until a target map demands it.
- Validate against more community maps (Ravenholm, Citadel, Half-Life: Source ports). The pack-1 strip-group struct gained a `numTopologyIndices` field in newer Source SDK; we currently treat the HL2-era 25-byte layout as canonical and skip any trailing topology data, which has held on every test map so far.

### Displacement power-mismatch stitching

Phase 2b shipped spatial-cluster seam welding (catches float noise on coincident edges). Phase 2c shipped the geometric T-junction snap (catches power-mismatch cases where a power-3 disp meeting a power-2 disp leaves stranded intermediate vertices on the higher-power side).

Open follow-up:

- Validate against a real power-mismatch test map. The current pass is geometric (lonely-vertex onto closest neighbour-disp edge sub-segment) and does *not* parse `CDispNeighbor` / `CDispCornerNeighbors`. If a map is found where the neighbour's perimeter sequence is genuinely ambiguous (e.g. four-way disp corner meet with three different powers), revisit and consider parsing the neighbour topology lump as originally planned.

### Master-material assets ship

Phase 11a wired `"CanContainContent": true` in the `.uplugin`, defaulted every `ParentMaterial_*` settings slot to `/HL2BSPImporter/MasterMaterials/M_HL2_*`, added the new `ParentMaterial_LightmappedGeneric_Decal` slot, and rewrote `Resources/MasterMaterials/README.md` to document the full Phase 5/5b + Phase 11b parameter contract. Phase 11b extended the MIC binder to write 19 additional parameters (selfillum, color tints, alpha, phong, UV transforms, detail blend mode, envmap tint) and added a per-shader-family histogram log line.

Outstanding (this entry stays open until):

- **WIP** — generating or authoring the seven `.uasset` master materials in the editor under `HL2BSPImporter/Content/MasterMaterials/` (`M_HL2_Lit`, `M_HL2_LitMasked`, `M_HL2_LitTranslucent`, `M_HL2_LitDecal`, `M_HL2_WorldVertexBlend`, `M_HL2_VertexLit`, `M_HL2_Unlit`) against the documented parameter contract. Step-by-step authoring guide and generator instructions: [`HL2BSPImporter/Content/MasterMaterials/MaterialBuilderInstructions.md`](HL2BSPImporter/Content/MasterMaterials/MaterialBuilderInstructions.md). Until the assets are saved, the importer logs warning-level missing-parent diagnostics such as `No loadable parent material for shader '<x>'` and skips synthesis (settings + bindings are in place; the assets are the only missing piece). Tracked under `Content/MasterMaterials/PLACEHOLDER.md`.
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

### Interchange refactor / partial reimport

Phase 12e added the smaller `FReimportHandler` layer: imported worldspawn `UStaticMesh` assets store `UAssetImportData`, and Content Browser right-click → Reimport reruns `UHL2BSPImporterFactory` against the original `.bsp` path. This covers the basic UE editor workflow without changing the factory architecture.

Next steps:
- Rewrite the factory as an Interchange `UInterchangeTranslatorBase` + pipeline. The Interchange static-mesh surface is production in 5.7 (see Phase 13 below), and this is still the right path for source-asset round-trip metadata, asset-registry diffs, async import, and per-import parameter overrides without round-tripping `Config/`.
- Add partial reimport modes after Interchange lands: geometry only, material/table refresh only, prop mesh refresh only.

### Phase 13 — Engine 5.7 modernization

UE 5.7 is the new baseline. The features below are concrete refactor / new-feature candidates that pay off because of 5.7-specific maturity. Each is sized as a standalone phase; none are required to keep the importer working — the existing 5.6 code paths continue to compile against 5.7.

Ordered roughly by user-visible impact.

1. **Nanite displacement maps for Source `dispinfo` (high impact, big simplification).** Source displacements are exactly the workload Nanite tessellation/displacement was designed for: a regular grid of per-vertex height + per-vertex alpha sampled from the dispinfo lump. In 5.7 Nanite displacement is production-ready (no longer experimental), so the entire Phase 2 / 2b / 2c pipeline (per-vertex triangulation + spatial-hash seam weld + T-junction snap) can collapse to: emit one flat quad per `ddispinfo_t` at the four corners, bake `dispverts.vec.z` into a `UTexture2D` height map and `dispverts.alpha` into the R channel of a blend mask, set the master material's Nanite displacement parameter to that height texture, set displacement magnitude from the disp's bbox. Power-mismatch cracks disappear at the source (they only exist because we're triangulating a uniform grid). Ship behind a new setting `bUseNaniteDisplacement` (default off until validated) so the existing geometry path stays as a fallback for non-Nanite renderer targets.

2. **PCG point-data emission for static / entity props (replaces the deferred ISM/HISM `bImportPropsAsInstances` work).** The static-prop instance table and entity-prop rows are already a flat (model, transform, skin, body) list. Emit a sibling `UPCGGraph` (or `UPCGPointData` asset) that consumes the table and spawns ISMs by model — users get free runtime regen, density / radius filtering, and HLOD attribution without writing a Blueprint. Closes the `bImportPropsAsInstances` setting (currently reserved + unimplemented). PCG point-data is stable in 5.7.

3. **MegaLights pass for `light_*` entities.** Source maps routinely ship 200–1000+ point lights (`light`, `light_spot`, sometimes `light_dynamic`). Pre-5.6 this was a non-starter under VSMs. With MegaLights stable in 5.7 those entities can be spawned as real `APointLight` / `ASpotLight` actors at import-time without tanking perf. Ship as an editor utility (or a new factory pass gated by `bSpawnLightActors`) that walks `_Entities`, maps `_light` colour/intensity to UE units, and writes one actor per entity. Pairs with item 5 (World Partition) so the actor count is OFPA-cheap.

4. **Interchange translator + pipeline refactor (builds on the basic `FReimportHandler`).** 5.7 is the first release where the Interchange static-mesh surface is suitable for shipping a third-party translator (multi-asset emit, custom payloads, per-pipeline factory parameters, source-asset round-trip, asset-registry diff). Rewrite `UHL2BSPImporterFactory` as `UInterchangeBSPTranslator` + `UInterchangeBSPPipeline`, exposing the existing `UHL2BSPImporterSettings` as pipeline parameters. Wins: partial reimport (e.g. just re-bake materials), `Diff` viewer, async import out of the editor's main thread, parameter overrides per-import without round-tripping `Config/`. Keeps the factory shim during transition so existing `.bsp → drag-drop` UX is preserved.

5. **World Partition + One File Per Actor "import as level" pass.** The lightweight `UHL2LevelSpawnLibrary::SpawnImportedActors` helper can now materialise the generated DataTables into actors in the active editor world. The larger WP+OFPA path would emit a `UWorld` directly: one OFPA actor per `FHL2Entity` row, auto-assigned to a Data Layer by classname (`prop_*` / `func_*` / `trigger_*` / `light_*` / `logic_*`), `prop_static` instances grouped into ISMs per model. OFPA keeps per-actor edits cheap; WP keeps the editor responsive on large maps. Ship behind `bImportAsLevel` (default off — DataTable path stays the canonical output for users who want full control).

6. **HLOD / Nanite-aware HLOD layer auto-config.** When item 5 lands, configure each spawned actor's `HLODLayer` by classname (small props → most aggressive proxy layer; brush sub-meshes → lower layer; lights → no HLOD). WP HLOD generates the merged proxies. No code beyond the layer assignment + a one-shot `BuildHLODs` invocation behind a setting.

7. **Substrate master materials (optional opt-in slot set).** Substrate is on-by-default in 5.7 for new projects. Author parallel `M_HL2_*_Substrate` parents that use slab-based BSDFs (a real specular slab for `$envmaptint` + `$phong*`, vertical-blend slab for `WorldVertexTransition`, masked emissive slab for `$selfillum`). Add a parallel `ParentMaterial_Substrate_*` settings block; `PickParent` prefers the Substrate slot when `r.Substrate=1`. Existing `M_HL2_*` (legacy material model) stays as the default so projects that haven't enabled Substrate don't break.

8. **Nanite Skeletal Meshes for animated `prop_dynamic` (unblocks the deferred MDL animation work).** The current studio loader walks past `mstudioseqdesc_t` / `mstudioanimdesc_t` / bone hierarchy; the existing roadmap defers skeletal synthesis as "substantial scope". Nanite Skeletal Meshes are production in 5.7, which makes the produced `USkeletalMesh` viable for the kinds of dense character meshes HL2 ships (Combine soldiers, Antlions, etc.) without needing a hand-authored LOD chain. Re-evaluates the cost/benefit of the deferred work: most of the cost is still MDL parsing, but the output side is now strictly better.

9. **Async / batched static-mesh build.** Replace the per-prop `M->Build()` in `HL2StaticPropMeshBuilder` and `HL2BSPImporterFactory` with `UStaticMesh::BatchBuild(TArray<UStaticMesh*>, ...)`. Batch-build was added pre-5.7 but is the recommended path in 5.7. On prop-heavy maps (Ravenholm, `d2_coast_*`) this should cut import time substantially. Pair with `UE::Tasks::Launch` for the per-model `LoadModel` parse stage (safe — pure I/O and CPU, no UObject mutation).

10. **`FDynamicMesh3` for the seam-weld + T-junction passes.** The Phase 2b/2c spatial-hash + union-find code is hand-rolled because `FMeshDescription` has no spatial query. `UE::Geometry::FDynamicMesh3` + `FDynamicMeshAABBTree3` (Modeling module, stable in 5.7) replace both passes with library calls and add proper non-manifold edge detection. Keep `FMeshDescription` as the final output (`FMeshDescriptionToDynamicMesh` / `FDynamicMeshToMeshDescription` round-trip is lossless for our subset).

11. **Texture Graph / TextureSet authoring path (investigation).** Group `$basetexture` + `$bumpmap` + `$detail` + `$blendmodulatetexture` into a single `UTextureSet` per VMT instead of N independent `UTexture2D` assets + a MIC. Tighter Content Browser story; one asset to override per material. Cost: TextureSet's parameter contract is fixed by the project's `TextureSetDefinition`, so the master-material parameter contract becomes coupled to the TextureSet schema. Decide before committing.

12. **Background import via `FAsyncTaskNotification`.** Replace `FScopedSlowTask` with a non-blocking notification so large maps don't lock the editor. Cooperates with item 4 (Interchange already runs async), so this lands either in the legacy factory or falls out of item 4 for free.

Tracking: the items above are independent and can ship in any order. Item 1 (Nanite displacement) and item 2 (PCG props) have the highest user-visible impact and the smallest blast radius (additive, behind a setting). Item 4 (Interchange) is the largest refactor and should land before item 5 (WP/OFPA) so the level-emission path uses the Interchange asset-registry contract from day one.

### Big-endian (Xbox 360 / PS3) BSPs

`'PSBV'` magic is currently rejected with a clear error. Scope: byteswap every header / lump-struct field on read. Low priority — community maps are PC-only in practice.

### Standalone .vmt and .vtf factories

Shipped under the Tranche B entry in [CHANGELOG.md](CHANGELOG.md). `UHL2VTFFactory` (UTexture2D / UTextureCube) and `UHL2VMTFactory` (UMaterialInstanceConstant) handle right-click → Import on individual `.vtf` / `.vmt` files in the Content Browser, and underpin the bulk-import toolbar.

The Tranche B follow-up shipped the normal-map alpha sibling: when a `$bumpmap` VTF carries non-uniform alpha, both the BSP-driven material builder and the standalone `.vtf` factory emit a sibling `<key>_a` `UTexture2D` (grayscale mask) and bind it to the master-material `NormalAlpha` / `Normal2Alpha` parameter. No outstanding work tracked under this entry.

### Editor toolbar + bulk-import menu ("BSP Importer")

Shipped under the Tranche B entry in [CHANGELOG.md](CHANGELOG.md). Three actions registered: Bulk Import Textures (.vtf), Bulk Import Materials (.vmt), Import BSP. Tranche B follow-ups added: **Bulk Import Models** (.mdl → `UStaticMesh` via the new `UHL2MDLFactory`) and a custom Half-Life lambda SVG icon on the toolbar combo button (loaded via a per-plugin `FSlateStyleSet`).

Outstanding (this entry stays open until):

- Convert Skyboxes menu entry — currently happens automatically during BSP import via the `bConvertSkyboxes` setting; a standalone toolbar action would let users build cubemaps from a `materials/skybox/` tree without importing a map.

### Sound script + soundscape importers (.wav only)

`scripts/game_sounds_manifest.txt` and `scripts/soundscapes_manifest.txt` both reference nested `.txt` files via `#include` directives, then map sound names → `.wav` paths + attenuation / radius (game sounds) or sound names → `ambient`/`random`/`playlooping`/`playsoundscape` directives (soundscapes).

Scope:
- `UHL2SoundScriptFactory` and `UHL2SoundScapeFactory` parse the respective manifests via `HL2KV` (Phase A4), follow `#include` directives (this is the consumer that lights up the deferred `#include` resolution path in `HL2KV`), emit one runtime asset per manifest carrying the parsed mapping.
- Per-`USoundWave` import for the referenced `.wav` payloads under `<SynthesizedAssetRoot>/Sounds/<source path>`, sourced from the per-import pakfile + `SourceContentRoots/sound/`.

Skipped: `UMP3SoundFactory` / MPG123 (third-party dep). UE 5.7 ships an in-engine MP3 decoder via WMF on Windows — could lift to that as a follow-up if HL2 music (`media/*.mp3`) becomes a target. Also skipped: `ambient_generic` / `env_soundscape` actor materialisation — leave that to user Blueprint over the imported DataTable until the runtime gameplay scope (currently excluded) revisits.

Sizing: M (~400 LOC of script parsers + ~300 LOC of runtime asset class). Blocked on Phase A4 (already shipped).

### Animated MDL importer (USkeletalMesh + UAnimSequence + UPhysicsAsset)

Phase 12 shipped `prop_static` static-mesh synthesis. Animated `prop_dynamic`, `prop_physics`, and `prop_ragdoll` need skeletal synthesis: `USkeletalMesh` + `USkeleton` + per-sequence `UAnimSequence` + per-bone `UPhysicsAsset`. Phase 13 item 8 (Nanite Skeletal Meshes) is the consumer that makes the scope worthwhile.

Scope (lifts AlleyKatPr0's `MDLFactory.cpp` ~2200 LOC reference implementation, ported to use our `HL2MdlReader`/`HL2VvdReader`/`HL2VtxReader` instead of the bundled `studiomdl/` namespace, and our `HL2KV` parser instead of `UValveDocument`):
- Bone hierarchy → `FReferenceSkeleton` (with multi-root synthesis: inject `"root"` bone, auto-rename colliding source bones to `"actual_root"`).
- Per-LOD `FSkeletalMeshImportData` build, bone weights from `mstudiovertex_t::m_BoneWeights`, MikkTSpace tangent path, calls `IMeshBuilderModule::BuildSkeletalMesh`.
- `mstudioseqdesc_t` + `mstudioanimdesc_t` walk handling section-banded compressed anims and `animblock != 0` indirection through external `.ani` files. Compressed-rotation/position pipeline (`ANIMROT`/`ANIMPOS` RLE-decoded via `ReadAnimValues`/`DecompressAnimValues` per axis, scaled by `bone->rotscale[axis]` / `bone->posscale[axis]`, optionally additive via `ANIMDELTA` flag), `RAWROT` (`Quat48`), `RAWROT2` (`Quat64`), `RAWPOS` (`Vector3f`), root-bone yaw fix-up (`angles.Z += DegreesToRadians(-90.0f)`). Source Tait-Euler ↔ quaternion via `SourceAnglesToQuat` / `QuatToSourceAngles` (canonical reference: marc-b-reynolds.github.io blog).
- `.phy` solid parse (compact + legacy surface header variants), KV1 text-section parse for `solid { index, name, surfaceprop }` → `USkeletalBodySetup` per bone (with `PhysMaterial = USurfaceProp` from Phase A3), `ragdollconstraint { parent, child, xmin/xmax/xfric, ymin/..., zmin/... }` → `UPhysicsConstraintTemplate`, `collisionrules { collisionpair "i,j" ... }` → `physicsAsset->EnableCollision(i, j)`.
- `mstudioattachment_t` 4×3 matrices → `USkeletalMeshSocket` with bone-relative Location/Rotation/Scale.
- `mstudiomodelgroup_t` `.mdl` includes for shared-skeleton animation packs.

Net-new readers required: `HL2PhyReader` (physics file), `HL2AniReader` (external animation banks). `HL2MdlReader` extension to expose seq/anim/bone-hierarchy that the current reader walks past.

Sizing: L (~2200 LOC port + ~600 LOC of net-new reader code). Blocked on Phase A3 + Phase A4 (both already shipped). Re-evaluate against Phase 13 item 8 (Nanite Skeletal Meshes) before scheduling.

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
- **Full import-as-level workflow**: `UHL2LevelSpawnLibrary::SpawnImportedActors` can place brush meshes, entity prop meshes, static props, and basic light placeholders from the generated DataTables into the active editor world. A factory-level or Interchange-driven `UWorld` emission path with World Partition, OFPA, Data Layers, calibrated lights, HLOD assignment, and runtime Source I/O remains deferred.
- **Sound / audio support**: nothing in the importer currently touches Source's audio pipeline beyond preserving raw audio-entity keyvalues on `FHL2Entity`. A future `HL2SoundBuilder` pass would need to (1) extract `.wav` / `.mp3` payloads from the per-import pakfile and `SourceContentRoots/sound/` trees, (2) import them as `USoundWave` assets under `<SynthesizedAssetRoot>/Sounds/<source path>`, (3) parse `scripts/soundscapes_*.txt` into reusable sound cue / `USoundscape`-equivalent assets, and (4) materialise audio entities (`ambient_generic`, `env_soundscape`, `env_soundscape_proxy`, `env_soundscape_triggerable`) into `AAmbientSound` actors with the resolved `USoundBase` reference and Source's attenuation / radius / spawnflag mapping. Deferred until a target map demands it; opens behind a `bImportSounds` setting and a `ParentSoundAttenuation` slot mirroring the material-parent contract.
