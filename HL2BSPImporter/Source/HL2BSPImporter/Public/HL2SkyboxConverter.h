#pragma once
#include "CoreMinimal.h"

class UTextureCube;
class UHL2BSPImporterSettings;

// Phase A1 — Skybox cubemap converter.
//
// Source skyboxes are authored as six independent square textures named
// `materials/skybox/<base><suffix>.vtf` where <suffix> is one of:
//   bk (back),  ft (front),  lf (left),  rt (right),  up (top),  dn (bottom).
// Each side has a sibling `<base><suffix>.vmt` that points at the .vtf via
// `$basetexture`. Source's renderer sweeps these into a cubemap on the GPU
// at draw time; for asset import we pack the six faces into a single
// UTextureCube so a UE master material can sample it via TextureCube node.
namespace HL2Sky
{
    struct FConvertParams
    {
        // Base name without face suffix, e.g. "sky_day01_05". The converter
        // expects six sibling files at materials/skybox/<BaseName>{bk,ft,...}.vmt.
        FString          BaseName;

        // Filesystem roots searched for the .vmt / .vtf pair (same convention
        // as HL2Mat::FBuilder — first hit wins).
        TArray<FString>  MaterialsRoots;

        // Long-package destination (typically <Settings.SynthesizedAssetRoot>/Skies).
        // The created UTextureCube lives at <DestPackagePath>/<BaseName>.
        FString          DestPackagePath;
    };

    enum class EConvertResult : uint8
    {
        Created,        // New UTextureCube emitted.
        Cached,         // Existing asset reused (idempotent re-import).
        MissingFaces,   // One or more side .vmt/.vtf files unresolved.
        DecodeFailed,   // VTF decode failed for at least one side.
        SaveFailed,     // Asset construction or registry registration failed.
        UnsupportedFormat, // HDR (RGBA8_CompressedHDR) or other unsupported source format.
    };

    // Convert a single skybox base name into a UTextureCube. Returns the asset
    // pointer (or nullptr on failure) and writes the outcome via OutResult.
    // Failures are warning-logged with per-face diagnostics; the caller's
    // import is not aborted.
    HL2BSPIMPORTER_API UTextureCube* ConvertSkybox(
        const FConvertParams& Params,
        EConvertResult&       OutResult,
        FString&              OutError);

    // Derive a base name (e.g. "sky_day01_05") from a sky face texture name as
    // captured by FBspFile (e.g. "skybox/sky_day01_05bk"). Strips the leading
    // directory segment and the trailing 2-char face suffix.
    HL2BSPIMPORTER_API FString DeriveBaseName(const FString& SkyFaceTextureName);
}
