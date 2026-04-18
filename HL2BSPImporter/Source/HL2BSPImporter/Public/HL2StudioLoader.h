#pragma once
#include "CoreMinimal.h"
#include "HL2StudioTypes.h"

namespace HL2Studio
{
    /**
     * Locate `<root>/<modelPath>.mdl` (and the matching `.vvd` + `.dx90.vtx`
     * siblings) under the configured content roots, parse all three, validate
     * cross-file checksums, and return a fully populated FStudioFile (vertex
     * stream + per-mesh triangle indices).
     *
     * @param ModelPath  Source-style relative path, e.g.
     *                   "models/props_c17/oildrum001.mdl". Case-insensitive;
     *                   forward / backward slashes accepted. The `.mdl`
     *                   extension is added if missing.
     * @param Roots      Filesystem directories searched in order. Each is
     *                   expected to contain a `models/` subtree (i.e. the same
     *                   roots passed to HL2Mat::FBuilder).
     * @param OutFile    Filled on success.
     * @param OutError   On failure, a short diagnostic.
     *
     * @return true if all three files were located, parsed, checksums match,
     *         and at least one triangle was produced.
     */
    HL2BSPIMPORTER_API bool LoadModel(
        const FString& ModelPath,
        const TArray<FString>& Roots,
        FStudioFile& OutFile,
        FString& OutError);
}
