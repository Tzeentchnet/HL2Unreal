#include "HL2VvdReader.h"
#include "HL2BSPImporter.h"

// vertexFileHeader_t — Source vertex stream container. One file per .mdl,
// shared across all LODs (the fixup table re-orders the raw vertex array for
// each LOD).
//
// On-disk layout (v4):
//   0  : int id          ('IDSV' = 0x56534449)
//   4  : int version     (4)
//   8  : int checksum    (must match studiohdr_t.checksum)
//   12 : int numLODs
//   16 : int numLODVertexes[MAX_NUM_LODS = 8]   (LOD-N vertex counts)
//   48 : int numFixups
//   52 : int fixupTableStart                     (file offset)
//   56 : int vertexDataStart                     (file offset)
//   60 : int tangentDataStart                    (file offset; 0 = no tangents)
//
// vertex stride = sizeof(mstudiovertex_t) = 48 bytes:
//   0  : mstudioboneweight_t  (16 bytes; 3 floats + 3 bytes bone + 1 byte numbones)
//   16 : Vector position      (12)
//   28 : Vector normal        (12)
//   40 : Vector2D uv          (8)
//
// vertexFileFixup_t (12 bytes each at fixupTableStart):
//   0  : int lod              (LOD this fixup belongs to)
//   4  : int sourceVertexID   (start index into the raw on-disk vertex array)
//   8  : int numVertexes      (run length)
//
// LOD reconstruction: for the requested LOD L, walk fixups where (lod >= L)
// and concatenate their [sourceVertexID, sourceVertexID+numVertexes) ranges
// into the output. When numFixups == 0 the on-disk array is already in
// correct order for every LOD.

namespace HL2Vvd
{
    namespace
    {
        constexpr uint32 VVD_ID            = 0x56534449u; // 'IDSV'
        constexpr int32  VVD_VERSION       = 4;
        constexpr int32  VERTEX_STRIDE     = 48;
        constexpr int32  TANGENT_STRIDE    = 16;
        constexpr int32  FIXUP_STRIDE      = 12;
        constexpr int32  MAX_NUM_LODS      = 8;
        constexpr int32  HEADER_FIXED_SIZE = 64;
        constexpr int32  MAX_VERTICES      = 524288;     // sanity cap

        template<typename T>
        FORCEINLINE bool ReadAt(const TArray<uint8>& B, int32 Ofs, T& Out)
        {
            if (Ofs < 0 || (int64)Ofs + (int64)sizeof(T) > B.Num()) { return false; }
            FMemory::Memcpy(&Out, B.GetData() + Ofs, sizeof(T));
            return true;
        }
    }

    bool Parse(
        const TArray<uint8>& B,
        int32 ExpectedChecksum,
        TArray<HL2Studio::FStudioVertex>& Out,
        FString& OutError)
    {
        Out.Reset();

        if (B.Num() < HEADER_FIXED_SIZE)
        {
            OutError = FString::Printf(TEXT("VVD too small (%d bytes)"), B.Num());
            return false;
        }

        uint32 Id = 0; int32 Version = 0, Checksum = 0, NumLODs = 0;
        ReadAt(B, 0,  Id);
        ReadAt(B, 4,  Version);
        ReadAt(B, 8,  Checksum);
        ReadAt(B, 12, NumLODs);

        if (Id != VVD_ID)
        {
            OutError = FString::Printf(TEXT("VVD bad magic 0x%08x (expected 'IDSV' 0x%08x)"), Id, VVD_ID);
            return false;
        }
        if (Version != VVD_VERSION)
        {
            OutError = FString::Printf(TEXT("VVD version %d unsupported (expected %d)"), Version, VVD_VERSION);
            return false;
        }
        if (Checksum != ExpectedChecksum)
        {
            OutError = FString::Printf(TEXT("VVD checksum 0x%08x != MDL checksum 0x%08x"),
                Checksum, ExpectedChecksum);
            return false;
        }
        if (NumLODs <= 0 || NumLODs > MAX_NUM_LODS)
        {
            OutError = FString::Printf(TEXT("VVD numLODs %d out of range"), NumLODs);
            return false;
        }

        int32 LODVerts[MAX_NUM_LODS] = {};
        for (int32 i = 0; i < MAX_NUM_LODS; ++i)
        {
            ReadAt(B, 16 + i * 4, LODVerts[i]);
        }

        int32 NumFixups = 0, FixupStart = 0, VertexStart = 0, TangentStart = 0;
        ReadAt(B, 48, NumFixups);
        ReadAt(B, 52, FixupStart);
        ReadAt(B, 56, VertexStart);
        ReadAt(B, 60, TangentStart);

        const int32 LOD0Count = LODVerts[0];
        if (LOD0Count <= 0 || LOD0Count > MAX_VERTICES)
        {
            OutError = FString::Printf(TEXT("VVD LOD-0 vertex count %d out of range"), LOD0Count);
            return false;
        }
        if (NumFixups < 0 || NumFixups > 65536)
        {
            OutError = FString::Printf(TEXT("VVD fixup count %d out of range"), NumFixups);
            return false;
        }
        if (VertexStart < 0 || (int64)VertexStart > B.Num())
        {
            OutError = TEXT("VVD vertexDataStart out of range");
            return false;
        }
        if (TangentStart != 0 && (TangentStart < 0 || (int64)TangentStart > B.Num()))
        {
            OutError = TEXT("VVD tangentDataStart out of range");
            return false;
        }

        // Helper: read one mstudiovertex_t at raw vertex index `RawIdx`.
        auto ReadVertex = [&](int32 RawIdx, HL2Studio::FStudioVertex& V) -> bool
        {
            const int32 PosOfs = VertexStart + RawIdx * VERTEX_STRIDE + 16; // skip 16-byte bone weights
            if (PosOfs < 0 || (int64)PosOfs + 32 > B.Num()) { return false; }
            FMemory::Memcpy(&V.Position, B.GetData() + PosOfs + 0,  12);
            FMemory::Memcpy(&V.Normal,   B.GetData() + PosOfs + 12, 12);
            FMemory::Memcpy(&V.UV,       B.GetData() + PosOfs + 24, 8);

            if (TangentStart != 0)
            {
                const int32 TanOfs = TangentStart + RawIdx * TANGENT_STRIDE;
                if (TanOfs >= 0 && (int64)TanOfs + 16 <= B.Num())
                {
                    FMemory::Memcpy(&V.Tangent, B.GetData() + TanOfs, 16);
                }
            }
            return true;
        };

        // No fixups -> the file is already in LOD order. Read first LOD0Count
        // vertices straight through.
        if (NumFixups == 0)
        {
            Out.SetNum(LOD0Count);
            for (int32 i = 0; i < LOD0Count; ++i)
            {
                if (!ReadVertex(i, Out[i]))
                {
                    OutError = FString::Printf(TEXT("VVD vertex %d read failed"), i);
                    Out.Reset();
                    return false;
                }
            }
            UE_LOG(LogHL2BSPImporter, Verbose, TEXT("VVD parsed: %d LOD-0 verts, no fixups."), LOD0Count);
            return true;
        }

        // Apply fixup table: include every fixup with lod >= 0 (==LOD 0).
        Out.Reserve(LOD0Count);
        for (int32 i = 0; i < NumFixups; ++i)
        {
            int32 FxLOD = 0, FxSrc = 0, FxNum = 0;
            ReadAt(B, FixupStart + i * FIXUP_STRIDE + 0, FxLOD);
            ReadAt(B, FixupStart + i * FIXUP_STRIDE + 4, FxSrc);
            ReadAt(B, FixupStart + i * FIXUP_STRIDE + 8, FxNum);
            if (FxLOD < 0) { continue; }                 // LOD chain begins at 0
            if (FxNum <= 0) { continue; }
            if (FxSrc < 0 || FxSrc + FxNum > MAX_VERTICES)
            {
                OutError = FString::Printf(TEXT("VVD fixup %d (src=%d num=%d) out of range"), i, FxSrc, FxNum);
                Out.Reset();
                return false;
            }
            for (int32 r = 0; r < FxNum; ++r)
            {
                HL2Studio::FStudioVertex V;
                if (!ReadVertex(FxSrc + r, V))
                {
                    OutError = FString::Printf(TEXT("VVD vertex %d (fixup %d) read failed"), FxSrc + r, i);
                    Out.Reset();
                    return false;
                }
                Out.Add(V);
            }
        }

        if (Out.Num() != LOD0Count)
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("VVD fixup walk produced %d verts, header LOD-0 count is %d. Trusting fixup output."),
                Out.Num(), LOD0Count);
        }
        UE_LOG(LogHL2BSPImporter, Verbose,
            TEXT("VVD parsed: %d LOD-0 verts via %d fixups (header LOD-0=%d)."),
            Out.Num(), NumFixups, LOD0Count);
        return true;
    }
}
