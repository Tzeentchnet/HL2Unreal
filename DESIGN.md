# HL2 BSP Importer — Design Reference

This document describes the architecture, data flow, and key design decisions for the HL2 BSP Importer Unreal Engine plugin. It serves as a technical reference for maintainers and contributors.

## Overview

The plugin imports Half-Life 2 / Source Engine VBSP maps (`.bsp`) directly into Unreal Engine as Static Mesh assets. It parses brush geometry, resolves Source material names to Unreal materials, builds a mesh via the MeshDescription pipeline, and optionally generates a companion DataTable with parsed entity data. Displacements are supported for quad faces.

Primary goals:

- Use UE 5.6–compatible MeshDescription APIs (no RawMesh)
- Preserve Source UVs via `texinfo` projection
- Map Source texture names to UE materials via JSON
- Support displacements (quad), configurable scale/axis
- Output entities to a DataTable for downstream tooling

## Module Layout

- Plugin descriptor: `HL2BSPImporter/HL2BSPImporter.uplugin`
- Module: `HL2BSPImporter` (Editor)
  - Public headers: `HL2BSPImporter/Source/HL2BSPImporter/Public`
  - Private sources: `HL2BSPImporter/Source/HL2BSPImporter/Private`
  - Build rules: `HL2BSPImporter/Source/HL2BSPImporter/HL2BSPImporter.Build.cs`

Key files:

- Import factory: `.../Private/HL2BSPImporterFactory.cpp`, `.../Public/HL2BSPImporterFactory.h`
- BSP reader: `.../Private/BspFile.cpp`, `.../Public/BspFile.h`
- Settings: `.../Public/HL2BSPImporterSettings.h` (+ default config in `Config/DefaultHL2BSPImporter.ini`)
- Entities DataTable: `.../Private/HL2EntityTable.cpp`, `.../Public/HL2EntityTable.h`
- Types: `.../Public/HL2BSPImporterTypes.h`
- Module bootstrap + log category: `.../Private/HL2BSPImporter.cpp`

## Build & Dependencies

- UE 5.6 target, Editor module
- MeshDescription stack:
  - `MeshDescription`, `StaticMeshDescription`, `StaticMeshAttributes`, `StaticMeshOperations`
- Editor/runtime support:
  - `UnrealEd`, `AssetRegistry`, `Projects`, `Json`, `JsonUtilities`, `RenderCore`, `RHI`, `AssetTools`, `DeveloperSettings`

Configured in `HL2BSPImporter.Build.cs`.

## Import Data Flow

1. `UHL2BSPImporterFactory::FactoryCanImport(Filename)`
   - Filters by `.bsp` extension.
2. `UHL2BSPImporterFactory::FactoryCreateFile(...)`
   - Logs preflight info (file exists/size, header probe identifier/version).
   - Parses BSP via `FBspFile::LoadFromFile` (returns false on any lump/format error).
   - Loads material map JSON ? `TMap<FString, UMaterialInterface*>`.
   - Builds `FMeshDescription` from parsed faces and displacements.
   - Validates MeshDescription (array sizes, triangle references, degenerates); computes normals/tangents or falls back to flat normals if unsafe.
   - Creates `UStaticMesh` in `InParent` with `Flags` and builds from MeshDescriptions.
   - Applies Nanite/collision settings; registers assets; creates companion `UHL2EntityTable` if entities are present.

## BSP Reader (VBSP v20)

File: `BspFile.cpp`

- Validates header `Ident == 'VBSP'`. Logs version and map revision.
- Reads lumps (with bounds checks):
  - Vertices (3): `DVertex[Num]`
  - Edges (12): `DEdge[Num]`
  - SurfEdges (13): `int32[Num]` (signed refs into edges with direction)
  - Faces (7): `DFace[Num]` (references `FirstEdge`, `NumEdges`, `TexInfo`, `DispInfo`)
  - TexInfo (6): `DTexInfo[Num]` (texture and lightmap vectors, `TexData` index)
  - TexData (2): `DTexData[Num]` (texture size, string table id)
  - Texture string table (43) and data (44) for material name resolution
- Geometry assembly:
  - For each face, iterate `NumEdges` via `SurfEdges[FirstEdge + i]` and build a polygon loop.
  - Compute per-vertex UV using `TexInfo.TextureVecs` and normalize by `DTexData.{Width,Height}`.
  - Store `FBspVertex { Position, UV }` and `FBspFace { FirstVertex, NumVertices, TextureName }`.
- Displacements (partial):
  - Read `LUMP_DISPINFO` (26) and `LUMP_DISP_VERTS` (33), store `FDispInfo { Power, VertStart, MapFace }` and `FDispVert { Vector[3] }`.
- Entities:
  - Read entity text lump (0), parse `{ "key" "value" ... }` blocks.
  - Extract `targetname`, `classname`, `origin`, `angles`, `model` into `FHL2Entity`.
- Diagnostics: logs lump read failures and summary counts for maintainability.

## Coordinate System & Units

- Source ? Unreal:
  - Optional Y/Z swap via `UHL2BSPImporterSettings::bFlipYZ`.
  - Flip Y sign to convert handedness/forward axis.
  - Scale by `WorldScale` (default 2.54: inches?centimeters).
- Transform helpers in factory: `TransformPos`, `TransformDir`.

UVs are computed in Source space and remain valid under linear transforms.

## Mesh Construction

File: `HL2BSPImporterFactory.cpp`

- `FMeshDescription` with `FStaticMeshAttributes`:
  - Vertex positions, vertex-instance normals/tangents/binormal signs/colors, UVs (1 channel).
- Polygon groups by Source texture name:
  - Map each unique face `TextureName` ? `FPolygonGroupID` and store slot name in polygon group attributes.
- Triangulation:
  - Fan-triangulate polygons: `(0,1,2) (0,2,3) ...`.
- Normals/Tangents (UE 5.6):
  - If arrays are compact and triangles valid, call `FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents)`.
  - Otherwise, generate flat face normals as a safe fallback (avoids Debug asserts/breakpoints).
- StaticMesh build:
  - Fill `StaticMaterials` in the same order as polygon groups.
  - Build via `BuildFromMeshDescriptions({ &MD })`.

## Displacements

Parsing:

- `FDispInfo.Power` ? `Side = (1 << Power) + 1`.
- `VertStart` indexes `DispVerts` (length `Side * Side`).
- `MapFace` links to base face (must be a quad for current implementation).

Building (current):

- Requires quad base face.
- Sample bilinear position across the base quad (corners 0..1), add transformed displacement offset.
- Build a vertex grid and triangulate cells into two triangles.
- UVs are bilinearly interpolated from the base face’s four corner UVs.
- Assign triangles to the polygon group of the base face’s texture.

Future extensions:

- Triangle base faces with barycentric interpolation.
- Blend normals across displacement grids prior to tangent calc.

## Materials & Mapping

Material map loader (factory):

- Reads JSON file defined by `UHL2BSPImporterSettings::MaterialJsonPath`.
  - Accepts `/Game/...` paths (resolved to `Content/...`) or absolute filesystem paths.
  - Fallback: `Plugins/HL2BSPImporter/Resources/Materials.json`.
- JSON schema (array of objects):

```json
[
  { "TextureName": "concrete/concretefloor028a", "MaterialPath": "/Game/Materials/Concrete/M_ConcreteFloor_028a" }
]
```

- Loads materials via `FSoftObjectPath::TryLoad()`; builds a `TMap<FString, UMaterialInterface*>`.
- During mesh build: polygon group slot names drive material slots; default surface material used if no mapping.

## Entities Output

- After mesh creation, if BSP contained entities, create `UHL2EntityTable` alongside the mesh (`<MeshName>_Entities`).
- Table row structure includes `FHL2Entity { Name, Class, Origin, Rotation, Model }`.

## Settings

Class: `UHL2BSPImporterSettings` (Developer Settings)

- `WorldScale` (float): inches?cm default 2.54.
- `bFlipYZ` (bool): swap Y/Z axes before Y-flip.
- `MaterialJsonPath` (string): material mapping JSON path. Leave empty to use plugin fallback `Resources/Materials.json`.
- `bBuildNanite` (bool): enables Nanite for imported mesh.
- `bImportCollision` (bool): sets `CTF_UseComplexAsSimple` collision on the mesh.
- `bImportPropsAsInstances` (bool): reserved for future prop placement.

Defaults in `HL2BSPImporter/Config/DefaultHL2BSPImporter.ini`.

## Collision & Nanite

- Nanite: `Mesh->NaniteSettings.bEnabled = bBuildNanite` prior to build.
- Collision: if `bImportCollision`, create BodySetup and set `CTF_UseComplexAsSimple`.

## Logging & Diagnostics

- Log category: `LogHL2BSPImporter` (defined in module `.cpp`).
- Factory logs import lifecycle, file preflight (exists/size/header), and MeshDescription validation.
- Parser logs header/lump read failures and summary counts.
- Import feedback: key steps are mirrored to `FFeedbackContext* Warn` for visibility in the import UI.

## Error Handling

- BSP reader validates header and lump bounds (`ReadArray` checks) and bails on errors.
- Material map loader tolerates absent or malformed JSON (returns empty map, logs warnings).
- Import factory returns `nullptr` on BSP read failure; uses safe normals fallback if MeshDescription is unsafe.

## Limitations

- Displacements: only quad base faces are built; triangle support pending.
- Lightmap UVs: rely on build defaults; no explicit custom lightmap layer.
- Vertex reuse: vertices are currently not deduplicated across triangles for simplicity.
- Materials: one material per face via texture name.

## Future Work

- Triangle displacement building using barycentric basis.
- Improved smoothing across displacement grids prior to tangent calc.
- Lightmap UV generation control (BuildSettings) and LODs.
- Entity-driven prop placement using `UHL2EntityTable`.
- Async import path and progress reporting for large maps.

## Testing Notes

- Validate with a small HL2 map containing mixed materials, a displacement quad, and entities.
- Verify material resolution, UV continuity, Nanite/collision flags, and entities table contents.
