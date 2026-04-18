#include "HL2VtxReader.h"
#include "HL2BSPImporter.h"

// `.dx90.vtx` (`OptimizedModel::FileHeader_t`) parser.
//
// Hierarchy (each level is an array referenced by a count + offset relative to
// its parent):
//   FileHeader -> BodyPart[] -> Model[] -> ModelLOD[] -> Mesh[] ->
//                 StripGroup[] -> ( Vertex_t[] + Index[] (uint16) + Strip[] )
//
// Strip flags (per Source `optimize.h`):
//   STRIP_IS_LIST     0x01   indices are a flat triangle list (i*3, i*3+1, i*3+2)
//   STRIP_IS_TRISTRIP 0x02   triangulate per the normal triangle-strip rule
//
// We extract LOD 0 only.
//
// Pragma packing: MeshHeader_t / StripGroupHeader_t / StripHeader_t / Vertex_t
// are written with `#pragma pack(1)` in the engine. We use explicit field
// offsets and field-by-field reads instead of struct mirrors so the layout
// choices are documented in code rather than hidden in a compiler attribute.

namespace HL2Vtx
{
    namespace
    {
        constexpr int32 VTX_VERSION = 7;

        // FileHeader_t (default pack):
        //   0  int version
        //   4  int vertCacheSize
        //   8  uint16 maxBonesPerStrip
        //   10 uint16 maxBonesPerTri
        //   12 int maxBonesPerVert
        //   16 int checkSum
        //   20 int numLODs
        //   24 int materialReplacementListOffset
        //   28 int numBodyParts
        //   32 int bodyPartOffset
        constexpr int32 FILE_HEADER_SIZE = 36;

        // BodyPartHeader_t (default pack): { int numModels; int modelOffset; }
        constexpr int32 BODYPART_STRIDE  = 8;

        // ModelHeader_t  : { int numLODs; int lodOffset; }
        constexpr int32 MODEL_STRIDE     = 8;

        // ModelLODHeader_t: { int numMeshes; int meshOffset; float switchPoint; }
        constexpr int32 MODELLOD_STRIDE  = 12;

        // MeshHeader_t (pack 1):
        //   0  int  numStripGroups
        //   4  int  stripGroupHeaderOffset
        //   8  uint8 flags
        constexpr int32 MESH_STRIDE      = 9;

        // StripGroupHeader_t (pack 1, HL2 era — 25 bytes; newer Source adds
        // numTopologyIndices/topologyOffset, ignored here):
        //   0  int numVerts
        //   4  int vertOffset
        //   8  int numIndices
        //   12 int indexOffset
        //   16 int numStrips
        //   20 int stripOffset
        //   24 uint8 flags
        constexpr int32 STRIPGROUP_STRIDE = 25;

        // StripHeader_t (pack 1, 27 bytes):
        //   0  int numIndices
        //   4  int indexOffset
        //   8  int numVerts
        //   12 int vertOffset
        //   16 int16 numBones
        //   18 uint8 flags
        //   19 int numBoneStateChanges
        //   23 int boneStateChangeOffset
        constexpr int32 STRIP_STRIDE     = 27;

        // Vertex_t (pack 1, 9 bytes):
        //   0  uint8[3] boneWeightIndex
        //   3  uint8 numBones
        //   4  uint16 origMeshVertID
        //   6  int8[3] boneID
        constexpr int32 VTXVERT_STRIDE   = 9;

        constexpr uint8 STRIP_IS_LIST     = 0x01;
        constexpr uint8 STRIP_IS_TRISTRIP = 0x02;

        template<typename T>
        FORCEINLINE bool ReadAt(const TArray<uint8>& B, int32 Ofs, T& Out)
        {
            if (Ofs < 0 || (int64)Ofs + (int64)sizeof(T) > B.Num()) { return false; }
            FMemory::Memcpy(&Out, B.GetData() + Ofs, sizeof(T));
            return true;
        }

        FORCEINLINE int32 ReadI32(const TArray<uint8>& B, int32 Ofs, bool& bOk)
        {
            int32 V = 0; if (!ReadAt(B, Ofs, V)) { bOk = false; } return V;
        }

        FORCEINLINE uint16 ReadU16(const TArray<uint8>& B, int32 Ofs, bool& bOk)
        {
            uint16 V = 0; if (!ReadAt(B, Ofs, V)) { bOk = false; } return V;
        }
    } // namespace

    bool Parse(
        const TArray<uint8>& B,
        int32 ExpectedChecksum,
        HL2Studio::FStudioFile& File,
        FString& OutError)
    {
        if (B.Num() < FILE_HEADER_SIZE)
        {
            OutError = FString::Printf(TEXT("VTX too small (%d bytes)"), B.Num());
            return false;
        }

        bool bOk = true;
        const int32 Version          = ReadI32(B,  0, bOk);
        const int32 Checksum         = ReadI32(B, 16, bOk);
        const int32 NumLODs          = ReadI32(B, 20, bOk);
        const int32 NumBodyParts     = ReadI32(B, 28, bOk);
        const int32 BodyPartOffset   = ReadI32(B, 32, bOk);
        if (!bOk) { OutError = TEXT("VTX header read failed"); return false; }

        if (Version != VTX_VERSION)
        {
            OutError = FString::Printf(TEXT("VTX version %d unsupported (expected %d)"), Version, VTX_VERSION);
            return false;
        }
        if (Checksum != ExpectedChecksum)
        {
            OutError = FString::Printf(TEXT("VTX checksum 0x%08x != MDL checksum 0x%08x"),
                Checksum, ExpectedChecksum);
            return false;
        }
        if (NumLODs <= 0)
        {
            OutError = FString::Printf(TEXT("VTX numLODs=%d"), NumLODs);
            return false;
        }
        if (NumBodyParts != File.BodyParts.Num())
        {
            OutError = FString::Printf(
                TEXT("VTX body-part count %d does not match MDL %d"),
                NumBodyParts, File.BodyParts.Num());
            return false;
        }

        int64 TotalTriangles = 0;
        int64 TotalStripGroups = 0;

        for (int32 BpIdx = 0; BpIdx < NumBodyParts; ++BpIdx)
        {
            const int32 BpOfs = BodyPartOffset + BpIdx * BODYPART_STRIDE;
            int32 BpNumModels = 0, BpModelOffset = 0;
            if (!ReadAt(B, BpOfs + 0, BpNumModels)
             || !ReadAt(B, BpOfs + 4, BpModelOffset))
            {
                OutError = FString::Printf(TEXT("VTX bodypart %d header read failed"), BpIdx);
                return false;
            }
            HL2Studio::FStudioBodyPart& Bp = File.BodyParts[BpIdx];
            if (BpNumModels != Bp.Models.Num())
            {
                OutError = FString::Printf(
                    TEXT("VTX bodypart %d model count %d != MDL %d"),
                    BpIdx, BpNumModels, Bp.Models.Num());
                return false;
            }

            for (int32 MdlIdx = 0; MdlIdx < BpNumModels; ++MdlIdx)
            {
                const int32 ModelOfs = BpOfs + BpModelOffset + MdlIdx * MODEL_STRIDE;
                int32 MdlNumLODs = 0, LodOffset = 0;
                if (!ReadAt(B, ModelOfs + 0, MdlNumLODs)
                 || !ReadAt(B, ModelOfs + 4, LodOffset))
                {
                    OutError = FString::Printf(TEXT("VTX model %d/%d header read failed"), BpIdx, MdlIdx);
                    return false;
                }
                if (MdlNumLODs <= 0) { continue; }

                // LOD 0 only.
                const int32 LodOfs = ModelOfs + LodOffset;
                int32 LodNumMeshes = 0, MeshOffset = 0;
                if (!ReadAt(B, LodOfs + 0, LodNumMeshes)
                 || !ReadAt(B, LodOfs + 4, MeshOffset))
                {
                    OutError = FString::Printf(TEXT("VTX modelLOD %d/%d/0 read failed"), BpIdx, MdlIdx);
                    return false;
                }

                HL2Studio::FStudioModel& Model = Bp.Models[MdlIdx];
                if (LodNumMeshes != Model.Meshes.Num())
                {
                    OutError = FString::Printf(
                        TEXT("VTX model %d/%d LOD-0 mesh count %d != MDL %d"),
                        BpIdx, MdlIdx, LodNumMeshes, Model.Meshes.Num());
                    return false;
                }

                for (int32 MeshIdx = 0; MeshIdx < LodNumMeshes; ++MeshIdx)
                {
                    const int32 MeshOfs = LodOfs + MeshOffset + MeshIdx * MESH_STRIDE;
                    int32 NumStripGroups = 0, StripGroupHeaderOffset = 0;
                    if (!ReadAt(B, MeshOfs + 0, NumStripGroups)
                     || !ReadAt(B, MeshOfs + 4, StripGroupHeaderOffset))
                    {
                        OutError = FString::Printf(TEXT("VTX mesh %d/%d/%d header read failed"),
                            BpIdx, MdlIdx, MeshIdx);
                        return false;
                    }

                    HL2Studio::FStudioMesh& Mesh = Model.Meshes[MeshIdx];

                    for (int32 SgIdx = 0; SgIdx < NumStripGroups; ++SgIdx)
                    {
                        const int32 SgOfs = MeshOfs + StripGroupHeaderOffset + SgIdx * STRIPGROUP_STRIDE;
                        int32 NumVerts = 0, VertOff = 0, NumIndices = 0, IndexOff = 0,
                              NumStrips = 0, StripOff = 0;
                        if (!ReadAt(B, SgOfs + 0,  NumVerts)
                         || !ReadAt(B, SgOfs + 4,  VertOff)
                         || !ReadAt(B, SgOfs + 8,  NumIndices)
                         || !ReadAt(B, SgOfs + 12, IndexOff)
                         || !ReadAt(B, SgOfs + 16, NumStrips)
                         || !ReadAt(B, SgOfs + 20, StripOff))
                        {
                            OutError = FString::Printf(TEXT("VTX stripgroup %d/%d/%d/%d read failed"),
                                BpIdx, MdlIdx, MeshIdx, SgIdx);
                            return false;
                        }
                        if (NumVerts < 0 || NumIndices < 0 || NumStrips < 0)
                        {
                            OutError = TEXT("VTX stripgroup has negative count");
                            return false;
                        }
                        ++TotalStripGroups;

                        const int32 SgVertBase  = SgOfs + VertOff;
                        const int32 SgIndexBase = SgOfs + IndexOff;
                        const int32 SgStripBase = SgOfs + StripOff;

                        // Range-check the strip-group's own vertex/index arrays once.
                        if ((int64)SgVertBase  + (int64)NumVerts   * VTXVERT_STRIDE > B.Num()
                         || (int64)SgIndexBase + (int64)NumIndices * 2              > B.Num()
                         || (int64)SgStripBase + (int64)NumStrips  * STRIP_STRIDE   > B.Num())
                        {
                            OutError = FString::Printf(TEXT("VTX stripgroup %d/%d/%d/%d arrays OOB"),
                                BpIdx, MdlIdx, MeshIdx, SgIdx);
                            return false;
                        }

                        for (int32 StripIdx = 0; StripIdx < NumStrips; ++StripIdx)
                        {
                            const int32 StripOfs = SgStripBase + StripIdx * STRIP_STRIDE;
                            int32 SNumIdx = 0, SIdxOff = 0;
                            uint8 SFlags = 0;
                            if (!ReadAt(B, StripOfs + 0,  SNumIdx)
                             || !ReadAt(B, StripOfs + 4,  SIdxOff)
                             || !ReadAt(B, StripOfs + 18, SFlags))
                            {
                                OutError = FString::Printf(TEXT("VTX strip %d header read failed"), StripIdx);
                                return false;
                            }
                            if (SNumIdx <= 0) { continue; }
                            if ((int64)SIdxOff + SNumIdx > NumIndices)
                            {
                                OutError = FString::Printf(TEXT("VTX strip %d index range %d+%d exceeds %d"),
                                    StripIdx, SIdxOff, SNumIdx, NumIndices);
                                return false;
                            }

                            // Resolve a strip-local index slot to a mesh-local vertex index.
                            auto ResolveIndex = [&](int32 StripIndexSlot, uint32& OutMeshLocal) -> bool
                            {
                                bool bRok = true;
                                const uint16 SgVertSlot = ReadU16(B,
                                    SgIndexBase + (SIdxOff + StripIndexSlot) * 2, bRok);
                                if (!bRok || SgVertSlot >= NumVerts) { return false; }
                                uint16 OrigMeshVertID = 0;
                                if (!ReadAt(B, SgVertBase + SgVertSlot * VTXVERT_STRIDE + 4, OrigMeshVertID))
                                {
                                    return false;
                                }
                                OutMeshLocal = OrigMeshVertID;
                                return true;
                            };

                            if (SFlags & STRIP_IS_LIST)
                            {
                                const int32 NumTris = SNumIdx / 3;
                                Mesh.TriangleIndices.Reserve(Mesh.TriangleIndices.Num() + NumTris * 3);
                                for (int32 t = 0; t < NumTris; ++t)
                                {
                                    uint32 A=0, B0=0, C=0;
                                    if (!ResolveIndex(t * 3 + 0, A)
                                     || !ResolveIndex(t * 3 + 1, B0)
                                     || !ResolveIndex(t * 3 + 2, C))
                                    {
                                        OutError = TEXT("VTX triangle-list index resolution failed");
                                        return false;
                                    }
                                    Mesh.TriangleIndices.Add(A);
                                    Mesh.TriangleIndices.Add(B0);
                                    Mesh.TriangleIndices.Add(C);
                                }
                                TotalTriangles += NumTris;
                            }
                            else if (SFlags & STRIP_IS_TRISTRIP)
                            {
                                const int32 NumTris = (SNumIdx >= 3) ? (SNumIdx - 2) : 0;
                                Mesh.TriangleIndices.Reserve(Mesh.TriangleIndices.Num() + NumTris * 3);
                                for (int32 t = 0; t < NumTris; ++t)
                                {
                                    uint32 A=0, B0=0, C=0;
                                    if (!ResolveIndex(t + 0, A)
                                     || !ResolveIndex(t + 1, B0)
                                     || !ResolveIndex(t + 2, C))
                                    {
                                        OutError = TEXT("VTX triangle-strip index resolution failed");
                                        return false;
                                    }
                                    // Standard tristrip winding: alternate CW/CCW by parity.
                                    if (t & 1) { Mesh.TriangleIndices.Add(A); Mesh.TriangleIndices.Add(C);  Mesh.TriangleIndices.Add(B0); }
                                    else        { Mesh.TriangleIndices.Add(A); Mesh.TriangleIndices.Add(B0); Mesh.TriangleIndices.Add(C);  }
                                }
                                TotalTriangles += NumTris;
                            }
                            else
                            {
                                UE_LOG(LogHL2BSPImporter, Verbose,
                                    TEXT("VTX strip flags 0x%02x neither LIST nor TRISTRIP; skipping."), SFlags);
                            }
                        }
                    }
                }
            }
        }

        UE_LOG(LogHL2BSPImporter, Verbose,
            TEXT("VTX parsed: %d bodyparts, %lld stripgroups, %lld triangles."),
            NumBodyParts, (long long)TotalStripGroups, (long long)TotalTriangles);
        return true;
    }
}
