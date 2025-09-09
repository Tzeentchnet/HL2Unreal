#include "BspFile.h"
#include "HL2BSPImporter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Source/HL2 BSP (VBSP v20) minimal reader for faces/verts and texnames.

#pragma pack(push, 1)
struct FLumpInfo { int32 Ofs; int32 Len; int32 Version; int32 FourCC; };
struct FBspHeader { int32 Ident; int32 Version; FLumpInfo Lumps[64]; int32 MapRevision; };
struct DVertex { float Pos[3]; };
struct DEdge { uint16 V[2]; };
struct DFace
{
    uint16 Planenum; uint8 Side; uint8 OnNode; int32 FirstEdge; int16 NumEdges; int16 TexInfo; int16 DispInfo; int16 SurfaceFogVolumeID;
    uint8 Styles[4]; int32 Lightofs; float Area; int32 LmMins[2]; int32 LmSize[2]; int32 OrigFace; uint16 NumPrims; uint16 FirstPrimID; uint32 SmoothingGroups;
};
struct DTexInfo { float TextureVecs[2][4]; float LightmapVecs[2][4]; int32 Flags; int32 TexData; };
struct DTexData { float Reflectivity[3]; int32 NameStringTableID; int32 Width; int32 Height; int32 ViewWidth; int32 ViewHeight; };
#pragma pack(pop)

static FORCEINLINE bool ReadArray(const TArray<uint8>& Data, int32 Ofs, int32 Count, void* Out)
{
    int64 Need = (int64)Ofs + Count;
    if (Ofs < 0 || Need > Data.Num()) return false;
    FMemory::Memcpy(Out, Data.GetData() + Ofs, Count);
    return true;
}

bool FBspFile::LoadFromFile(const FString& Filename)
{
    Vertices.Reset();
    Faces.Reset();
    DispInfos.Reset();
    DispVerts.Reset();

    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("BSP LoadFileToArray failed: %s"), *Filename);
        return false;
    }

    if (Bytes.Num() < (int32)sizeof(FBspHeader)) { UE_LOG(LogHL2BSPImporter, Error, TEXT("BSP too small for header: %s (size=%d)"), *Filename, Bytes.Num()); return false; }
    FBspHeader H{};
    FMemory::Memcpy(&H, Bytes.GetData(), sizeof(FBspHeader));
    const int32 VBSP = int32('V') | (int32('B') << 8) | (int32('S') << 16) | (int32('P') << 24);
    if (H.Ident != VBSP) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Wrong BSP magic. Expected 'VBSP' got 0x%08x for %s"), H.Ident, *Filename); return false; }
    UE_LOG(LogHL2BSPImporter, Log, TEXT("VBSP header: Version=%d MapRevision=%d"), H.Version, H.MapRevision);

    const FLumpInfo& LVerts = H.Lumps[3]; // LUMP_VERTEXES
    const FLumpInfo& LEdges = H.Lumps[12]; // LUMP_EDGES
    const FLumpInfo& LSurfEdges = H.Lumps[13]; // LUMP_SURFEDGES
    const FLumpInfo& LFaces = H.Lumps[7]; // LUMP_FACES
    const FLumpInfo& LTexInfo = H.Lumps[6]; // LUMP_TEXINFO
    const FLumpInfo& LTexData = H.Lumps[2]; // LUMP_TEXDATA
    const FLumpInfo& LTexStrTab = H.Lumps[43]; // LUMP_TEXDATA_STRING_TABLE
    const FLumpInfo& LTexStrData = H.Lumps[44]; // LUMP_TEXDATA_STRING_DATA

    // Load vertices
    const int32 NumSrcVerts = LVerts.Len / sizeof(DVertex);
    TArray<DVertex> SrcVerts; SrcVerts.SetNum(NumSrcVerts);
    if (!ReadArray(Bytes, LVerts.Ofs, LVerts.Len, SrcVerts.GetData())) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed reading LUMP_VERTEXES (ofs=%d len=%d)"), LVerts.Ofs, LVerts.Len); return false; }

    // Load edges
    const int32 NumEdges = LEdges.Len / sizeof(DEdge);
    TArray<DEdge> Edges; Edges.SetNum(NumEdges);
    if (!ReadArray(Bytes, LEdges.Ofs, LEdges.Len, Edges.GetData())) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed reading LUMP_EDGES (ofs=%d len=%d)"), LEdges.Ofs, LEdges.Len); return false; }

    // Load surfedges (int32 indices, may be negative)
    const int32 NumSurfEdges = LSurfEdges.Len / sizeof(int32);
    TArray<int32> SurfEdges; SurfEdges.SetNum(NumSurfEdges);
    if (!ReadArray(Bytes, LSurfEdges.Ofs, LSurfEdges.Len, SurfEdges.GetData())) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed reading LUMP_SURFEDGES (ofs=%d len=%d)"), LSurfEdges.Ofs, LSurfEdges.Len); return false; }

    // Load faces
    const int32 NumFaces = LFaces.Len / sizeof(DFace);
    TArray<DFace> FacesSrc; FacesSrc.SetNum(NumFaces);
    if (!ReadArray(Bytes, LFaces.Ofs, LFaces.Len, FacesSrc.GetData())) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed reading LUMP_FACES (ofs=%d len=%d)"), LFaces.Ofs, LFaces.Len); return false; }

    // Load texinfo
    const int32 NumTexInfos = LTexInfo.Len / sizeof(DTexInfo);
    TArray<DTexInfo> TexInfos; TexInfos.SetNum(NumTexInfos);
    if (!ReadArray(Bytes, LTexInfo.Ofs, LTexInfo.Len, TexInfos.GetData())) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed reading LUMP_TEXINFO (ofs=%d len=%d)"), LTexInfo.Ofs, LTexInfo.Len); return false; }

    // Load texdata
    const int32 NumTexData = LTexData.Len / sizeof(DTexData);
    TArray<DTexData> TexDatas; TexDatas.SetNum(NumTexData);
    if (!ReadArray(Bytes, LTexData.Ofs, LTexData.Len, TexDatas.GetData())) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed reading LUMP_TEXDATA (ofs=%d len=%d)"), LTexData.Ofs, LTexData.Len); return false; }

    // Load texture string table and data
    const int32 NumStrOffsets = LTexStrTab.Len / sizeof(int32);
    TArray<int32> StrOffsets; StrOffsets.SetNum(NumStrOffsets);
    if (!ReadArray(Bytes, LTexStrTab.Ofs, LTexStrTab.Len, StrOffsets.GetData())) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed reading LUMP_TEXDATA_STRING_TABLE (ofs=%d len=%d)"), LTexStrTab.Ofs, LTexStrTab.Len); return false; }
    TArray<uint8> StrData; StrData.SetNum(LTexStrData.Len);
    if (!ReadArray(Bytes, LTexStrData.Ofs, LTexStrData.Len, StrData.GetData())) { UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed reading LUMP_TEXDATA_STRING_DATA (ofs=%d len=%d)"), LTexStrData.Ofs, LTexStrData.Len); return false; }

    UE_LOG(LogHL2BSPImporter, Log, TEXT("VBSP header OK. Verts=%d Edges=%d SurfEdges=%d Faces=%d TexInfo=%d TexData=%d StrTab=%d"),
        NumSrcVerts, NumEdges, NumSurfEdges, NumFaces, NumTexInfos, NumTexData, NumStrOffsets);

    auto GetTexName = [&](int32 TexInfoIndex) -> FString
    {
        if (TexInfoIndex < 0 || TexInfoIndex >= TexInfos.Num()) return FString();
        const int32 TexDataIndex = TexInfos[TexInfoIndex].TexData;
        if (TexDataIndex < 0 || TexDataIndex >= TexDatas.Num()) return FString();
        const int32 StrIdx = TexDatas[TexDataIndex].NameStringTableID;
        if (StrIdx < 0 || StrIdx >= StrOffsets.Num()) return FString();
        const int32 Ofs = StrOffsets[StrIdx];
        if (Ofs < 0 || Ofs >= StrData.Num()) return FString();
        const ANSICHAR* Start = (const ANSICHAR*)(StrData.GetData() + Ofs);
        return FString(UTF8_TO_TCHAR(Start));
    };

    auto ComputeUV = [&](const FVector& P, int32 TexInfoIndex) -> FVector2D
    {
        if (TexInfoIndex < 0 || TexInfoIndex >= TexInfos.Num()) return FVector2D::ZeroVector;
        const DTexInfo& TI = TexInfos[TexInfoIndex];
        const FVector4 S(TI.TextureVecs[0][0], TI.TextureVecs[0][1], TI.TextureVecs[0][2], TI.TextureVecs[0][3]);
        const FVector4 T(TI.TextureVecs[1][0], TI.TextureVecs[1][1], TI.TextureVecs[1][2], TI.TextureVecs[1][3]);
        float u = FVector::DotProduct(P, FVector(S.X, S.Y, S.Z)) + S.W;
        float v = FVector::DotProduct(P, FVector(T.X, T.Y, T.Z)) + T.W;
        // Optional: normalize by texture size if available
        const int32 TexDataIndex = TI.TexData;
        if (TexDataIndex >= 0 && TexDataIndex < TexDatas.Num())
        {
            const float W = FMath::Max(1, TexDatas[TexDataIndex].Width);
            const float H = FMath::Max(1, TexDatas[TexDataIndex].Height);
            u /= W; v /= H;
        }
        return FVector2D(u, v);
    };

    // Build faces
    for (int32 f = 0; f < FacesSrc.Num(); ++f)
    {
        const DFace& DF = FacesSrc[f];
        if (DF.NumEdges < 3) continue;
        const int32 StartIndex = Vertices.Num();
        TArray<int32> PolyVertIdx; PolyVertIdx.Reserve(DF.NumEdges);
        for (int32 i = 0; i < DF.NumEdges; ++i)
        {
            const int32 SeIdx = SurfEdges[DF.FirstEdge + i];
            int32 EdgeIndex = FMath::Abs(SeIdx);
            if (EdgeIndex < 0 || EdgeIndex >= Edges.Num()) continue;
            const DEdge& E = Edges[EdgeIndex];
            const int32 VIdx = (SeIdx >= 0) ? E.V[0] : E.V[1];
            if (VIdx < 0 || VIdx >= SrcVerts.Num()) continue;
            FVector P(SrcVerts[VIdx].Pos[0], SrcVerts[VIdx].Pos[1], SrcVerts[VIdx].Pos[2]);
            FVector2D UV = ComputeUV(P, DF.TexInfo);
            Vertices.Add({ P, UV });
        }
        const int32 NumAdded = Vertices.Num() - StartIndex;
        if (NumAdded >= 3)
        {
            FBspFace OutF;
            OutF.FirstVertex = StartIndex;
            OutF.NumVertices = NumAdded;
            OutF.TextureName = GetTexName(DF.TexInfo);
            Faces.Add(MoveTemp(OutF));
        }
    }

    // Displacements (optional)
    const int32 LUMP_DISPINFO = 26;
    const int32 LUMP_DISP_VERTS = 33;
    const FLumpInfo& LDispInfo = H.Lumps[LUMP_DISPINFO];
    const FLumpInfo& LDispVerts = H.Lumps[LUMP_DISP_VERTS];

#pragma pack(push, 1)
    struct DDispInfo
    {
        FVector StartPosition; // float[3]
        int32 DispVertStart;
        int32 DispTriStart;
        int32 Power;
        int32 MinTess; float SmoothingAngle; int32 Contents; uint16 MapFace; uint32 Flags; int32 EntityNum;
        uint8 LightmapAlphaStart; uint8 LightmapSamplePositionStart; uint32 EdgeNeighbors[4]; uint32 CornerNeighbors[4]; uint32 AllowedVerts[10];
    };
    struct DDispVert { FVector Vector; float Dist; float Alpha; };
#pragma pack(pop)

    if (LDispInfo.Len >= (int32)sizeof(DDispInfo))
    {
        const int32 NumDispInfos = LDispInfo.Len / sizeof(DDispInfo);
        TArray<DDispInfo> Disp; Disp.SetNum(NumDispInfos);
        if (ReadArray(Bytes, LDispInfo.Ofs, LDispInfo.Len, Disp.GetData()))
        {
            DispInfos.Reset(); DispInfos.Reserve(NumDispInfos);
            for (const DDispInfo& D : Disp)
            {
                FDispInfo O; O.Power = D.Power; O.VertStart = D.DispVertStart; O.MapFace = (int32)D.MapFace; DispInfos.Add(O);
            }
        }
    }
    if (LDispVerts.Len >= (int32)sizeof(DDispVert))
    {
        const int32 NumDV = LDispVerts.Len / sizeof(DDispVert);
        TArray<DDispVert> DV; DV.SetNum(NumDV);
        if (ReadArray(Bytes, LDispVerts.Ofs, LDispVerts.Len, DV.GetData()))
        {
            DispVerts.Reset(); DispVerts.Reserve(NumDV);
            for (const DDispVert& V : DV)
            {
                FDispVert Out; Out.Vector[0] = V.Vector.X; Out.Vector[1] = V.Vector.Y; Out.Vector[2] = V.Vector.Z; DispVerts.Add(Out);
            }
        }
    }

    // Entities (text lump)
    const FLumpInfo& LEnts = H.Lumps[0];
    if (LEnts.Len > 0)
    {
        FString EntText;
        EntText.Reserve(LEnts.Len);
        TArray<TCHAR> Buffer; Buffer.SetNumUninitialized(LEnts.Len + 1);
        for (int32 i = 0; i < LEnts.Len; ++i)
        {
            Buffer[i] = (TCHAR)Bytes[LEnts.Ofs + i];
        }
        Buffer[LEnts.Len] = 0;
        EntText = FString(Buffer.GetData());

        TArray<FHL2Entity> Out;
        TMap<FString, FString> KV;
        auto Flush = [&]()
        {
            if (KV.Num() == 0) return;
            FHL2Entity E;
            KV.RemoveAndCopyValue(TEXT("targetname"), E.Name);
            KV.RemoveAndCopyValue(TEXT("classname"), E.Class);
            FString OriginStr; if (KV.RemoveAndCopyValue(TEXT("origin"), OriginStr))
            {
                TArray<FString> Parts; OriginStr.ParseIntoArrayWS(Parts);
                if (Parts.Num() == 3)
                {
                    E.Origin = FVector(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
                }
            }
            FString Angles; if (KV.RemoveAndCopyValue(TEXT("angles"), Angles))
            {
                TArray<FString> A; Angles.ParseIntoArrayWS(A);
                if (A.Num() == 3)
                {
                    E.Rotation = FRotator(FCString::Atof(*A[0]), FCString::Atof(*A[1]), FCString::Atof(*A[2]));
                }
            }
            KV.RemoveAndCopyValue(TEXT("model"), E.Model);
            Out.Add(E);
            KV.Reset();
        };

        const TCHAR* S = *EntText;
        bool InEnt = false;
        while (*S)
        {
            // Skip whitespace
            while (*S && (*S == TEXT(' ') || *S == TEXT('\t') || *S == TEXT('\r') || *S == TEXT('\n'))) ++S;
            if (!*S) break;

            if (!InEnt)
            {
                if (*S == TEXT('{')) { InEnt = true; KV.Reset(); ++S; continue; }
                ++S; continue;
            }

            if (*S == TEXT('}')) { Flush(); InEnt = false; ++S; continue; }

            // Expect key
            if (*S != TEXT('"')) { ++S; continue; }
            ++S; const TCHAR* K0 = S; while (*S && *S != TEXT('"')) ++S; FString Key(S - K0, K0);
            if (*S == TEXT('"')) ++S;
            while (*S && (*S == TEXT(' ') || *S == TEXT('\t'))) ++S;
            if (*S != TEXT('"')) { continue; }
            ++S; const TCHAR* V0 = S; while (*S && *S != TEXT('"')) ++S; FString Val(S - V0, V0);
            if (*S == TEXT('"')) ++S;
            KV.Add(Key, Val);
        }

        Entities = MoveTemp(Out);
    }

    UE_LOG(LogHL2BSPImporter, Log, TEXT("BSP parsed: OutVerts=%d OutFaces=%d DispInfos=%d DispVerts=%d Entities=%d"),
        Vertices.Num(), Faces.Num(), DispInfos.Num(), DispVerts.Num(), Entities.Num());
    return true;
}
