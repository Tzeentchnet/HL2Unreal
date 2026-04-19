# Changelog

All notable changes to this plugin are documented in this file. Forward-looking work (open follow-ups, partial features, deferred items) lives in [ROADMAP.md](ROADMAP.md); when a roadmap entry ships, move it here under the appropriate phase.

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/), and this project tracks Unreal Engine 5.7+ compatibility (5.6 was the prior baseline through Phase 12d).

## [Unreleased]

### Added — Per-bodygroup variant build path (Phase 12d)

- `HL2StaticPropMeshBuilder::BuildStaticMesh` gained a `BodyMask` parameter (default 0) that decodes Source's packed `body` keyvalue into a per-bodypart model index using the canonical `idx = (mask / base[bp]) % BodyParts[bp].Models.Num()` formula, where `base[0] = 1` and `base[bp+1] = base[bp] * BodyParts[bp].Models.Num()`. `BodyMask == 0` keeps the Phase 12a default-bodygroup behaviour exactly (`Models[0]` of every bodypart), so `prop_static` output is byte-identical. Out-of-range computed indices fall back to `Models[0]` for the offending bodypart.
- `FHL2Entity` gained an `int32 BodyGroup` (default 0) field; the entity-text parser in `BspFile.cpp` reads either the canonical Source `body` keyvalue or the `bodygroup` alias (preferring `body` on ties, removing both from the keyvalue map). `prop_static` rows do not get a bodygroup field — the `sprp` GameLump carries no mask.
- Factory entity-prop synthesis now extends the `(model, skin)` dedup key with an optional `#bg<N>` suffix (full grammar: `<model>(#skin<N>)?(#bg<M>)?`), and appends `_bg<N>` to the asset filename for non-zero masks (stacked as `_skin{N}_bg{M}` when both vary). Variant decoding uses a small suffix-strip helper that walks the key right-to-left so either suffix may be absent. The `prop_static` synthesis pass is unchanged — those rows always pass `BodyMask = 0`, preserving the Phase 12 asset paths exactly.
- `Static prop mesh built: ...` log line now includes `body=N` alongside `skin=N` so per-bodygroup builds are observable. The end-of-import summary lines are unchanged in shape.
- Multi-bodygroup props (`combine_intmonitor02`, citizens with head-A/B/C, broken/unbroken pairs, etc.) referenced from `prop_dynamic` / `prop_physics` with a non-zero `body` keyvalue now bake the correct per-bodypart variant set instead of silently collapsing to the default. No new settings or .ini entries.

### Added — Entity-prop static-mesh synthesis (Phase 12c)

- Source point-entity props (`prop_dynamic`, `prop_physics`, `prop_ragdoll`, `prop_dynamic_override`, `prop_physics_multiplayer`, etc.) are emitted as `FHL2Entity` rows in the entity lump rather than via the `sprp` GameLump, so they previously bypassed the Phase 12 mesh-synthesis pass entirely. The factory now runs a parallel synthesis block that walks `Entities` looking for any row whose `Class` starts with `prop_` and whose `Model` resolves to a `.mdl` path (brush refs `*N` are skipped), dedups by `(model, skin)`, and back-assigns a new `FHL2Entity::PropMesh` `FSoftObjectPath` field per row.
- New `FHL2Entity::Skin` (`int32`, default 0) captures the `skin` keyvalue at parse time. The entity-text parser in `BspFile.cpp` now reads it via the existing `KV.RemoveAndCopyValue` path, mirroring `origin` / `angles` / `model` extraction. Missing / non-numeric values default to 0.
- Synthesis re-uses the same `HL2Studio::LoadModel` + `HL2Studio::BuildStaticMesh` pipeline as `prop_static`, the same `<SynthesizedAssetRoot>/Props/<sub>/...` layout, the same `_skin{N}` filename convention, and shares the on-disk asset cache — so a model referenced by both a `prop_static` instance and a `prop_dynamic` entity is a `TryLoad` cache hit on the second pass (no rebuild).
- The entity-table creation block was moved to *after* the static-prop synthesis so the `_Entities` DataTable carries the resolved `PropMesh` paths. The `_StaticProps` table ordering is unchanged.
- New end-of-import log line: `Entity prop meshes: built=N cached=N failed=N (unique=N, candidates=N)`. The static-prop summary line is unchanged in shape; combined with the entity line it gives a complete picture of mesh synthesis across the level.
- Scope: only the static geometry is produced. Skeletal asset synthesis, animation graph wiring, and physics-body simulation remain explicitly out of scope under the existing Phase 12 contract — the user's pipeline is expected to take the `PropMesh` path and either spawn it as a Static Mesh actor (default-pose preview) or substitute its own Skeletal Mesh asset for fully animated props.
- Gated by the existing `bImportStaticPropMeshes` setting; no new settings or .ini entries.

### Added — Static-prop default bodygroup + skin-family selection (Phase 12a)

- `HL2StaticPropMeshBuilder::BuildStaticMesh` now emits only `BodyParts[bp].Models[0]` per bodypart instead of iterating every model in every bodypart. This is the default bodygroup state — `prop_static` instances carry no bodygroup mask, so the previous behaviour superimposed every alternate variant (broken/unbroken, head A/B/C, …) on top of the default. Single-bodypart-single-model props (the HL2 norm) are unchanged; multi-bodygroup props (e.g. `models/props_combine/combine_intmonitor02.mdl`) now render only the default geometry. Other variants stay parsed in `FStudioFile::BodyParts` for a future per-bodygroup build path.
- `HL2MdlReader` now parses the `studiohdr_t` skin table (`numskinref`, `numskinfamilies`, `skinindex`) into a new `FStudioSkinTable` carried on `FStudioFile::Skin`. Layout is `int16 skinref[numskinfamilies][numskinref]` at byte offset `skinindex` from the file start, sign-extended into `int32`. Missing / out-of-range / EOF-spanning tables log Verbose and leave `Skin` empty, in which case the builder falls back to identity skin 0 (the prior behaviour).
- `BuildStaticMesh` gains an optional `SkinIndex` parameter (default 0). Each `mstudiomesh_t::material` ref-slot is now remapped through `SkinFamilies[SkinIndex]` before resolving the material via `HL2Mat::FBuilder`, so a barrel with skin 1 (rust variant) produces a mesh whose polygon-group slots reference the rust VMTs instead of the clean ones. Out-of-range skin / ref-slot indices silently fall back to identity.
- The factory dedup map now keys on `(NormalisedModelPath, Skin)` instead of model path alone. Skin 0 keeps the bare model key so the asset path is unchanged from Phase 12; skin > 0 produces sibling assets with a `_skin{N}` filename suffix (e.g. `oildrum001_skin1`). Each prop_static row's `StaticMeshAsset` is back-assigned to the skin-specific variant. Re-imports re-use existing per-skin assets at the destination path.
- `Static prop mesh built: ...` log line now includes `skin=N` so per-skin builds are observable. The end-of-import summary line is unchanged in shape — `built/cached/failed/unique/instances` — but the `unique` count now reflects distinct `(model, skin)` pairs.
- No new settings or .ini entries: the change is purely additive on the existing `bImportStaticPropMeshes` pipeline.

### Added — MDL / VVD / VTX static-prop mesh importer (Phase 12)

- New Source studio file readers (`HL2MdlReader`, `HL2VvdReader`, `HL2VtxReader`) plus a top-level `HL2StudioLoader::LoadModel` orchestrator that resolves a `.mdl/.vvd/.dx90.vtx` triple under the configured `SourceContentRoots` (and the per-import pakfile extract dir), parses all three, and cross-checks the studio checksum across files. Supports `studiohdr_t` versions 44..49 (HL2 → Source 2013-era), `vertexFileHeader_t` v4, and `OptimizedModel::FileHeader_t` v7. LOD 0 only.
- `HL2MdlReader::Parse` walks the body-part / model / mesh hierarchy and produces an `FStudioFile` with one `FStudioBodyPart`/`FStudioModel`/`FStudioMesh` entry per `mstudiomesh_t`, along with a per-material candidate list assembled from the MDL's `numcdtextures` directories crossed with each `numtextures` name (Source's standard search order). Header layout is documented field-by-field with explicit byte offsets to keep the on-disk contract visible (`mstudiohdr_t` is ~408 bytes; we only need ~10 fields).
- `HL2VvdReader::Parse` decodes the LOD-0 vertex stream (position / normal / UV from `mstudiovertex_t` at stride 48; tangent + handedness from the optional tangent table at stride 16). The `vertexFileFixup_t` table is honoured: when present, each fixup with `lod >= 0` contributes a `[sourceVertexID, sourceVertexID + numVertexes)` run to the rebuilt LOD-0 array. Bone weights are skipped (static props don't need them).
- `HL2VtxReader::Parse` walks the BodyPart → Model → ModelLOD → Mesh → StripGroup → Strip hierarchy and produces flat triangle index lists per `FStudioMesh`. `STRIP_IS_LIST` and `STRIP_IS_TRISTRIP` strip flags are both honoured (tristrips are triangulated with the standard alternating-winding rule); other flag combinations log Verbose and skip the strip. Each `Vertex_t.origMeshVertID` is mapped to a mesh-local index, then combined with the MDL's `mstudiomesh_t.vertexoffset` and `mstudiomodel_t.vertexindex` to address into the post-fixup VVD array. Pragma-pack(1) layouts (MeshHeader_t / StripGroupHeader_t / StripHeader_t / Vertex_t) are walked via field offsets rather than struct mirrors.
- Cross-file validation: VVD checksum, VTX checksum, body-part count, and per-model mesh count must all match the MDL. Mismatches fail with a clear error rather than reading out of range. Bone state changes, flex deltas, material-replacement lists, and additional LODs are walked-past but not consumed.
- New `HL2StaticPropMeshBuilder::BuildStaticMesh` synthesises a `UStaticMesh` from an `FStudioFile`. One polygon group per `mstudiomesh_t` (named after the resolved Source material slot, lower-case + forward-slashed). Vertices are de-duplicated globally across the model so a vertex shared between two meshes is one `FVertexID` with two `FVertexInstanceID`s. The Source→Unreal coordinate transform (Y/Z swap + Y negate + `WorldScale`) is applied to positions and normals; tangents are recomputed via MikkTSpace through the standard editor source-model build path, mirroring the BSP factory's mesh pipeline (Nanite settings, `SrcLightmapIndex=1`, `MinLightmapResolution=64`, complex-as-simple collision when enabled).
- Material resolution per polygon group iterates the MDL-supplied candidate list (most-likely cdtex/name combination first) through the existing `HL2Mat::FBuilder`, so a prop sharing a VMT with worldspawn (e.g. `metal/metalwall045a`) re-uses the same `UMaterialInstanceConstant` rather than creating a duplicate. The first candidate that resolves wins; failures fall back to the engine default surface material with the slot still named after the first candidate so manual JSON-map overrides remain possible.
- New developer setting `bImportStaticPropMeshes` (default `false`) gates the entire pipeline. When enabled (and `bSynthesizeMaterials` is on), the factory walks every unique `prop_static` model path, builds one `UStaticMesh` per unique model under `<SynthesizedAssetRoot>/Props/<model path>` (Source's hierarchy preserved, with a `_` substitution for any character outside `a-z0-9_-/.`), and populates each `FHL2StaticProp::StaticMeshAsset` (new `FSoftObjectPath` field) with the resulting asset. Re-imports re-use existing assets at the destination path instead of overwriting them. A `Static prop meshes: built=N cached=N failed=N (unique=N, instances=N)` summary line lands at the end of the import; per-model failures (missing file, version mismatch, zero triangles) log Verbose and leave the row's `StaticMeshAsset` empty so a fallback pipeline can still resolve them.
- `Config/DefaultHL2BSPImporter.ini` carries the new flag default. Settings UI exposes it under `Project Settings → HL2 BSP Importer → Props`.

### Added — Displacement power-mismatch T-junction snap (Phase 2c)

- New geometric snap pass in `BuildMeshDescriptionFromBSP` runs after the existing spatial-cluster seam weld (Phase 2b) and fixes the cracks that remain when a higher-power displacement meets a lower-power neighbour: the high-power side's intermediate edge vertices have no counterpart on the low-power side, so the centroid pass leaves them stranded as cluster-of-one.
- During disp emission each disp now records its four perimeter edges as ordered `FVertexID` sequences (`FDispEdgeBundle::Edges[4]`, length = `(1 << power) + 1`). The new pass walks every consecutive (Va, Vb) pair on every disp's edge and treats each pair as a candidate "neighbour edge sub-segment". For each lonely (cluster-size 1) perimeter vertex P on disp A, the pass finds the closest sub-segment from a *different* disp B and snaps P onto the segment when (a) P projects strictly inside the segment (`t ∈ (0.05, 0.95)`, so coincident-corner cases the centroid pass already owns are skipped), (b) the closest-point distance is `≤ DisplacementSeamWeldDistance`, and (c) the segment has nonzero length.
- Sub-segments are pre-binned into the same spatial-hash cell size (`WeldDist`) the existing cluster pass uses, by walking every cell their bbox (expanded by `WeldDist`) covers. Each lonely-vertex query touches only the segments in its own cell, so the pass is O(N) in lonely-perimeter-vertex count for typical maps.
- Approach intentionally avoids parsing `CDispNeighbor` / `CDispCornerNeighbors` from `LUMP_DISPINFO`: the prior centroid pass already aligns coincident corners, so neighbour edges are well-defined by the neighbour's own perimeter vertex sequence. This sidesteps Source's per-edge orientation / span semantics and the existing struct's intentionally-opaque neighbour byte block.
- `FBuildResult` gains `DispEdgeSnapsPowerMismatch` and the build summary log line now reports it alongside `SeamClusters` / `SeamVertsWelded`. No new settings: the snap reuses `bStitchDisplacementSeams` (default true) and `DisplacementSeamWeldDistance` (default 1.0 cm) from Phase 2b.

### Added — Extended VMT parameter coverage (Phase 11b)

- `HL2MaterialBuilder.cpp` `GetOrCreateMaterial` now binds an additional set of MIC parameters whenever the corresponding VMT key is present. Every binding is silently no-op when (a) the key is absent in the VMT or (b) the parent material doesn't expose the parameter, so the change is purely additive on top of Phase 5 / 5b.
- New textures: `EmissiveColor` ← `$selfillummask`. New scalars: `EmissiveStrength` (set to 1.0 when `$selfillum 1`), `OpacityScalar` ← `$alpha`, `Phong` (1.0 when `$phong 1`), `PhongExponent` ← `$phongexponent` (default 5.0), `PhongBoost` ← `$phongboost` (default 1.0), `BaseColorUVRotate` / `NormalUVRotate` (degrees), `DetailBlendMode` ← `$detailblendmode` (0..12 cast to float), `DetailBlendFactor` ← `$detailblendfactor`. New vectors: `EmissiveTint` ← `$selfillumtint`, `BaseColorTint` ← `$color`, `BaseColor2Tint` ← `$color2`, `EnvmapTint` ← `$envmaptint`, `BaseColorUVScale` / `BaseColorUVOffset` / `NormalUVScale` / `NormalUVOffset` packed as `(x, y, 0, 0)` from `$basetexturetransform` / `$bumptransform`.
- New helper `ParseSourceColor` in `HL2MaterialBuilder.cpp` accepts every Source color notation the engine writes: `[r g b]` (0..1 floats), `{r g b}` (0..255 ints), bare `r g b`, single luminance `0.5`. Alpha defaults to 1.0; comma separators are tolerated.
- New helper `ParseUVTransform` parses Source's full `center cx cy scale sx sy rotate deg translate tx ty` matrix syntax in any token order, with missing tokens defaulting to identity. The `center` pivot is folded into the translate output to keep the master parameter set minimal (Scale / Rotate / Offset only).
- New parent-material slot `ParentMaterial_LightmappedGeneric_Decal` (`FSoftObjectPath`, settings + ini default `/HL2BSPImporter/MasterMaterials/M_HL2_LitDecal`). `PickParent` now recognises any shader name containing `decal` and routes it to this slot first, falling through to the existing translucent / masked / lit chain when the slot is unset.
- Shader-family classification (`ClassifyShader` in the anonymous namespace) maps each VMT shader to one of `Lit / LitMasked / LitTranslucent / Decal / WorldVertexTransition / VertexLit / Unlit / Other`. The selected family drives both the parent-material picker and a per-create histogram on `FBuilder` (`NumShader_Lit / NumShader_LitMasked / NumShader_LitTrans / NumShader_Wvt / NumShader_VertexLit / NumShader_Unlit / NumShader_Decal / NumShader_Other`). Histogram is bumped per *created* MIC, not per cached hit, so the totals reflect distinct materials rather than face count.
- New factory log line at the end of each import: `Material shaders: lit=N masked=N translucent=N wvt=N vlit=N unlit=N decal=N other=N` so the mix of shaders is observable per BSP without re-running with verbose logging.
- Cubemap reflections: only `EnvmapTint` is bound this phase. The `Cubemap` `TextureCube` parameter is left at its master-material default pending the Phase 11c investigation into whether per-leaf cubemap baking adds anything on top of Lumen reflections.

### Added — Master materials shipped in plugin (Phase 11a)

- `HL2BSPImporter.uplugin` now sets `"CanContainContent": true` so the plugin's `Content/MasterMaterials/` folder is mounted at `/HL2BSPImporter/MasterMaterials/` in any project that enables the plugin.
- `UHL2BSPImporterSettings::ParentMaterial_*` defaults now point at the in-plugin master assets (`/HL2BSPImporter/MasterMaterials/M_HL2_Lit`, `_LitMasked`, `_LitTranslucent`, `_LitDecal`, `_WorldVertexBlend`, `_VertexLit`, `_Unlit`). `Config/DefaultHL2BSPImporter.ini` carries the same paths so existing installs upgrade on next plugin load.
- `Resources/MasterMaterials/README.md` rewritten to (a) flag that the masters now ship in-plugin so a fresh install is zero-config, (b) document the per-project override pattern (point any `ParentMaterial_*` settings slot at your own `UMaterialInterface`; missing parameters are silently skipped), and (c) lay out the full Phase 5 / 5b + Phase 11b parameter contract in one table per category.
- New `HL2BSPImporter/Content/MasterMaterials/PLACEHOLDER.md` notes that the actual `.uasset` master materials must be authored in the editor against the documented contract — they cannot be generated from C++ and are therefore tracked separately from this code-only changeset.

### Added — Entity I/O graph parsing (Phase 10)

- The entity-text parser in `BspFile.cpp` now preserves Source I/O output connections instead of collapsing them through the per-entity key→value map. Any keyvalue whose key begins with `On` (case-insensitive — Source's `CBaseEntityOutput` convention; covers `OnTrigger`, `OnPressed`, `OnStartTouch`, `OnEndTouch`, `OnUser1`..`OnUser4`, `OnDamaged`, `OnBreak`, `OnIgnite`, etc.) is appended to a per-entity `Outputs` list. A single output that fires three downstream targets (the common Source pattern of three identical `OnTrigger` keys on one `trigger_multiple`) now produces three rows instead of silently keeping only the last one.
- New USTRUCT `FHL2EntityIO` carries the parsed connection: `OutputName`, `TargetName`, `InputName`, `Parameter`, `Delay` (seconds), `TimesToFire` (-1 = infinite). The Source value format `target<sep>input<sep>parameter<sep>delay<sep>timesToFire` is split on `0x1B` (ESC, modern Source) when present, otherwise on `,` (HL2-era), so both vintages of compiled BSPs round-trip cleanly. Missing trailing fields default to empty / 0.0 / -1.
- `FHL2Entity` gains `TArray<FHL2EntityIO> Outputs` exposed `EditAnywhere, BlueprintReadWrite`. The entity DataTable rows produced by `UHL2EntityTable::CreateFromEntities` automatically carry the new array, so a Blueprint or editor utility can iterate `Row.Entity.Outputs` to materialise the level's logic graph (event dispatchers, delegate bindings, etc.) on top of the spawned actors.
- New summary log line at the end of entity parsing: `Entities: N parsed, M I/O outputs.` — directly observable count for verifying that maps with heavy logic (e.g. `d1_trainstation_*`) didn't lose their connections.
- Backwards-compatible: non-`On*` keyvalues continue through the existing `KV` map and `targetname` / `classname` / `origin` / `angles` / `model` extraction is unchanged.

### Added — WorldVertexTransition blend completion (Phase 5b)

- The MIC synthesizer now reads `$bumpmap2` and binds it to a `Normal2` texture parameter (`TC_Normalmap`, `sRGB=false`) on the parent, alongside the already-shipped `BaseColor2` for `$basetexture2`. The two-layer normal blend on Source displacement terrain (cliff/grass, gravel/dirt, etc.) now reaches the master material instead of being dropped on the floor.
- `$blendmodulatetexture` is bound to a new `BlendModulate` texture parameter. Source's convention (R = blend midpoint, G = blend softness) is documented in the master-materials README with the recommended remap → lerp wiring; parents that don't expose `BlendModulate` simply ignore the texture and fall back to a raw `VertexColor.A` blend.
- All three new bindings are silently no-ops when the VMT doesn't carry the corresponding key, so single-layer `WorldVertexTransition` materials continue to import unchanged. No new settings or .ini entries.
- `Resources/MasterMaterials/README.md` now lists `Normal2` and `BlendModulate` in the parameter contract and documents the standard `(VertexColor.a - (edge - soft)) / (2*soft)` remap so a project authoring a `M_HL2_WorldVertexBlend` parent has the full intended graph in one place.

### Added — Displacement seam stitching (Phase 2b)

- Cross-displacement perimeter welding in `BuildMeshDescriptionFromBSP`. While building each displacement grid the four-edge perimeter `FVertexID`s are collected into a per-import list. After every displacement is emitted, those vertices are clustered with a spatial hash (cell size = weld distance) and union-find over the 3×3×3 neighbourhood. Each cluster of size ≥2 is snapped to its centroid in `VertexPositions`, eliminating the hairline T-junction cracks that appear at terrain seams when the source map wasn't perfectly "Sew"-ed in Hammer. Interior displacement vertices are left untouched, so sculpted detail is preserved.
- New developer settings `bStitchDisplacementSeams` (default `true`) and `DisplacementSeamWeldDistance` (default `1.0` Unreal cm, clamped 0.0001..16). The default covers float-precision noise on a 2.54 cm/inch map without distorting intentionally-separated edges. The setting is exposed under `Project Settings → Plugins → HL2 BSP Importer → Displacements` and persisted in `Config/DefaultHL2BSPImporter.ini`.
- The build summary log line now reports `SeamClusters=N SeamVertsWelded=M` so the effect of the pass is observable per import. `FBuildResult` gains `DispSeamClusters` / `DispSeamVertsWelded` for callers that want to read the same numbers programmatically.
- Note: this fixes seam alignment between displacements that were authored with coincident-up-to-noise edge vertices. T-junction *splits* across power-mismatched neighbours (a power-3 disp meeting a power-2 disp) require resolving Source's per-edge `CDispNeighbor` topology and snapping intermediate vertices onto the lower-power neighbour's interpolated edge; that is still pending and tracked under "Known limitations".

### Added — Compressed sprp GameLump support (Phase 9b)

- The `sprp` sub-lump under `LUMP_GAME_LUMP` is now decompressed in-place when `dgamelump_t.flags & 0x1` is set. The compressed payload at `FileOfs..FileOfs+FileLen` is fed through the existing `HL2Lzma::DecompressSourceLump` path (the standard 17-byte Source LZMA header — `'LZMA'` magic + `actualSize` + `lzmaSize` + 5-byte properties — is expected at the start of the sub-lump payload, matching how Source's compressed game lumps are written by tooling that emits per-sublump LZMA). The decompressed buffer is then parsed by the existing dict / leaf / entry walker, so static-prop instances from compressed `sprp` payloads are now extracted instead of being dropped with a warning.
- A missing/invalid LZMA header on a sub-lump flagged compressed produces a single `Warning` and skips static props (rather than reading garbage). Decoder failures fall through with the decoder's own log line.
- New verbose log: `sprp GameLump decompressed: <compressed> -> <actual> bytes.`
- The whole-lump LZMA path on `LUMP_GAME_LUMP` itself (via `AcquireLumpBytes`) is preserved and stacks correctly with this change: a compressed game-lump directory is decompressed first, then any sub-lump flagged compressed inside it is decompressed a second time.

### Added — Static-prop instance table (Phase 9)

- New `LUMP_GAME_LUMP` (35) parser in `BspFile.cpp` walks the `dgamelumpheader_t` directory and locates the `sprp` (`'sprp'` four-cc, little-endian id `0x70727073`) sub-lump. Sub-lumps marked compressed (`flags & 0x1`) are routed through the LZMA decoder (see Phase 9b).
- `sprp` payload is parsed in three sections per Source convention: dictionary (`int32 count` + `char[128]` model paths), leaf array (`int32 count` + `uint16[count]`, walked but not consumed), and entries (`int32 count` + per-entry `StaticPropLump_t`). Per-entry stride is computed from `(payloadEnd - cursor) / entryCount` and cross-checked against a per-version table (v4=56, v5=60, v6=64, v7/v8=68, v9=72, v10=76, v11=80). The on-disk stride wins when the two disagree, accommodating community VBSP variants. Versions outside 4..11 still parse the v4 56-byte common prefix.
- v11 `UniformScale` is read from offset 76 of each entry when the stride permits; finite-positive values are used, otherwise scale defaults to 1.0.
- New raw type `FBspStaticProp` (in `BspFile.h`) holds the per-instance source data: `ModelName`, `Origin` (Source-coord inches), `Angles` (pitch/yaw/roll), `UniformScale`, `Skin`, `Solid`. Exposed via `FBspFile::GetStaticProps()`.
- New USTRUCT `FHL2StaticProp` and DataTable type `UHL2StaticPropTable` (sibling to the existing `UHL2EntityTable`). Origin/Rotation are persisted in **Unreal coordinates** (cm, left-handed, Z-up) so a Blueprint can spawn `StaticMeshActor`s straight out of the table once the user's MDL pipeline has produced the referenced meshes.
- Factory now writes a `<MapName>_StaticProps` DataTable next to the worldspawn mesh and the entities table when the BSP's `sprp` GameLump contains any entries. Row keys are `<index>_<basename(model)>` (de-duplicated with numeric suffixes).
- New log line: `Static props: dict=N entries=M v=V stride=S badDictRefs=K` after the `sprp` parse, plus `Created StaticProps DataTable: <path> (N rows)` after persistence.
- All bounds checks on the GameLump payload are explicit and use 64-bit math; an oversized or truncated `sprp` lump is rejected with a warning rather than read out of range. The dict cap is 65536 entries and per-entry-name reads are bounded at 128 chars.
- Note: actual MDL/VVD/VTX importing remains out of scope for this phase. The static-prop table only carries instance metadata and model paths; mesh resolution is left to a future phase or to the user's existing pipeline.

### Added — Brush-entity sub-meshes (Phase 8b)

- `FBspFile` now walks every entry in `LUMP_MODELS` (not just model 0). Worldspawn is still emitted into `Faces[0..WorldNumFaces)`; each brush sub-model (`func_door`, `func_brush`, `func_water`, `trigger_*` with renderable surfaces, etc.) is emitted contiguously into the same `Faces` array and recorded in a new `TArray<FBspBrushModel> BrushModels` exposing `(ModelIndex, FirstFace, NumFaces, Origin, Mins, Maxs)`. The face-emission inner loop was extracted into a single `EmitModelFaces` lambda so worldspawn and brush models share identical surf-flag filtering and bounds checking.
- Factory now resolves entity → brush-model index by parsing each entity's `model "*N"` keyvalue, then for every brush model with renderable faces builds a separate `UStaticMesh` sub-asset alongside the worldspawn asset (`<MapName>_BModel_<N>` in the same package path). Vertices are pivoted around the owning entity's `origin` keyvalue so spawning the actor at the entity origin places the brush correctly (and so `func_door` rotation pivots are preserved).
- `BuildMeshDescriptionFromBSP` now takes a face index range, a `bIncludeDisplacements` flag, and an `OriginOffsetSrc` pivot. Worldspawn calls it with the world face range + displacements enabled; brush models call it with their own range and displacements disabled (Source displacements are always world-only in practice).
- `FHL2Entity` gains `BrushMesh` (`FSoftObjectPath`). Brush-entity rows in the entity DataTable now point at their generated sub-mesh asset, so a Blueprint or editor utility can spawn a `StaticMeshActor` per row at `(Origin, Rotation)` and have the right geometry pre-attached.
- The shared `HL2Mat::FBuilder` and JSON `MaterialMap` are reused across worldspawn and every brush sub-model, so VMT/VTF/MIC/Texture caches dedupe across the whole import (a brush door sharing `metal/metalwall045a` with worldspawn produces one MIC, not two).
- New factory log lines: `Brush sub-models: declared=N meshes-built=M` and a roll-up `Material builder totals` after all meshes are built.

### Added — Raw-deflate pakfile support (Phase 8a)

- `HL2PakFile.cpp` now decompresses DEFLATE (method 8) entries via the engine-bundled zlib (`inflateInit2` with `windowBits = -MAX_WBITS` for raw-deflate streams as written by every PKZIP encoder, including `7z a -tzip`). Previously these entries were counted in `NumSkippedDeflate` and dropped on the floor, which silently lost most of the embedded content from any pakfile produced by anything other than stock `bspzip` (which uses STORE).
- `HL2BSPImporter.Build.cs` adds `AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib")` so the module links against UE's bundled zlib.
- `FExtractStats::NumSkippedDeflate` is preserved as a legacy counter (always 0 now) for ABI stability with anything that was reading it. DEFLATE inflate failures are reported via `NumFailed` with a per-entry warning that includes the zlib return code.
- Stock `bspzip` STORE entries continue to work via the unchanged STORE path; mixed STORE+DEFLATE archives are handled in a single pass.

## [Phase 7]

### Added — Embedded pakfile extraction (Phase 7)

- New ZIP reader (`HL2PakFile.h/.cpp`): parses the `End-Of-Central-Directory` record, walks the central directory, and extracts STORE-method (method 0) entries from the BSP's `LUMP_PAKFILE` (40) into a per-import temp directory under `<Project>/Intermediate/HL2BSPImporter/Pak/<map>_<guid>/`. Path sanitisation rejects absolute paths, drive letters, and `..` segments to prevent zip-slip. Per-entry size is capped at 256 MB.
- `FBspFile` now captures the pakfile lump bytes (decompressed if the lump itself was LZMA-packed) and exposes them via `GetPakfileBytes()`.
- `HL2Mat::FBuilder::AddExtraRoot()` lets callers prepend an additional content root searched ahead of the user-configured `SourceContentRoots`.
- `UHL2BSPImporterFactory` extracts the pakfile after parsing the BSP and adds the extract directory as the highest-priority root passed to the material builder. Community maps that ship VMTs/VTFs inside the BSP can now have those resources resolved without the user pre-staging a content tree on disk.
- DEFLATE entries (method 8) are detected, counted, and skipped with a verbose log line. Stock `bspzip` output uses STORE so this covers the common case; raw-deflate decompression is the next follow-up.

### Added — LZMA-compressed lump support (Phase 6)

- New LZMA decoder (`HL2Lzma.h/.cpp`) bundling the public-domain LZMA SDK (`LzmaDec`) under `Private/ThirdParty/Lzma/`. The SDK source is included from a single C++ wrapper TU (`LzmaDec.c.inl`) so UBT does not pick it up as a stray C compilation unit.
- `BspFile.cpp` `ReadLump` and the `LUMP_ENTITIES` text path now route through a shared `AcquireLumpBytes` helper that detects per-lump compression (`lump_t.fourCC != 0`), validates the Source 17-byte LZMA header (`'LZMA'` magic + `actualSize` + `lzmaSize` + 5-byte properties), and decompresses to an owned buffer before parsing. Compressed lumps in v19/v20 BSPs (occasionally produced by Source SDK 2013 / community tooling) are now consumed instead of failing the import.
- `Build.cs`: added `PrivateIncludePaths` entry for the module's `Private/` root so the wrapper can resolve `ThirdParty/Lzma/LzmaDec.h`.
- Sanity caps preserved: decompressed lumps are still bounded by the existing 256 MB per-lump limit; LZMA payloads with mismatched declared/decoded sizes are rejected.

### Added — Material synthesis (Phase 5)

- New VMT KeyValues parser (`HL2VmtParser.h/.cpp`): tokenises Source-format material files (`//` and `/* */` comments, quoted/bare tokens, nested blocks, last-wins keys, case-insensitive keys, `Patch { include / replace / insert }` indirection with bounded recursion).
- New VTF reader (`HL2VtfReader.h/.cpp`): parses VTF v7.0–v7.5 headers (including the v7.3+ resource directory) and decodes mip-0 / frame-0 / face-0 / slice-0 of the high-res image into 8-bit BGRA. Supported pixel formats: `BGRA8888`, `BGRX8888`, `RGBA8888`, `ABGR8888`, `ARGB8888`, `BGR888`, `RGB888`, `I8`, `A8`, `IA88`, `DXT1`, `DXT1_ONEBITALPHA`, `DXT3`, `DXT5`. Cubemap and volume layouts are stride-correct (data is skipped past) but only face/slice 0 is decoded.
- New material/texture builder (`HL2MaterialBuilder.h/.cpp`): for any face slot without an explicit `MaterialJsonPath` mapping, the importer locates `materials/<key>.vmt` under the configured `SourceContentRoots`, parses + patch-resolves it, decodes referenced `.vtf` files into `UTexture2D` assets (`TC_Normalmap` for `$bumpmap`, `sRGB=false` for normal maps), and synthesizes a `UMaterialInstanceConstant` parented to one of the user-supplied master materials. Per-import caches dedupe textures and MICs across slots; existing on-disk assets at the destination path are reused instead of overwritten.
- Parent-material picker maps Source shaders → settings slots: `LightmappedGeneric` → `LightmappedGeneric` / `..._Masked` (when `$alphatest`) / `..._Translucent` (when `$translucent`); `WorldVertexTransition` → `WorldVertexTransition`; `VertexLitGeneric` → `VertexLitGeneric`; `UnlitGeneric` → `UnlitGeneric`. MIC parameters set: `BaseColor`, `BaseColor2`, `Normal`, `Detail`, `AlphaTestRef`, `DetailScale`.
- New developer-settings: `bSynthesizeMaterials`, `SourceContentRoots` (TArray<FString>), `SynthesizedAssetRoot` (`/Game/`-rooted), and six `FSoftObjectPath` parent-material slots. Defaults documented in `Config/DefaultHL2BSPImporter.ini`.
- New `Resources/MasterMaterials/README.md` documenting the parent-material parameter contract and a suggested asset layout (`M_HL2_Lit`, `M_HL2_LitMasked`, `M_HL2_LitTranslucent`, `M_HL2_WorldVertexBlend`, `M_HL2_Unlit`).
- Factory now logs material assignment counts at the end of each import: `explicit / synthesized (created/cached/failed, tex created/cached/failed) / default`.

### Fixed — BSP parsing (Phase 1)

- Replaced the `DDispInfo` struct with the canonical 176-byte layout. The previous ~120-byte layout silently corrupted every displacement read. Added `static_assert` size checks for `FLumpInfo`, `FBspHeader`, `DVertex`, `DEdge`, `DFace`, `DTexInfo`, `DTexData`, `DModel`, `DDispInfo`, and `DDispVert`.
- Added explicit VBSP version gating. Only v19/v20 (HL2 / EP1 / EP2 / CS:S) are accepted; v21+ (CS:GO / L4D2) and big-endian `'PSBV'` (Xbox/PS3) BSPs now fail with a clear error message instead of producing garbage.
- Compressed lumps (`lump_t.fourCC != 0`, LZMA on CS:GO/L4D2 / Workshop maps) now fail loudly rather than reading raw compressed bytes as struct arrays.
- `ReadLump` validates element-size alignment, offset/length sign, end-of-file overrun, and a 256 MB per-lump sanity cap; entity text is capped at 32 MB. This neutralises a class of malicious-BSP DoS / OOB-read vectors.
- `LUMP_MODELS (14)` is now read; only model 0 (`worldspawn`) faces are emitted into the main mesh, so doors, water brushes, and other `func_*` brush-entity geometry no longer pollute the worldspawn at the origin.
- Faces whose `dtexinfo.flags` carry `SURF_NODRAW`, `SURF_SKY`, `SURF_SKY2D`, `SURF_TRIGGER`, `SURF_HINT`, or `SURF_SKIP` are filtered out. Tools brushes, the skybox shell, and trigger volumes are no longer imported as solid geometry.
- Displacement base faces are no longer double-emitted as flat brush faces (eliminates z-fighting and duplicate triangles on cliffs / terrain). Their corner positions and UVs are stashed onto the corresponding `FDispInfo` for the displacement renderer.
- Surf-edge bounds checking: `FMath::Abs(SeIdx)` replaced with an `INT32_MIN`-safe variant; `DF.FirstEdge + i` is bounds-checked against the surf-edge array, and edge / vertex indices are bounds-checked before use.
- Entity-text parser: rewritten with a bounded cursor that always advances, eliminating an infinite loop on malformed input and an out-of-bounds read on unterminated quoted values. Source bytes are now decoded with `UTF8_TO_TCHAR` instead of being widened as Latin-1.
- Lightmap UVs are now computed and stored on each `FBspVertex` from `dtexinfo.LightmapVecs` (used as channel 1 input to the static-mesh build).

### Fixed — Mesh builder (Phase 2)

- Vertex de-duplication: previously a fan-triangulated N-gon allocated `3 * (N - 2)` vertex IDs (one per triangle corner), defeating welding, smoothing, and Nanite optimisation. Now allocates one `FVertexID` + one `FVertexInstanceID` per polygon corner and reuses them across the fan.
- Triangle winding is reversed to compensate for the Y-axis flip in the Source→Unreal coordinate transform. Faces no longer appear back-face-culled / inside-out in the viewport.
- Vertex instance normals are now cleared to zero (rather than `FVector::UpVector`) before calling `FStaticMeshOperations::ComputeTangentsAndNormals`, which previously treated the up vectors as user-supplied normals and skipped recomputation.
- `ComputeTangentsAndNormals` is invoked with `Normals | Tangents | WeightedNTBs | BlendOverlappingNormals | UseMikkTSpace` so generated tangents match the runtime-baked MikkTSpace expectation.
- Added a second UV channel (`InstanceUVs.SetNumChannels(2)`) for lightmap UVs (channel 1).
- Switched from `UStaticMesh::BuildFromMeshDescriptions` (runtime build path) to the editor source-model path: `AddSourceModel` → `CreateMeshDescription` → `CommitMeshDescription` → `Build()`. This is what makes `NaniteSettings.bEnabled`, auto-lightmap-UV generation, and proper LOD compilation actually take effect.
- Build settings configured: `bRemoveDegenerates`, `bUseMikkTSpace`, `bGenerateLightmapUVs`, `SrcLightmapIndex = 1`, `DstLightmapIndex = 1`, `MinLightmapResolution = 128`. `Mesh->SetLightMapCoordinateIndex(1)` and `SetLightMapResolution(128)`.
- Material slot list is built from polygon-group slot names with matching `MaterialSlotName` and `ImportedMaterialSlotName`, so `Build()` doesn't drop or re-derive assignments.
- `CreateBodySetup()` is guarded by `if (!Mesh->GetBodySetup())` to avoid leaking a previous body setup.
- Replaced deprecated `FMeshDescription::GetVertexPositions()` with `FStaticMeshAttributes::GetVertexPositions()`.

### Fixed — Displacements (Phase 2)

- The displacement renderer now uses real base-face corner positions and UVs (previously it would synthesize an axis-aligned quad around `StartPosition`, producing visually wrong terrain).
- Displacement quad corners are rotated so the corner closest to `m_StartPosition` becomes the (0,0) of the disp grid (previously displacements were rotated 90/180/270° from intent).
- Displacement vertex offsets are scaled by `m_Dist` before transform (previously the magnitude was lost).
- Per-vertex `m_Alpha` is written to vertex-color alpha (drives `WorldVertexTransition` two-texture blends).
- Displacement triangles share vertex instances across their two triangles (no per-triangle duplication).

### Fixed — Factory & asset plumbing (Phase 3)

- Removed the global `static TMap<FString, UMaterialInterface*> GMaterialMap`. Raw, un-rooted UObject pointers across imports were a GC-dangle / crash-on-next-import risk and not thread-safe. The map is now built locally per import using `TObjectPtr<UMaterialInterface>`.
- Texture-name lookups are normalised: lowercase + backslash → forward slash. Source paths are case-insensitive and may use either separator.
- `UHL2BSPImporterFactory` constructor sets `bText = false`, `bCreateNew = false`, `bEditAfterNew = false`, `ImportPriority = DefaultImportPriority + 1`.
- The entity-table package path is now built with `FPackageName::GetLongPackagePath(InParent->GetPathName())` instead of `InParent->GetName()`. Previously `CreatePackage` was called with a short name and silently created the asset at the package root or failed.
- `UHL2EntityTable::CreateFromEntities` accepts `Name` and `EObjectFlags`; flags from the import call are propagated, with `RF_Public | RF_Standalone | RF_Transactional` added. Row names use `targetname` when present, with collision-safe suffixing.
- `FHL2Entity` UPROPERTYs were upgraded from bare `UPROPERTY()` to `UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2")` so DataTable rows are editable in the editor (the entire purpose of producing a DataTable was previously undermined).
- `GetDefault<UHL2BSPImporterSettings>()` is null-checked before dereference.
- An `FScopedSlowTask` progress dialog now wraps the import (Validate → Parse → Materials → Mesh build → StaticMesh build → Entities).

### Changed — Build / module / config (Phase 4)

- `HL2BSPImporter.Build.cs`: dropped the no-op `PrivatePCHHeaderFile`, moved `AssetRegistry`, `AssetTools`, `Json`, `JsonUtilities`, `MeshDescription`, `StaticMeshDescription`, `Projects`, `RenderCore`, `RHI` to private dependencies. Added `MeshUtilities` and `PhysicsCore`. `UnrealEd` and `DeveloperSettings` remain public (referenced from public headers).
- `HL2BSPImporter.uplugin`: added `"WhitelistTargets": ["Editor"]` to gate the module to editor builds.
- `Config/DefaultHL2BSPImporter.ini`: removed quotes from empty `MaterialJsonPath=` (some INI parsers store the literal `""` otherwise), normalised `True/False` casing, defaulted `bImportPropsAsInstances=False` (the feature is unimplemented).
- Deleted dead CVars (`hl2.scale`, `hl2.import_props`) from `HL2BSPImporterLog.cpp` — they shadowed the developer settings and confused configuration.
- Removed all `LogTemp` log calls from the factory; they now all route through `LogHL2BSPImporter`.

### Removed

- `GEMINI.md` — its content overlapped with `README.md` / `DESIGN.md` and was not maintained alongside the actual API.
