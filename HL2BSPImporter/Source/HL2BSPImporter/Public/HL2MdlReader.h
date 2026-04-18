#pragma once
#include "CoreMinimal.h"
#include "HL2StudioTypes.h"

namespace HL2Mdl
{
    /**
     * Parse a Source `.mdl` (`studiohdr_t`) file and populate the body-part /
     * model / mesh hierarchy and material candidate list. Only LOD 0 is kept.
     * Vertex positions / normals / UVs come from the matching `.vvd`; triangle
     * index lists come from the matching `.dx90.vtx` — both are populated by
     * subsequent reader passes (see HL2VvdReader and HL2VtxReader).
     *
     * Supported `studiohdr_t` versions: 44..49 (HL2 → Source 2013). On a
     * version mismatch the reader fails with a clear error rather than reading
     * out-of-bounds.
     *
     * @param Bytes      Raw .mdl file bytes.
     * @param OutFile    Filled on success. MaterialCandidates is sized to
     *                   `numtextures`; each entry contains every `cdtex/name`
     *                   permutation Source would try for that texture slot
     *                   (lower-cased, forward-slashed).
     * @param OutError   On failure, a short diagnostic.
     */
    HL2BSPIMPORTER_API bool Parse(
        const TArray<uint8>& Bytes,
        HL2Studio::FStudioFile& OutFile,
        FString& OutError);
}
