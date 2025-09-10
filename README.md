# HL2 BSP Importer (UE 5.6)

Import Half-Life 2 / Source Engine BSP maps into Unreal Engine as Static Meshes. Uses the UE5 MeshDescription pipeline, preserves Source UVs, supports quad displacements, applies materials via a JSON map, and outputs an entities `UDataTable`.

---

## Features

- Import Source/HL2 `.bsp` map files as Unreal Static Meshes
- MeshDescription pipeline (UE 5.6 compatible)
- Brush UVs from Source `texinfo` projection; displacement UVs from base face
- Quad displacements (bilinear basis) with settings-aware transforms
- Material mapping via JSON (Source texture name -> UE `MaterialInterface`)
- Optional Nanite and Complex-As-Simple collision
- Outputs a `UDataTable` of parsed entities alongside the mesh

---

## Installation

1. Copy Plugin:
   Drop the `HL2BSPImporter` folder into your project's `Plugins` directory.

2. Regenerate Project Files:
   Right-click your `.uproject` and choose "Generate Visual Studio project files" (or your IDE equivalent).

3. Build:
   Open your project in Unreal Engine 5.6 and build.

---

## Usage

- In the Unreal Editor, use the Import dialog to select a `.bsp` file.
- The plugin creates a Static Mesh asset from brush and displacement geometry.
- If the map contains entities, a companion DataTable asset `<MeshName>_Entities` is created.
- Materials are assigned by matching Source texture names with entries in `HL2BSPImporter/Resources/Materials.json` or a custom `MaterialJsonPath`.
- Adjust settings in Project Settings → Plugins → HL2 BSP Importer.

---

## Configuration

Settings are available in `HL2BSPImporter/Config/DefaultHL2BSPImporter.ini` and Project Settings:

- WorldScale: World scale factor (default 2.54, inches→cm)
- bFlipYZ: Swap Y/Z before converting to Unreal (default true)
- MaterialJsonPath: leave empty to use the plugin fallback `HL2BSPImporter/Resources/Materials.json`. You can set `/Game/...` or an absolute path to a custom JSON.
- bBuildNanite: Enable Nanite for imported meshes
- bImportCollision: Use Complex-As-Simple collision on the mesh
- bImportPropsAsInstances: Reserved for future prop placement

Material JSON schema:

```
[
  { "TextureName": "concrete/concretefloor028a", "MaterialPath": "/Game/Materials/Concrete/M_ConcreteFloor_028a" }
]
```

---

## How Materials Are Resolved

1. Load JSON from `MaterialJsonPath` (supports `/Game/...` or absolute path); fallback to `HL2BSPImporter/Resources/Materials.json`.
2. For each `TextureName`, load `MaterialPath` via `FSoftObjectPath::TryLoad()`.
3. During import, polygon groups are named after Source texture names. If a name exists in the map, the corresponding material is used; otherwise, the default surface material is assigned.

---

## Diagnostics & Logging

- Logging category: `LogHL2BSPImporter`. Filter the Output Log by this category during import.
- Import dialog feedback: key steps are also printed via the import feedback context (the import window).
- Parser diagnostics:
  - If a BSP fails to load, the importer logs file existence/size and a probe of the header magic/version.
  - For valid VBSP files, the parser logs lump read issues and final counts (verts/faces/disp/ents).
- Mesh build safety:
  - Before computing normals/tangents, the importer verifies MeshDescription array sizes and triangle validity.
  - If unsafe (non-compact arrays, invalid references, or degenerate triangles), it falls back to flat normals to avoid asserts in Debug builds.

---

## Directory Layout

```
HL2BSPImporter/
├─ HL2BSPImporter.uplugin
├─ Resources/
│  ├─ Icon128.png (optional)
│  └─ Materials.json (fallback material map)
├─ Config/
│  └─ DefaultHL2BSPImporter.ini
└─ Source/
   └─ HL2BSPImporter/
      ├─ HL2BSPImporter.Build.cs
      ├─ Public/
      │  ├─ HL2BSPImporterFactory.h
      │  ├─ HL2BSPImporterTypes.h
      │  └─ BspFile.h
      └─ Private/
         ├─ HL2BSPImporter.cpp
         ├─ HL2BSPImporterFactory.cpp
         ├─ BspFile.cpp
         ├─ HL2EntityTable.cpp
         └─ HL2BSPImporterLog.cpp
```

---

## Limitations

- Displacements: only quad base faces are built (triangle support pending)
- Lightmap UVs: rely on build defaults; no explicit second UV set yet
- Materials: one material per face via texture name mapping

---

## Troubleshooting

- Empty mesh after import:
  - Ensure the `.bsp` is a Source/HL2 VBSP map (v20) and not compressed (e.g. `.bz2`).
  - Check Output Log for `LogHL2BSPImporter` messages; malformed or unsupported lumps abort import.
  - Try a small stock HL2 map to rule out content issues.

- All faces use default grey material:
  - Verify `HL2BSPImporter/Resources/Materials.json` or your `MaterialJsonPath` exists and is readable.
  - Confirm `TextureName` values match Source texture names exactly (e.g. `concrete/concretefloor028a`).
  - Confirm `MaterialPath` assets exist and load (open in Content Browser); paths should start with `/Game/`.

- Displacements look wrong or are missing:
  - Only quad-base displacements are currently supported; triangle displacements are not yet built.

- Wrong scale or orientation:
  - Adjust `WorldScale` (inches→cm default 2.54) and `bFlipYZ` in Project Settings → Plugins → HL2 BSP Importer.
  - Forward-axis Y flip is applied by design for Source→UE conversion.

- No collision:
  - Enable `bImportCollision` in settings; the importer uses Complex-As-Simple collision on the mesh.

- No entities DataTable created:
  - Some maps strip entities; verify the BSP contains the entity text lump.

---

## License

MIT License or see LICENSE file (add your license here).

---

## Credits

Created by TzeentchNET

This fork includes UE 5.6 updates and safety/diagnostics improvements to import flow and MeshDescription handling.

