#pragma once
#include "CoreMinimal.h"
#include "HL2StudioTypes.h"

namespace HL2Vtx
{
    /**
     * Parse a Source `.dx90.vtx` (`OptimizedModel::FileHeader_t`) and append
     * triangle index lists to the meshes of the supplied FStudioFile.
     *
     * VTX hierarchy is BodyPart → Model → ModelLOD → Mesh → StripGroup →
     * (Strip + Vertex array + Index array). We extract LOD 0 only. Strips
     * that are encoded as triangle strips (rather than triangle lists) are
     * triangulated. Each VTX vertex's `origMeshVertID` is the offset within
     * the mesh's vertex range in the post-fixup VVD array; combined with the
     * mesh's VertexOffset (from the MDL) and the model's VertexBase, this
     * gives a global index into FStudioFile::Vertices.
     *
     * The supplied FStudioFile must already have its body-part / model / mesh
     * skeleton populated by HL2Mdl::Parse, with sizes matching what the VTX
     * declares (asserted; mismatched files fail with a clear error).
     *
     * Bone state changes, flex deltas, and material-replacement lists are all
     * walked-past but not consumed.
     *
     * @param Bytes      Raw .dx90.vtx file bytes.
     * @param Checksum   Studio file's checksum; must match the VTX checksum.
     * @param InOutFile  FStudioFile populated by HL2Mdl::Parse; this call adds
     *                   triangle indices to each mesh.
     * @param OutError   On failure, a short diagnostic.
     */
    HL2BSPIMPORTER_API bool Parse(
        const TArray<uint8>& Bytes,
        int32 Checksum,
        HL2Studio::FStudioFile& InOutFile,
        FString& OutError);
}
