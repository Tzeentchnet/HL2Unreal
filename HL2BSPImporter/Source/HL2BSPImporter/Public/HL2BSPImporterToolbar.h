#pragma once
#include "CoreMinimal.h"

class UFactory;

// Editor toolbar + bulk-import menu for the HL2 BSP Importer plugin (Tranche B1).
//
// Adds a "BSP Importer" combo button to the level editor toolbar with menu
// sections that drive bulk imports across the standalone .vtf / .vmt factories
// (Tranche B2) and a parity entry for single .bsp drag-drop.
namespace HL2Toolbar
{
    // Called from FHL2BSPImporterModule::StartupModule().
    void Register();

    // Called from FHL2BSPImporterModule::ShutdownModule().
    void Unregister();

    // Recursively scan a directory for files with the given extension (case
    // insensitive, no leading dot — e.g. "vtf"). Order is deterministic.
    HL2BSPIMPORTER_API void ScanDirectoryForExtension(const FString& AbsRoot, const TCHAR* Extension, TArray<FString>& OutAbsFiles);

    // Drive the standalone .vtf factory over a list of absolute filenames,
    // routing each file's destination to `<SynthesizedAssetRoot>/Textures/<key>`
    // (deriving `<key>` from the path's `materials/...` segment, falling back to
    // `<DefaultDestRoot>/<basename>` when no segment is present).
    // Reports per-file success/failure via the editor log and returns the count
    // of newly-created/updated assets.
    int32 BulkImportVTF(const TArray<FString>& AbsFiles);

    // Same shape as BulkImportVTF, but for the .vmt factory; assets go under
    // `<SynthesizedAssetRoot>/Materials/<key>`.
    int32 BulkImportVMT(const TArray<FString>& AbsFiles);

    // Same shape as BulkImportVTF, but for the .mdl factory; assets go under
    // `<SynthesizedAssetRoot>/Props/<sub>/<name>` derived from the file's
    // `models/<sub>/<name>.mdl` layout. The standalone factory honours the
    // path-derived destination per file so a chosen Source `models/` tree
    // mirrors into the canonical synthesized-asset layout.
    int32 BulkImportMDL(const TArray<FString>& AbsFiles);
}
