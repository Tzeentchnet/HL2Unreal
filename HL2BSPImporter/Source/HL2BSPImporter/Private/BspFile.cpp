#include "BspFile.h"
#include "HL2BSPImporter.h"
#include "HL2Lzma.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// VBSP (Source/HL2) reader. Targets v19/v20 (HL2, EP1, EP2, CS:S).
// v21+ (CSGO/L4D2) and big-endian (Xbox/PS3, 'PSBV') are explicitly rejected.

namespace
{
    constexpr int32 LUMP_ENTITIES               = 0;
    constexpr int32 LUMP_PLANES                 = 1;
    constexpr int32 LUMP_TEXDATA                = 2;
    constexpr int32 LUMP_VERTEXES               = 3;
    constexpr int32 LUMP_TEXINFO                = 6;
    constexpr int32 LUMP_FACES                  = 7;
    constexpr int32 LUMP_EDGES                  = 12;
    constexpr int32 LUMP_SURFEDGES              = 13;
    constexpr int32 LUMP_MODELS                 = 14;
    constexpr int32 LUMP_DISPINFO               = 26;
    constexpr int32 LUMP_DISP_VERTS             = 33;
    constexpr int32 LUMP_GAME_LUMP              = 35;
    constexpr int32 LUMP_PAKFILE                = 40;
    constexpr int32 LUMP_TEXDATA_STRING_DATA    = 43;
    constexpr int32 LUMP_TEXDATA_STRING_TABLE   = 44;

    constexpr int32 IDENT_VBSP_LE = (int32('V')) | (int32('B') << 8) | (int32('S') << 16) | (int32('P') << 24);
    constexpr int32 IDENT_VBSP_BE = (int32('P')) | (int32('S') << 8) | (int32('B') << 16) | (int32('V') << 24);

    constexpr int32 MIN_BSP_VERSION = 19;
    constexpr int32 MAX_BSP_VERSION = 20;

    // Sanity caps to bound attacker-controlled lump sizes.
    constexpr int64 MAX_LUMP_BYTES  = 256 * 1024 * 1024; // 256 MB per lump
    constexpr int32 MAX_ENT_TEXT    = 32  * 1024 * 1024; // 32 MB entity text

#pragma pack(push, 1)
    struct FLumpInfo { int32 Ofs; int32 Len; int32 Version; int32 FourCC; };
    struct FBspHeader { int32 Ident; int32 Version; FLumpInfo Lumps[64]; int32 MapRevision; };
    static_assert(sizeof(FLumpInfo) == 16,    "lump_t must be 16 bytes");
    static_assert(sizeof(FBspHeader) == 1036, "dheader_t must be 1036 bytes");

    struct DVertex { float Pos[3]; };
    static_assert(sizeof(DVertex) == 12, "dvertex_t must be 12 bytes");

    struct DEdge { uint16 V[2]; };
    static_assert(sizeof(DEdge) == 4, "dedge_t must be 4 bytes");

    struct DFace
    {
        uint16 Planenum;     uint8  Side;             uint8  OnNode;
        int32  FirstEdge;    int16  NumEdges;         int16  TexInfo;
        int16  DispInfo;     int16  SurfaceFogVolumeID;
        uint8  Styles[4];    int32  Lightofs;         float  Area;
        int32  LmMins[2];    int32  LmSize[2];        int32  OrigFace;
        uint16 NumPrims;     uint16 FirstPrimID;      uint32 SmoothingGroups;
    };
    static_assert(sizeof(DFace) == 56, "dface_t must be 56 bytes");

    struct DTexInfo { float TextureVecs[2][4]; float LightmapVecs[2][4]; int32 Flags; int32 TexData; };
    static_assert(sizeof(DTexInfo) == 72, "dtexinfo_t must be 72 bytes");

    struct DTexData { float Reflectivity[3]; int32 NameStringTableID; int32 Width; int32 Height; int32 ViewWidth; int32 ViewHeight; };
    static_assert(sizeof(DTexData) == 32, "dtexdata_t must be 32 bytes");

    struct DModel
    {
        float Mins[3];   float Maxs[3];
        float Origin[3];
        int32 HeadNode;
        int32 FirstFace; int32 NumFaces;
    };
    static_assert(sizeof(DModel) == 48, "dmodel_t must be 48 bytes");

    struct DDispInfo
    {
        float  StartPosition[3];
        int32  DispVertStart;
        int32  DispTriStart;
        int32  Power;
        int32  MinTess;
        float  SmoothingAngle;
        int32  Contents;
        uint16 MapFace;
        int32  LightmapAlphaStart;
        int32  LightmapSamplePositionStart;
        // CDispNeighbor m_EdgeNeighbors[4]   = 4 * 18 = 72 bytes (4*(2*8 + 2 padding) actually 2 spans of 6 bytes ea + 2 pad = 18)
        // CDispCornerNeighbors m_CornerNeighbors[4] = 4 * 10 = 40 bytes (4 corners * (4 ushort + 1 byte + pad)) -> reference uses 40 total
        // m_AllowedVerts[10] uint32 = 40 bytes
        // Rather than model the unions exactly we reserve a raw block sized so the total struct hits 176 bytes.
        uint8  NeighborBlob[72 + 40];
        uint32 AllowedVerts[10];
    };
    static_assert(sizeof(DDispInfo) == 176, "ddispinfo_t must be 176 bytes");

    struct DDispVert { float Vector[3]; float Dist; float Alpha; };
    static_assert(sizeof(DDispVert) == 20, "ddispvert_t must be 20 bytes");

    // dgamelump_t: per-sub-lump descriptor inside LUMP_GAME_LUMP. id is a four-cc stored as int32
    // in little-endian order (e.g. 'sprp' = ('s')|('p'<<8)|('r'<<16)|('p'<<24) = 0x70727073).
    struct DGameLump
    {
        int32  Id;
        uint16 Flags;
        uint16 Version;
        int32  FileOfs;
        int32  FileLen;
    };
    static_assert(sizeof(DGameLump) == 16, "dgamelump_t must be 16 bytes");

    constexpr int32 GAMELUMP_ID_SPRP = (int32('s')) | (int32('p') << 8) | (int32('r') << 16) | (int32('p') << 24);

    // Common StaticPropLump_t prefix shared by versions 4..11. We read this much and then skip
    // the version-specific tail by computing per-entry stride from filelen / entry count.
    // 56 bytes: Origin(12) Angles(12) PropType(2) FirstLeaf(2) LeafCount(2) Solid(1) Flags(1)
    //           Skin(4) FadeMinDist(4) FadeMaxDist(4) LightingOrigin(12) = 56
    struct StaticPropLumpV4
    {
        float  Origin[3];
        float  Angles[3];
        uint16 PropType;
        uint16 FirstLeaf;
        uint16 LeafCount;
        uint8  Solid;
        uint8  Flags;
        int32  Skin;
        float  FadeMinDist;
        float  FadeMaxDist;
        float  LightingOrigin[3];
    };
    static_assert(sizeof(StaticPropLumpV4) == 56, "StaticPropLump_t v4 must be 56 bytes");

    // Per-version stride table. Values come from public Source SDK headers / community VBSP docs.
    // Versions not listed here fall back to the v4 prefix (entries are still readable; trailing
    // fields like UniformScale will be zero).
    int32 GetStaticPropStride(int32 Version)
    {
        switch (Version)
        {
            case 4:  return 56;   // base
            case 5:  return 60;   // + ForcedFadeScale (float)
            case 6:  return 64;   // + MinDXLevel/MaxDXLevel (uint16 x2)
            case 7:  return 68;   // + DiffuseModulation (uint32) [HL2:EP1 community variant]
            case 8:  return 68;   // replaces DX with MinCPULevel..MaxGPULevel (uint8 x4)
            case 9:  return 72;   // + DiffuseModulation (uint32)
            case 10: return 76;   // + FlagsEx (uint32)
            case 11: return 80;   // + UniformScale (float)
            default: return 0;
        }
    }

    int32 GetUniformScaleOffset(int32 Version)
    {
        // UniformScale is the last 4 bytes of v11.
        return (Version == 11) ? 76 : -1;
    }
#pragma pack(pop)

    // Resolve a lump to its decompressed byte view, performing common bounds/sanity checks.
    // Returns true on success; sets OutPtr/OutLen to either an in-place slice of `Bytes`
    // or to data owned by `OwnedDecompressed` (when the lump is LZMA-compressed).
    bool AcquireLumpBytes(
        const TArray<uint8>& Bytes,
        const FLumpInfo& L,
        const TCHAR* DebugName,
        TArray<uint8>& OwnedDecompressed,
        const uint8*& OutPtr,
        int64& OutLen)
    {
        OutPtr = nullptr;
        OutLen = 0;
        OwnedDecompressed.Reset();

        if (L.Ofs < 0 || L.Len < 0)
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("Lump %s has negative offset/length (ofs=%d len=%d)."), DebugName, L.Ofs, L.Len);
            return false;
        }
        const int64 End = static_cast<int64>(L.Ofs) + static_cast<int64>(L.Len);
        if (End > Bytes.Num())
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("Lump %s extends past EOF (ofs=%d len=%d, file=%d)."), DebugName, L.Ofs, L.Len, Bytes.Num());
            return false;
        }
        if (L.Len > MAX_LUMP_BYTES)
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("Lump %s exceeds sanity cap (%d > %lld)."), DebugName, L.Len, (long long)MAX_LUMP_BYTES);
            return false;
        }

        const uint8* RawPtr = Bytes.GetData() + L.Ofs;

        if (L.FourCC != 0)
        {
            // Source's LZMA-compressed lump. The LZMA header sits at L.Ofs; FourCC carries the
            // *uncompressed* size (per VBSP spec) but our decoder reads it from the LZMA header itself.
            if (!HL2Lzma::IsSourceLzma(RawPtr, L.Len))
            {
                UE_LOG(LogHL2BSPImporter, Error,
                    TEXT("Lump %s has FourCC=0x%08x but no recognisable LZMA header."), DebugName, L.FourCC);
                return false;
            }
            if (!HL2Lzma::DecompressSourceLump(RawPtr, L.Len, OwnedDecompressed, DebugName))
            {
                return false;
            }
            OutPtr = OwnedDecompressed.GetData();
            OutLen = OwnedDecompressed.Num();
            return true;
        }

        OutPtr = RawPtr;
        OutLen = L.Len;
        return true;
    }

    // Read a typed array from a lump. Performs OOM/overflow caps and structural-size validation.
    template<typename T>
    bool ReadLump(const TArray<uint8>& Bytes, const FLumpInfo& L, const TCHAR* DebugName, TArray<T>& Out)
    {
        Out.Reset();

        if (L.Len == 0) { return true; }

        TArray<uint8> Decompressed;
        const uint8* DataPtr = nullptr;
        int64        DataLen = 0;
        if (!AcquireLumpBytes(Bytes, L, DebugName, Decompressed, DataPtr, DataLen))
        {
            return false;
        }
        if (DataLen == 0) { return true; }

        if (DataLen % static_cast<int64>(sizeof(T)) != 0)
        {
            UE_LOG(LogHL2BSPImporter, Error,
                TEXT("Lump %s size %lld is not a multiple of element size %d."), DebugName, (long long)DataLen, (int32)sizeof(T));
            return false;
        }

        const int64 Count64 = DataLen / static_cast<int64>(sizeof(T));
        if (Count64 > MAX_int32)
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("Lump %s element count %lld overflows int32."), DebugName, (long long)Count64);
            return false;
        }
        const int32 Count = static_cast<int32>(Count64);
        Out.SetNumUninitialized(Count);
        FMemory::Memcpy(Out.GetData(), DataPtr, DataLen);
        return true;
    }

    // Bounds-safe absolute value of a surf-edge index (handles INT32_MIN).
    int32 SurfEdgeAbs(int32 SeIdx)
    {
        if (SeIdx == MIN_int32) { return -1; } // out of range; caller treats as invalid
        return SeIdx < 0 ? -SeIdx : SeIdx;
    }
}

bool FBspFile::LoadFromFile(const FString& Filename)
{
    Vertices.Reset();
    Faces.Reset();
    DispInfos.Reset();
    DispVerts.Reset();
    Entities.Reset();
    BrushModels.Reset();
    StaticProps.Reset();
    PakfileBytes.Reset();
    WorldFirstFace = 0;
    WorldNumFaces  = 0;
    Version        = 0;

    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("BSP LoadFileToArray failed: %s"), *Filename);
        return false;
    }
    if (Bytes.Num() < static_cast<int32>(sizeof(FBspHeader)))
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("BSP too small for header: %s (size=%d)"), *Filename, Bytes.Num());
        return false;
    }

    FBspHeader H{};
    FMemory::Memcpy(&H, Bytes.GetData(), sizeof(FBspHeader));

    if (H.Ident == IDENT_VBSP_BE)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Big-endian (Xbox/PS3) BSP not supported: %s"), *Filename);
        return false;
    }
    if (H.Ident != IDENT_VBSP_LE)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Wrong BSP magic. Expected 'VBSP', got 0x%08x for %s"), H.Ident, *Filename);
        return false;
    }
    if (H.Version < MIN_BSP_VERSION || H.Version > MAX_BSP_VERSION)
    {
        UE_LOG(LogHL2BSPImporter, Error,
            TEXT("Unsupported VBSP version %d. This importer supports v%d-v%d (HL2/EP1/EP2/CS:S)."),
            H.Version, MIN_BSP_VERSION, MAX_BSP_VERSION);
        return false;
    }
    Version = H.Version;
    UE_LOG(LogHL2BSPImporter, Log, TEXT("VBSP header: Version=%d MapRevision=%d"), H.Version, H.MapRevision);

    TArray<DVertex>  SrcVerts;
    TArray<DEdge>    Edges;
    TArray<int32>    SurfEdges;
    TArray<DFace>    FacesSrc;
    TArray<DTexInfo> TexInfos;
    TArray<DTexData> TexDatas;
    TArray<int32>    StrOffsets;
    TArray<uint8>    StrData;
    TArray<DModel>   Models;
    TArray<DDispInfo> DispInfosSrc;
    TArray<DDispVert> DispVertsSrc;

    if (!ReadLump(Bytes, H.Lumps[LUMP_VERTEXES],             TEXT("LUMP_VERTEXES"),             SrcVerts))      return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_EDGES],                TEXT("LUMP_EDGES"),                Edges))         return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_SURFEDGES],            TEXT("LUMP_SURFEDGES"),            SurfEdges))     return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_FACES],                TEXT("LUMP_FACES"),                FacesSrc))      return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_TEXINFO],              TEXT("LUMP_TEXINFO"),              TexInfos))      return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_TEXDATA],              TEXT("LUMP_TEXDATA"),              TexDatas))      return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_TEXDATA_STRING_TABLE], TEXT("LUMP_TEXDATA_STRING_TABLE"), StrOffsets))    return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_TEXDATA_STRING_DATA],  TEXT("LUMP_TEXDATA_STRING_DATA"),  StrData))       return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_MODELS],               TEXT("LUMP_MODELS"),               Models))        return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_DISPINFO],             TEXT("LUMP_DISPINFO"),             DispInfosSrc))  return false;
    if (!ReadLump(Bytes, H.Lumps[LUMP_DISP_VERTS],           TEXT("LUMP_DISP_VERTS"),           DispVertsSrc))  return false;

    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("VBSP lumps OK. Verts=%d Edges=%d SurfEdges=%d Faces=%d TexInfo=%d TexData=%d Models=%d DispInfo=%d DispVerts=%d"),
        SrcVerts.Num(), Edges.Num(), SurfEdges.Num(), FacesSrc.Num(),
        TexInfos.Num(), TexDatas.Num(), Models.Num(), DispInfosSrc.Num(), DispVertsSrc.Num());

    if (Models.Num() == 0)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("BSP has no models lump entries; cannot determine worldspawn face range."));
        return false;
    }

    // Build faces. Worldspawn (model 0) is emitted first into Faces[0..WorldNumFaces); each
    // brush model 1..N-1 is then emitted contiguously and recorded in BrushModels with a
    // (FirstFace, NumFaces) range into the same Faces array.
    WorldFirstFace = 0;
    int32 SkippedNoDraw       = 0;
    int32 SkippedDisplacement = 0;
    int32 SkippedTooFew       = 0;
    int32 SkippedBadIndex     = 0;

    auto GetTexName = [&](int32 TexInfoIndex) -> FString
    {
        if (TexInfoIndex < 0 || TexInfoIndex >= TexInfos.Num()) return FString();
        const int32 TexDataIndex = TexInfos[TexInfoIndex].TexData;
        if (TexDataIndex < 0 || TexDataIndex >= TexDatas.Num()) return FString();
        const int32 StrIdx = TexDatas[TexDataIndex].NameStringTableID;
        if (StrIdx < 0 || StrIdx >= StrOffsets.Num()) return FString();
        const int32 Ofs = StrOffsets[StrIdx];
        if (Ofs < 0 || Ofs >= StrData.Num()) return FString();
        const ANSICHAR* Start = reinterpret_cast<const ANSICHAR*>(StrData.GetData() + Ofs);
        return FString(UTF8_TO_TCHAR(Start));
    };

    auto ComputeUV = [&](const FVector& P, int32 TexInfoIndex, FVector2D& OutUV, FVector2D& OutLightmapUV)
    {
        OutUV          = FVector2D::ZeroVector;
        OutLightmapUV  = FVector2D::ZeroVector;
        if (TexInfoIndex < 0 || TexInfoIndex >= TexInfos.Num()) return;
        const DTexInfo& TI = TexInfos[TexInfoIndex];
        const FVector S(TI.TextureVecs[0][0], TI.TextureVecs[0][1], TI.TextureVecs[0][2]);
        const FVector T(TI.TextureVecs[1][0], TI.TextureVecs[1][1], TI.TextureVecs[1][2]);
        float u = static_cast<float>(FVector::DotProduct(P, S)) + TI.TextureVecs[0][3];
        float v = static_cast<float>(FVector::DotProduct(P, T)) + TI.TextureVecs[1][3];
        const int32 TexDataIndex = TI.TexData;
        if (TexDataIndex >= 0 && TexDataIndex < TexDatas.Num())
        {
            const float W = static_cast<float>(FMath::Max<int32>(1, TexDatas[TexDataIndex].Width));
            const float Hh = static_cast<float>(FMath::Max<int32>(1, TexDatas[TexDataIndex].Height));
            u /= W; v /= Hh;
        }
        OutUV = FVector2D(u, v);

        // Lightmap UV: same vector dot, normalized later by caller per-face (placeholder pass-through in raw texel space).
        const FVector LS(TI.LightmapVecs[0][0], TI.LightmapVecs[0][1], TI.LightmapVecs[0][2]);
        const FVector LT(TI.LightmapVecs[1][0], TI.LightmapVecs[1][1], TI.LightmapVecs[1][2]);
        const float lu = static_cast<float>(FVector::DotProduct(P, LS)) + TI.LightmapVecs[0][3];
        const float lv = static_cast<float>(FVector::DotProduct(P, LT)) + TI.LightmapVecs[1][3];
        OutLightmapUV = FVector2D(lu, lv);
    };

    auto EmitModelFaces = [&](const DModel& Model, int32 ModelIndex) -> int32
    {
        if (Model.FirstFace < 0 || Model.NumFaces < 0 ||
            static_cast<int64>(Model.FirstFace) + Model.NumFaces > FacesSrc.Num())
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("Model %d has invalid face range (FirstFace=%d NumFaces=%d total=%d); skipping."),
                ModelIndex, Model.FirstFace, Model.NumFaces, FacesSrc.Num());
            return 0;
        }

        const int32 FirstEmitted = Faces.Num();

        for (int32 f = Model.FirstFace; f < Model.FirstFace + Model.NumFaces; ++f)
        {
            const DFace& DF = FacesSrc[f];
            if (DF.NumEdges < 3) { ++SkippedTooFew; continue; }

            const uint32 SurfFlags = (DF.TexInfo >= 0 && DF.TexInfo < TexInfos.Num())
                ? static_cast<uint32>(TexInfos[DF.TexInfo].Flags)
                : 0u;
            const bool bIsDisplacement = (DF.DispInfo >= 0);
            if (!bIsDisplacement && (SurfFlags & EHL2SurfFlag::SkipRenderMask)) { ++SkippedNoDraw; continue; }

            if (DF.FirstEdge < 0 ||
                static_cast<int64>(DF.FirstEdge) + DF.NumEdges > SurfEdges.Num())
            {
                ++SkippedBadIndex; continue;
            }

            TArray<FVector,   TInlineAllocator<8>> PolyPos;
            TArray<FVector2D, TInlineAllocator<8>> PolyUV;
            TArray<FVector2D, TInlineAllocator<8>> PolyLM;
            bool bAnyBad = false;
            for (int32 i = 0; i < DF.NumEdges; ++i)
            {
                const int32 SeIdx = SurfEdges[DF.FirstEdge + i];
                const int32 EdgeIndex = SurfEdgeAbs(SeIdx);
                if (EdgeIndex < 0 || EdgeIndex >= Edges.Num()) { bAnyBad = true; break; }

                const DEdge& E = Edges[EdgeIndex];
                const int32 VIdx = (SeIdx >= 0) ? E.V[0] : E.V[1];
                if (VIdx < 0 || VIdx >= SrcVerts.Num()) { bAnyBad = true; break; }

                FVector P(SrcVerts[VIdx].Pos[0], SrcVerts[VIdx].Pos[1], SrcVerts[VIdx].Pos[2]);
                FVector2D UV, LMUV;
                ComputeUV(P, DF.TexInfo, UV, LMUV);
                PolyPos.Add(P);
                PolyUV.Add(UV);
                PolyLM.Add(LMUV);
            }
            if (bAnyBad) { ++SkippedBadIndex; continue; }
            if (PolyPos.Num() < 3) { ++SkippedTooFew; continue; }

            // Displacements live in worldspawn in practice; stash corners on the disp row and
            // skip emitting the base face. (We still skip+stash for sub-models if they happen
            // to carry displacements — disp triangles are emitted by the factory at world scope.)
            if (bIsDisplacement)
            {
                ++SkippedDisplacement;
                const int32 DI = DF.DispInfo;
                if (DI >= 0 && DI < DispInfosSrc.Num() && PolyPos.Num() == 4)
                {
                    if (DispInfos.Num() < DispInfosSrc.Num()) { DispInfos.SetNum(DispInfosSrc.Num()); }
                    FDispInfo& Out = DispInfos[DI];
                    for (int32 c = 0; c < 4; ++c)
                    {
                        Out.Corners[c]   = PolyPos[c];
                        Out.CornerUVs[c] = PolyUV[c];
                    }
                    Out.TextureName = GetTexName(DF.TexInfo);
                }
                continue;
            }

            const int32 StartIndex = Vertices.Num();
            for (int32 i = 0; i < PolyPos.Num(); ++i)
            {
                FBspVertex BV;
                BV.Position   = PolyPos[i];
                BV.UV         = PolyUV[i];
                BV.LightmapUV = PolyLM[i];
                Vertices.Add(BV);
            }
            FBspFace OutF;
            OutF.FirstVertex   = StartIndex;
            OutF.NumVertices   = PolyPos.Num();
            OutF.TextureName   = GetTexName(DF.TexInfo);
            OutF.DispInfoIndex = DF.DispInfo;
            OutF.Side          = DF.Side;
            OutF.SurfFlags     = SurfFlags;
            Faces.Add(MoveTemp(OutF));
        }

        return Faces.Num() - FirstEmitted;
    };

    // Worldspawn = model 0
    WorldNumFaces = EmitModelFaces(Models[0], 0);

    // Brush sub-models = models 1..N-1
    BrushModels.Reserve(FMath::Max(0, Models.Num() - 1));
    for (int32 m = 1; m < Models.Num(); ++m)
    {
        const DModel& M = Models[m];
        const int32 FirstEmitted = Faces.Num();
        const int32 EmittedCount = EmitModelFaces(M, m);
        if (EmittedCount <= 0) { continue; } // no renderable faces (e.g. trigger-only brush entity)

        FBspBrushModel BM;
        BM.ModelIndex = m;
        BM.FirstFace  = FirstEmitted;
        BM.NumFaces   = EmittedCount;
        BM.Origin     = FVector(M.Origin[0], M.Origin[1], M.Origin[2]);
        BM.Mins       = FVector(M.Mins[0],   M.Mins[1],   M.Mins[2]);
        BM.Maxs       = FVector(M.Maxs[0],   M.Maxs[1],   M.Maxs[2]);
        BrushModels.Add(BM);
    }

    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("Face emit: world=%d brush-models=%d total-faces=%d skipped(nodraw/sky=%d, disp=%d, <3edges=%d, badidx=%d)"),
        WorldNumFaces, BrushModels.Num(), Faces.Num(),
        SkippedNoDraw, SkippedDisplacement, SkippedTooFew, SkippedBadIndex);

    // Populate disp metadata in-place (corner data was already filled by the face loop above).
    if (DispInfos.Num() < DispInfosSrc.Num())
    {
        DispInfos.SetNum(DispInfosSrc.Num());
    }
    for (int32 i = 0; i < DispInfosSrc.Num(); ++i)
    {
        const DDispInfo& D = DispInfosSrc[i];
        FDispInfo& O = DispInfos[i];
        O.StartPosition = FVector(D.StartPosition[0], D.StartPosition[1], D.StartPosition[2]);
        O.Power         = D.Power;
        O.VertStart     = D.DispVertStart;
        O.MapFace       = static_cast<int32>(D.MapFace);
    }
    DispVerts.Reserve(DispVertsSrc.Num());
    for (const DDispVert& V : DispVertsSrc)
    {
        FDispVert Out;
        Out.Vector[0] = V.Vector[0]; Out.Vector[1] = V.Vector[1]; Out.Vector[2] = V.Vector[2];
        Out.Dist  = V.Dist;
        Out.Alpha = V.Alpha;
        DispVerts.Add(Out);
    }

    // Entities (text lump). Source entity text is ASCII/UTF-8.
    const FLumpInfo& LEnts = H.Lumps[LUMP_ENTITIES];
    if (LEnts.Len > 0)
    {
        TArray<uint8> EntDecompressed;
        const uint8*  EntPtr = nullptr;
        int64         EntLen = 0;
        if (!AcquireLumpBytes(Bytes, LEnts, TEXT("LUMP_ENTITIES"), EntDecompressed, EntPtr, EntLen))
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("Entity lump unreadable; skipping."));
        }
        else if (EntLen > MAX_ENT_TEXT)
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("Entity lump too large (%lld > %d), skipping."), (long long)EntLen, MAX_ENT_TEXT);
        }
        else if (EntLen > 0)
        {
            const ANSICHAR* RawBegin = reinterpret_cast<const ANSICHAR*>(EntPtr);
            FString EntText(static_cast<int32>(EntLen), RawBegin); // FString(int32 Len, const ANSICHAR*) constructor
            ParseEntities(EntText, Entities);
        }
    }

    // Pakfile (LUMP_PAKFILE / 40). Standard PKZIP archive embedded in the BSP.
    // We capture the (decompressed-if-needed) raw bytes here and let HL2PakFile parse them.
    {
        const FLumpInfo& LPak = H.Lumps[LUMP_PAKFILE];
        if (LPak.Len > 0)
        {
            TArray<uint8> PakDecompressed;
            const uint8*  PakPtr = nullptr;
            int64         PakLen = 0;
            if (AcquireLumpBytes(Bytes, LPak, TEXT("LUMP_PAKFILE"), PakDecompressed, PakPtr, PakLen) && PakLen > 0)
            {
                if (PakDecompressed.Num() > 0)
                {
                    PakfileBytes = MoveTemp(PakDecompressed);
                }
                else
                {
                    PakfileBytes.SetNumUninitialized(static_cast<int32>(PakLen));
                    FMemory::Memcpy(PakfileBytes.GetData(), PakPtr, PakLen);
                }
                UE_LOG(LogHL2BSPImporter, Log, TEXT("Pakfile lump: %d bytes."), PakfileBytes.Num());
            }
            else
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Pakfile lump unreadable; skipping."));
            }
        }
    }

    // GameLump (LUMP_GAME_LUMP / 35) — we only consume the `sprp` sub-lump (static props).
    // Layout: int32 LumpCount; dgamelump_t Lumps[LumpCount]; followed by each sub-lump's payload
    // at its absolute fileofs (offsets are file-absolute, NOT lump-relative).
    {
        const FLumpInfo& LGL = H.Lumps[LUMP_GAME_LUMP];
        if (LGL.Len > 0)
        {
            TArray<uint8> GLDecompressed;
            const uint8*  GLPtr = nullptr;
            int64         GLLen = 0;
            if (!AcquireLumpBytes(Bytes, LGL, TEXT("LUMP_GAME_LUMP"), GLDecompressed, GLPtr, GLLen) || GLLen < 4)
            {
                UE_LOG(LogHL2BSPImporter, Verbose, TEXT("GameLump empty/unreadable; no static props."));
            }
            else
            {
                int32 LumpCount = 0;
                FMemory::Memcpy(&LumpCount, GLPtr, sizeof(int32));
                if (LumpCount < 0 || LumpCount > 256)
                {
                    UE_LOG(LogHL2BSPImporter, Warning, TEXT("GameLump count %d out of range; ignoring."), LumpCount);
                }
                else if (4 + static_cast<int64>(LumpCount) * sizeof(DGameLump) > GLLen)
                {
                    UE_LOG(LogHL2BSPImporter, Warning, TEXT("GameLump directory truncated (count=%d, lumpLen=%lld)."), LumpCount, (long long)GLLen);
                }
                else
                {
                    const DGameLump* Dir = reinterpret_cast<const DGameLump*>(GLPtr + 4);
                    int32 SprpVersion = -1;
                    int32 SprpFileOfs = -1;
                    int32 SprpFileLen = 0;
                    bool  bSprpCompressed = false;
                    for (int32 i = 0; i < LumpCount; ++i)
                    {
                        if (Dir[i].Id == GAMELUMP_ID_SPRP)
                        {
                            SprpVersion = static_cast<int32>(Dir[i].Version);
                            SprpFileOfs = Dir[i].FileOfs;
                            SprpFileLen = Dir[i].FileLen;
                            bSprpCompressed = (Dir[i].Flags & 0x1) != 0;
                            break;
                        }
                    }

                    if (SprpVersion >= 0 && SprpFileLen > 0)
                    {
                        const int64 SprpEnd = static_cast<int64>(SprpFileOfs) + SprpFileLen;
                        if (SprpFileOfs < 0 || SprpEnd > Bytes.Num())
                        {
                            UE_LOG(LogHL2BSPImporter, Warning, TEXT("sprp GameLump out of range (ofs=%d len=%d file=%d)."),
                                SprpFileOfs, SprpFileLen, Bytes.Num());
                        }
                        else
                        {
                            // Resolve the sprp payload to either the in-place file slice or
                            // an owned decompressed buffer when the sub-lump is LZMA-packed.
                            // Source's compressed game-lump format prepends the standard 17-byte
                            // LZMA header (id='LZMA' + actualSize + lzmaSize + 5 props) to the
                            // payload referenced by dgamelump_t::FileOfs/FileLen.
                            TArray<uint8> SprpDecompressed;
                            const uint8*  P   = nullptr;
                            const uint8*  End = nullptr;
                            if (bSprpCompressed)
                            {
                                const uint8* Compressed = Bytes.GetData() + SprpFileOfs;
                                if (!HL2Lzma::IsSourceLzma(Compressed, SprpFileLen))
                                {
                                    UE_LOG(LogHL2BSPImporter, Warning,
                                        TEXT("sprp GameLump marked compressed but no LZMA header found; skipping static props."));
                                }
                                else if (!HL2Lzma::DecompressSourceLump(Compressed, SprpFileLen, SprpDecompressed, TEXT("sprp GameLump")))
                                {
                                    // Decoder already logged the failure cause.
                                }
                                else
                                {
                                    P   = SprpDecompressed.GetData();
                                    End = P + SprpDecompressed.Num();
                                    UE_LOG(LogHL2BSPImporter, Verbose,
                                        TEXT("sprp GameLump decompressed: %d -> %d bytes."),
                                        SprpFileLen, SprpDecompressed.Num());
                                }
                            }
                            else
                            {
                                P   = Bytes.GetData() + SprpFileOfs;
                                End = P + SprpFileLen;
                            }

                            if (P != nullptr && End != nullptr)
                            {

                            auto ReadInt = [&](int32& Out) -> bool
                            {
                                if (P + sizeof(int32) > End) { return false; }
                                FMemory::Memcpy(&Out, P, sizeof(int32));
                                P += sizeof(int32);
                                return true;
                            };

                            int32 DictCount = 0;
                            if (!ReadInt(DictCount) || DictCount < 0 || DictCount > 65536)
                            {
                                UE_LOG(LogHL2BSPImporter, Warning, TEXT("sprp dict count invalid (%d)."), DictCount);
                            }
                            else if (P + static_cast<int64>(DictCount) * 128 > End)
                            {
                                UE_LOG(LogHL2BSPImporter, Warning, TEXT("sprp dict truncated."));
                            }
                            else
                            {
                                TArray<FString> Dict;
                                Dict.Reserve(DictCount);
                                for (int32 d = 0; d < DictCount; ++d)
                                {
                                    const ANSICHAR* Name = reinterpret_cast<const ANSICHAR*>(P);
                                    // Bounded read: stop at NUL or 128 chars.
                                    int32 Nlen = 0;
                                    while (Nlen < 128 && Name[Nlen] != 0) { ++Nlen; }
                                    Dict.Add(FString(Nlen, Name));
                                    P += 128;
                                }

                                int32 LeafCount = 0;
                                if (!ReadInt(LeafCount) || LeafCount < 0 ||
                                    P + static_cast<int64>(LeafCount) * sizeof(uint16) > End)
                                {
                                    UE_LOG(LogHL2BSPImporter, Warning, TEXT("sprp leaf array invalid/truncated (count=%d)."), LeafCount);
                                }
                                else
                                {
                                    P += static_cast<int64>(LeafCount) * sizeof(uint16); // skip leaf array

                                    int32 EntryCount = 0;
                                    if (!ReadInt(EntryCount) || EntryCount < 0)
                                    {
                                        UE_LOG(LogHL2BSPImporter, Warning, TEXT("sprp entry count invalid (%d)."), EntryCount);
                                    }
                                    else if (EntryCount > 0)
                                    {
                                        const int64 RemainingBytes = End - P;
                                        const int32 KnownStride    = GetStaticPropStride(SprpVersion);
                                        // Compute actual stride from on-disk layout, then sanity-check against the known table.
                                        if (EntryCount > 0 && RemainingBytes % EntryCount != 0)
                                        {
                                            UE_LOG(LogHL2BSPImporter, Warning,
                                                TEXT("sprp v%d entry payload (%lld bytes) is not divisible by entry count %d; ignoring static props."),
                                                SprpVersion, (long long)RemainingBytes, EntryCount);
                                        }
                                        else
                                        {
                                            const int64 ActualStride = (EntryCount > 0) ? (RemainingBytes / EntryCount) : 0;
                                            if (ActualStride < static_cast<int64>(sizeof(StaticPropLumpV4)))
                                            {
                                                UE_LOG(LogHL2BSPImporter, Warning,
                                                    TEXT("sprp v%d entry stride %lld < %d (v4 prefix). Ignoring."),
                                                    SprpVersion, (long long)ActualStride, (int32)sizeof(StaticPropLumpV4));
                                            }
                                            else
                                            {
                                                if (KnownStride != 0 && ActualStride != KnownStride)
                                                {
                                                    UE_LOG(LogHL2BSPImporter, Verbose,
                                                        TEXT("sprp v%d on-disk stride %lld differs from expected %d; using on-disk value."),
                                                        SprpVersion, (long long)ActualStride, KnownStride);
                                                }

                                                const int32 ScaleOffset = GetUniformScaleOffset(SprpVersion);
                                                StaticProps.Reserve(EntryCount);
                                                int32 BadDictRefs = 0;
                                                for (int32 e = 0; e < EntryCount; ++e)
                                                {
                                                    StaticPropLumpV4 V4{};
                                                    FMemory::Memcpy(&V4, P, sizeof(StaticPropLumpV4));

                                                    FBspStaticProp Prop;
                                                    if (V4.PropType < Dict.Num())
                                                    {
                                                        Prop.ModelName = Dict[V4.PropType];
                                                    }
                                                    else
                                                    {
                                                        ++BadDictRefs;
                                                    }
                                                    Prop.Origin = FVector(V4.Origin[0], V4.Origin[1], V4.Origin[2]);
                                                    Prop.Angles = FVector(V4.Angles[0], V4.Angles[1], V4.Angles[2]);
                                                    Prop.Skin   = V4.Skin;
                                                    Prop.Solid  = V4.Solid;
                                                    Prop.UniformScale = 1.f;

                                                    if (ScaleOffset >= 0 && ScaleOffset + 4 <= ActualStride)
                                                    {
                                                        float S = 1.f;
                                                        FMemory::Memcpy(&S, P + ScaleOffset, sizeof(float));
                                                        if (S > 0.f && FMath::IsFinite(S)) { Prop.UniformScale = S; }
                                                    }

                                                    StaticProps.Add(MoveTemp(Prop));
                                                    P += ActualStride;
                                                }

                                                UE_LOG(LogHL2BSPImporter, Log,
                                                    TEXT("Static props: dict=%d entries=%d v=%d stride=%lld badDictRefs=%d"),
                                                    Dict.Num(), EntryCount, SprpVersion, (long long)ActualStride, BadDictRefs);
                                            }
                                        }
                                    }
                                }
                            }
                            } // end if (P != nullptr && End != nullptr)
                        }
                    }
                }
            }
        }
    }

    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("BSP parsed: OutVerts=%d OutFaces=%d DispInfos=%d DispVerts=%d Entities=%d StaticProps=%d"),
        Vertices.Num(), Faces.Num(), DispInfos.Num(), DispVerts.Num(), Entities.Num(), StaticProps.Num());
    return true;
}

// Parse Source entity text into FHL2Entity rows. Hardened against malformed input:
// - cursor always advances at least one TCHAR per outer iteration
// - bounded reads, no past-end-of-buffer dereferences
void FBspFile::ParseEntities(const FString& EntText, TArray<FHL2Entity>& Out) const
{
    Out.Reset();
    const TCHAR* const Begin = *EntText;
    const TCHAR* const End   = Begin + EntText.Len();

    auto IsWS = [](TCHAR c) { return c == TEXT(' ') || c == TEXT('\t') || c == TEXT('\r') || c == TEXT('\n'); };

    // Source I/O output value separator: 0x1B (ESC) in modern Source, ',' in HL2-era maps.
    // Pick whichever the value actually contains; fall back to comma. Either way the field
    // order is target,input,parameter,delay,timesToFire (delay/timesToFire optional).
    auto ParseIoValue = [](const FString& Val, FHL2EntityIO& Out)
    {
        const TCHAR Sep = Val.Contains(TEXT("\x1b")) ? TEXT('\x1b') : TEXT(',');
        TArray<FString> Parts;
        Val.ParseIntoArray(Parts, &Sep, /*CullEmpty*/ false);
        if (Parts.Num() < 2) { return false; }
        Out.TargetName = Parts[0];
        Out.InputName  = Parts[1];
        Out.Parameter  = Parts.IsValidIndex(2) ? Parts[2] : FString();
        Out.Delay      = Parts.IsValidIndex(3) ? FCString::Atof(*Parts[3]) : 0.f;
        Out.TimesToFire= Parts.IsValidIndex(4) ? FCString::Atoi(*Parts[4]) : -1;
        return true;
    };

    const TCHAR* S = Begin;
    bool InEnt = false;
    TMap<FString, FString> KV;
    TArray<FHL2EntityIO> PendingOutputs;
    int64 TotalOutputs = 0;

    auto Flush = [&]()
    {
        if (KV.Num() == 0 && PendingOutputs.Num() == 0) { return; }
        FHL2Entity E;
        KV.RemoveAndCopyValue(TEXT("targetname"), E.Name);
        KV.RemoveAndCopyValue(TEXT("classname"),  E.Class);
        FString Tmp;
        if (KV.RemoveAndCopyValue(TEXT("origin"), Tmp))
        {
            TArray<FString> Parts; Tmp.ParseIntoArrayWS(Parts);
            if (Parts.Num() == 3)
            {
                E.Origin = FVector(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
            }
        }
        if (KV.RemoveAndCopyValue(TEXT("angles"), Tmp))
        {
            TArray<FString> Parts; Tmp.ParseIntoArrayWS(Parts);
            if (Parts.Num() == 3)
            {
                // Source: pitch yaw roll. UE FRotator: pitch yaw roll (same field order).
                E.Rotation = FRotator(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
            }
        }
        KV.RemoveAndCopyValue(TEXT("model"), E.Model);
        E.Outputs = MoveTemp(PendingOutputs);
        TotalOutputs += E.Outputs.Num();
        Out.Add(MoveTemp(E));
        KV.Reset();
        PendingOutputs.Reset();
    };

    while (S < End)
    {
        // Skip whitespace
        while (S < End && IsWS(*S)) { ++S; }
        if (S >= End) { break; }

        if (!InEnt)
        {
            if (*S == TEXT('{')) { InEnt = true; KV.Reset(); PendingOutputs.Reset(); }
            ++S;
            continue;
        }

        if (*S == TEXT('}')) { Flush(); InEnt = false; ++S; continue; }

        // Expect quoted key
        if (*S != TEXT('"')) { ++S; continue; }
        ++S;
        const TCHAR* K0 = S;
        while (S < End && *S != TEXT('"')) { ++S; }
        if (S >= End) { break; }
        FString Key(static_cast<int32>(S - K0), K0);
        ++S; // past closing quote

        // Skip inter-token whitespace
        while (S < End && (*S == TEXT(' ') || *S == TEXT('\t'))) { ++S; }

        // Expect quoted value; if missing, drop this key and resync.
        if (S >= End || *S != TEXT('"')) { continue; }
        ++S;
        const TCHAR* V0 = S;
        while (S < End && *S != TEXT('"')) { ++S; }
        if (S >= End) { break; }
        FString Val(static_cast<int32>(S - V0), V0);
        ++S; // past closing quote

        // Source I/O outputs: all keys that begin with "On" (case-insensitive). Append to a
        // per-entity list rather than collapsing through KV, so duplicate output keys are
        // preserved (one OnTrigger per downstream target).
        if (Key.Len() >= 2 && (Key[0] == TEXT('O') || Key[0] == TEXT('o'))
                          && (Key[1] == TEXT('N') || Key[1] == TEXT('n')))
        {
            FHL2EntityIO IO;
            IO.OutputName = Key;
            if (ParseIoValue(Val, IO))
            {
                PendingOutputs.Add(MoveTemp(IO));
            }
            continue;
        }

        KV.Add(MoveTemp(Key), MoveTemp(Val));
    }

    UE_LOG(LogHL2BSPImporter, Log, TEXT("Entities: %d parsed, %lld I/O outputs."),
        Out.Num(), TotalOutputs);
}
