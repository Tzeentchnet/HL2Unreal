#pragma once
#include "CoreMinimal.h"
#include "HL2StudioTypes.h"

namespace HL2Vvd
{
    /**
     * Parse a Source `.vvd` (`vertexFileHeader_t`) and emit the LOD-0 vertex
     * stream. The fixup table is honoured: when present, it specifies how the
     * raw on-disk vertex array is re-assembled for each LOD, and we walk it for
     * `lod >= 0` to rebuild the LOD-0 view.
     *
     * Tangent data is decoded if present (`tangentDataStart != 0`). Bone
     * weights are skipped — static props don't need them and the vertex
     * format always reserves 16 bytes for the boneweights regardless.
     *
     * @param Bytes      Raw .vvd file bytes.
     * @param Checksum   Studio file's checksum; must match the .vvd checksum
     *                   or the parse fails (the file is from a different MDL).
     * @param OutVertices  Filled on success — one entry per LOD-0 vertex.
     * @param OutError   On failure, a short diagnostic.
     */
    HL2BSPIMPORTER_API bool Parse(
        const TArray<uint8>& Bytes,
        int32 Checksum,
        TArray<HL2Studio::FStudioVertex>& OutVertices,
        FString& OutError);
}
