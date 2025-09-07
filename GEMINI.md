# Project: HL2 BSP Importer for Unreal Engine

## Project Overview

This project is an Unreal Engine 5.6 plugin that imports Half-Life 2 / Source Engine BSP map files (`.bsp`) into the editor as `UStaticMesh` assets. It uses the modern `MeshDescription` pipeline, preserving UVs, handling quad-based displacements, and mapping Source Engine textures to Unreal materials via a JSON file. It also extracts entity information from the map and saves it as a `UDataTable` asset.

The plugin is written in C++ and is designed to be a robust and configurable tool for bringing classic Source Engine levels into Unreal Engine for modern rendering and gameplay.

**Key Technologies:**

*   **Engine:** Unreal Engine 5.6
*   **Language:** C++
*   **Core APIs:** `MeshDescription`, `StaticMeshDescription`, `UnrealEd` (for the import factory)
*   **Configuration:** `.ini` files and a custom JSON format for material mapping.

**Architecture:**

The plugin is structured as a single "Editor" module (`HL2BSPImporter`). The core components are:

*   **`UHL2BSPImporterFactory`:** The main entry point for the import process. It handles file type recognition, calls the BSP parser, and orchestrates the creation of the `UStaticMesh` and `UDataTable` assets.
*   **`FBspFile`:** A parser for the VBSP file format (version 20). It reads lumps containing geometry, textures, and entity data.
*   **`UHL2BSPImporterSettings`:** A `UDeveloperSettings` class that exposes configuration options to the Project Settings UI, such as world scale, axis flipping, and the path to the material mapping JSON.
*   **`HL2EntityTable`:** A helper class for creating the `UDataTable` asset from the parsed entity data.

## Building and Running

**Prerequisites:**

*   Unreal Engine 5.6
*   A C++ compiler compatible with Unreal Engine (e.g., Visual Studio 2022)

**Building the Plugin:**

1.  **Installation:** Copy the `HL2BSPImporter` folder into the `Plugins` directory of your Unreal Engine project.
2.  **Generate Project Files:** Right-click on your `.uproject` file and select "Generate Visual Studio project files" (or the equivalent for your IDE).
3.  **Build:** Open the project in Unreal Engine and compile.

**Running the Importer:**

1.  Open your Unreal Engine project.
2.  In the Content Browser, click the "Import" button.
3.  Select a Half-Life 2 `.bsp` file.
4.  The plugin will create a `UStaticMesh` asset and, if entities are present, a `UDataTable` asset named `<MeshName>_Entities`.

## Development Conventions

*   **Coding Style:** The code follows the standard Unreal Engine coding conventions (e.g., `PascalCase` for classes and methods, `b` prefix for booleans).
*   **Dependencies:** All external dependencies are managed through the `.Build.cs` file, linking against standard Unreal Engine modules.
*   **Configuration:** Plugin settings are managed through a `UDeveloperSettings` class, with defaults in `Config/DefaultHL2BSPImporter.ini`.
*   **Material Mapping:** A JSON file (`Resources/Materials.json`) is used to map Source Engine texture names to Unreal `MaterialInterface` paths. This file can be customized by the user.
*   **Error Handling:** The plugin performs basic validation of the BSP file and will log errors to the Output Log if issues are encountered.
*   **Testing:** The `DESIGN.md` file suggests manual testing with a small HL2 map that includes a variety of features (brushes, displacements, entities) to verify the importer's functionality.
