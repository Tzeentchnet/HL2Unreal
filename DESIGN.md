# HL2 BSP Importer – Design Reference

This document describes the architecture, data flow, and key design decisions for the HL2 BSP Importer Unreal Engine plugin. It serves as a technical reference for maintainers and contributors.

## Overview

The plugin imports Half‑Life 2/Source Engine VBSP maps (.bsp) directly into Unreal Engine as Static Mesh assets. It parses brush geometry, resolves Source material names to Unreal materials, builds a mesh via the MeshDescription pipeline, and optionally generates a companion DataTable with parsed entity data. Displacements are supported for quad faces.

Primary goals:

- Use UE5.6‑compatible MeshDescription APIs (no RawMesh)
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
- Settings: `.../Public/BL2BSPImporterSettings.h` (+ default config in `Config/DefaultHL2BSPImporter.ini`)
- Entities DataTable: `.../Private/HL2EntityTable.cpp`, `.../Public/HL2EntityTable.h`
- Types: `.../Public/HL2BSPImporterTypes.h`
- CVars/bootstrap: `.../Private/HL2BSPImporterLog.cpp`, `.../Private/HL2BSPImporter.cpp`

## Build & Dependencies

- UE 5.6 target, Editor module
- MeshDescription path:
  - `MeshDescription`, `StaticMeshDescription`, `StaticMeshAttributes`, `StaticMeshOperations`
- Editor/runtime support:
  - `UnrealEd`, `AssetRegistry`, `Projects`, `Json`, `JsonUtilities`, `RenderCore`, `RHI`

Configured in `HL2BSPImporter.Build.cs`.

## Import Data Flow

1. `UHL2BSPImporterFactory::FactoryCreateFile(...)`
   - Validates `.bsp` extension (`FactoryCanImport`)
   - Parses BSP via `FBspFile::LoadFromFile`
   - Loads material map JSON → `TMap<FString, UMaterialInterface*>`
   - Builds `FMeshDescription` from parsed faces and displacements
   - Creates `UStaticMesh` and builds from MeshDescriptions
   - Applies Nanite/collision settings
   - Registers assets with `AssetRegistry`
   - Creates companion `UHL2EntityTable` if BSP contains entities

## BSP Reader (VBSP v20)

File: `BspFile.cpp`

- Validates header `Ident == 'VBSP'`
- Reads lumps:
  - Vertices (3): `DVertex[Num]`
  - Edges (12): `DEdge[Num]`
  - SurfEdges (13): `int32[Num]` (signed refs into edges with direction)
  - Faces (7): `DFace[Num]` (references `FirstEdge`, `NumEdges`, `TexInfo`, `DispInfo`)
  - TexInfo (6): `DTexInfo[Num]` (texture and lightmap vectors, `TexData` index)
  - TexData (2): `DTexData[Num]` (texture size, string table id)
  - Texture string table (43) and data (44) for material name resolution

Geometry assembly:

- For each face, iterates `NumEdges` via `SurfEdges[FirstEdge + i]`
  - Resolves to an oriented vertex index (edge v0 or v1)
  - Builds a polygon vertex loop
- Computes per‑vertex UV using `TexInfo.TextureVecs[2][4]` (projected), normalized by `DTexData.{Width,Height}`
- Stores:
  - `FBspVertex { Position (Source units), UV }`
  - `FBspFace { FirstVertex, NumVertices, TextureName }`

Displacements (partial):

- Reads `LUMP_DISPINFO` (26) and `LUMP_DISP_VERTS` (33)
- Stores `FDispInfo { Power, VertStart, MapFace }` and `FDispVert { Vector[3] }`

Entities:

- Reads entity text lump (0) and parses blocks `{ "key" "value" ... }`
- Extracts `targetname`, `classname`, `origin`, `angles`, `model` into `FHL2Entity`

## Coordinate System & Units

- Source → Unreal:
  - Optional Y/Z swap via `UHL2BSPImporterSettings::bFlipYZ`
  - Flip Y sign to convert handedness/forward axis
  - Scale by `WorldScale` (defaults to 2.54, i.e., inches → centimeters)
- Transform helpers in factory:
  - `TransformPos`, `TransformDir`

Note: BSP stores vertices in Source space; UVs are computed in Source space and remain valid under linear transforms.

## Mesh Construction

File: `HL2BSPImporterFactory.cpp`

- Uses `FMeshDescription` with `FStaticMeshAttributes`:
  - Registers vertex positions, vertex‑instance normals/tangents/binormal signs/colors, UVs (1 channel)
- Polygon groups by Source texture name:
  - Maps each unique face `TextureName` → `FPolygonGroupID`
  - Stores material slot names in polygon group attribute
- Triangulation:
  - Fan‑triangulates N‑gons: `(0,1,2) (0,2,3) ...`
- Normals/Tangents:
  - Computed via `FStaticMeshOperations::ComputeTangentsAndNormals`
- StaticMesh build:
  - Material slots filled in the same order as polygon groups (by name)
  - `BuildFromMeshDescriptions({ &MeshDesc })`

## Displacements

Parsing:

- `FDispInfo.Power` determines grid side: `Side = (1 << Power) + 1`
- `VertStart` indexes `DispVerts` (length `Side*Side`)
- `MapFace` links to the base face, which must be a quad for current implementation

Building (current):

- Requires quad base face
- Samples bilinear position across the base quad (corners 0..1), then adds transformed displacement offset
- Builds a vertex grid and triangulates cells into two triangles
- UVs are interpolated bilinearly from the base face’s four corner UVs
- Assigns triangles to the polygon group of the base face’s texture

Potential extensions:

- Triangle base faces with barycentric interpolation
- Use Source displacement orientation for more accurate basis vectors
- Blend normals across displacement grid prior to tangent calculation

## Materials & Mapping

Material map loader (factory):

- Reads JSON file defined by `UHL2BSPImporterSettings::MaterialJsonPath`
  - Accepts `/Game/...` paths (resolved to `Content/...`), or absolute filesystem path
  - Fallback: `Plugins/HL2BSPImporter/Resources/Materials.json`
- JSON schema (array of objects):

```json
[
  { "TextureName": "concrete/concretefloor028a", "MaterialPath": "/Game/Materials/Concrete/M_ConcreteFloor_028a" }
]
```

- For each entry: loads material via `FSoftObjectPath::TryLoad()` and stores in `TMap<FString, UMaterialInterface*>`
- During mesh build: polygon groups’ slot names drive `StaticMaterials`, resolved via the map, else default surface

## Entities Output

- After mesh creation, if BSP contained entities, the importer creates a `UHL2EntityTable` asset alongside the mesh (`<MeshName>_Entities`)
- Table structure (`FHL2EntityTableRow`) includes `FHL2Entity { Name, Class, Origin, Rotation, Model }`
- Purpose: downstream tools can spawn actors/instances, build gameplay logic, or perform lookups

## Settings

Class: `UHL2BSPImporterSettings` (Developer Settings)

- `WorldScale` (float): inches→cm default 2.54
- `bFlipYZ` (bool): swap Y/Z axes before Y‑flip
- `MaterialJsonPath` (string): material mapping JSON path (supports `/Game/...`)
- `bBuildNanite` (bool): enables Nanite for imported mesh
- `bImportCollision` (bool): sets `CTF_UseComplexAsSimple` collision on the mesh
- `bImportPropsAsInstances` (bool): reserved for future prop placement

Config defaults in `HL2BSPImporter/Config/DefaultHL2BSPImporter.ini`.

## Collision & Nanite

- Nanite: `Mesh->NaniteSettings.bEnabled = bBuildNanite` prior to build
- Collision: if `bImportCollision`, creates BodySetup and sets `CTF_UseComplexAsSimple`

## Logging & CVars

File: `HL2BSPImporterLog.cpp`

- CVars (available in editor console):
  - `hl2.scale` (float, default 2.54): matches `WorldScale`
  - `hl2.import_props` (int32): reserved for future prop import modes

## Error Handling

- BSP reader validates header and lump bounds (`ReadArray` range checks)
- Material map loader tolerates absent or malformed JSON (returns empty map)
- Import factory returns `nullptr` on BSP read failure to abort asset creation cleanly

## Limitations

- Displacements: only quad base faces are built; triangle support pending
- Lightmap UVs: rely on build defaults; no explicit custom lightmap layer
- Vertex reuse: current triangulation duplicates vertices per triangle for simplicity; MeshDescription build is robust but not deduplicated
- Material assignment: one material per face via texture name; complex multi‑material faces (rare) are not considered

## Future Work

- Triangle displacement building using barycentric basis
- Improved smoothing across displacement grids prior to tangent calc
- Lightmap UV generation control (BuildSettings) and LODs
- Entity‑driven prop placement (instanced static meshes or actors) using `UHL2EntityTable`
- Packed collision or simple collision proxy generation
- Async import path and progress reporting for large maps

## Testing Notes

- Recommended to validate with a small HL2 map containing:
  - Mixed brush faces with multiple materials
  - At least one displacement quad
  - A handful of entities (e.g., `info_player_start`, `prop_static`)
- Verify:
  - Material resolution via JSON mapping
  - UV continuity across faces and displacements
  - Nanite flag and collision settings
  - Entities table correctness

