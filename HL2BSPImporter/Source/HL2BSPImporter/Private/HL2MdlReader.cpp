#include "HL2MdlReader.h"
#include "HL2BSPImporter.h"

// Source `studiohdr_t` parser. Targets versions 44..49 (HL2 → Source 2013).
//
// We use explicit byte offsets rather than mirroring the full ~408-byte
// `studiohdr_t` because (a) only a handful of fields matter for static-prop
// import, and (b) the on-disk layout has subtle pointer-sizing pitfalls
// (`mstudio_meshvertexdata_t` declares a `void*` that is treated as int32 in
// the file format regardless of host architecture). Hard-coded offsets
// document the contract precisely.
//
// String resolution conventions:
//   - `sznameindex` on bodypart/model/mesh/texture is relative to that
//     struct's own start: `name = (char*)struct + sznameindex`.
//   - `cdtextureindex` on the studio header points to an array of int32
//     offsets, each relative to the file start, pointing to a NUL-terminated
//     C string.

namespace HL2Mdl
{
    namespace
    {
        constexpr uint32 MDL_ID         = 0x54534449u; // 'IDST' little-endian
        constexpr int32  MIN_VERSION    = 44;
        constexpr int32  MAX_VERSION    = 49;
        constexpr int32  STUDIOHDR_MIN  = 408;          // smallest header we accept
        constexpr int32  BODYPART_STRIDE = 16;          // mstudiobodyparts_t
        constexpr int32  MODEL_STRIDE    = 148;         // mstudiomodel_t (excluding pad)
                                                        //   actual file size = 156 incl. 8-byte vertexdata + 32-byte unused
        constexpr int32  MODEL_FILE_SIZE = 148 + 8 + 32;// 188 bytes total per model entry on disk
        constexpr int32  MESH_FILE_SIZE  = 116;         // mstudiomesh_t on disk
        constexpr int32  TEXTURE_STRIDE  = 64;          // mstudiotexture_t on disk
        constexpr int32  STRING_MAX      = 256;

        // Header field offsets within studiohdr_t (v44..v49).
        constexpr int32 OFS_id              = 0;
        constexpr int32 OFS_version         = 4;
        constexpr int32 OFS_checksum        = 8;
        constexpr int32 OFS_name            = 12;   // char[64]
        constexpr int32 OFS_length          = 76;
        constexpr int32 OFS_hull_min        = 104;  // Vector
        constexpr int32 OFS_hull_max        = 116;  // Vector
        constexpr int32 OFS_flags           = 152;
        constexpr int32 OFS_numtextures     = 204;
        constexpr int32 OFS_textureindex    = 208;
        constexpr int32 OFS_numcdtextures   = 212;
        constexpr int32 OFS_cdtextureindex  = 216;
        constexpr int32 OFS_numskinref      = 220;
        constexpr int32 OFS_numskinfamilies = 224;
        constexpr int32 OFS_skinindex       = 228;
        constexpr int32 OFS_numbodyparts    = 232;
        constexpr int32 OFS_bodypartindex   = 236;

        template<typename T>
        FORCEINLINE bool ReadAt(const TArray<uint8>& B, int32 Ofs, T& Out)
        {
            if (Ofs < 0 || (int64)Ofs + (int64)sizeof(T) > B.Num()) { return false; }
            FMemory::Memcpy(&Out, B.GetData() + Ofs, sizeof(T));
            return true;
        }

        FORCEINLINE int32 ReadI32(const TArray<uint8>& B, int32 Ofs, bool& bOk)
        {
            int32 V = 0;
            if (!ReadAt(B, Ofs, V)) { bOk = false; }
            return V;
        }

        // Read a NUL-terminated C string from B starting at AbsOfs (0..STRING_MAX).
        FString ReadCString(const TArray<uint8>& B, int32 AbsOfs)
        {
            if (AbsOfs < 0 || AbsOfs >= B.Num()) { return {}; }
            const int32 Avail = FMath::Min<int32>(STRING_MAX, B.Num() - AbsOfs);
            const ANSICHAR* Start = reinterpret_cast<const ANSICHAR*>(B.GetData() + AbsOfs);
            int32 Len = 0;
            while (Len < Avail && Start[Len] != '\0') { ++Len; }
            return FString(Len, Start);
        }

        // Read fixed-length char[N] field starting at AbsOfs.
        FString ReadFixedString(const TArray<uint8>& B, int32 AbsOfs, int32 MaxLen)
        {
            if (AbsOfs < 0 || (int64)AbsOfs + (int64)MaxLen > B.Num()) { return {}; }
            const ANSICHAR* Start = reinterpret_cast<const ANSICHAR*>(B.GetData() + AbsOfs);
            int32 Len = 0;
            while (Len < MaxLen && Start[Len] != '\0') { ++Len; }
            return FString(Len, Start);
        }

        // Source paths use backslashes; normalise to forward + lower-case for
        // material-cache key compatibility with HL2Mat::FBuilder.
        FString NormaliseTexturePath(const FString& In)
        {
            FString Out = In.Replace(TEXT("\\"), TEXT("/"));
            Out.ToLowerInline();
            // Strip leading 'materials/' if a prop's cdtex baked it in.
            if (Out.StartsWith(TEXT("materials/"))) { Out.RightChopInline(10, EAllowShrinking::No); }
            while (Out.StartsWith(TEXT("/"))) { Out.RightChopInline(1, EAllowShrinking::No); }
            return Out;
        }
    } // namespace

    bool Parse(const TArray<uint8>& Bytes, HL2Studio::FStudioFile& Out, FString& OutError)
    {
        Out = HL2Studio::FStudioFile{};

        if (Bytes.Num() < STUDIOHDR_MIN)
        {
            OutError = FString::Printf(TEXT("MDL too small (%d bytes)"), Bytes.Num());
            return false;
        }

        bool bOk = true;
        const uint32 Id      = (uint32)ReadI32(Bytes, OFS_id, bOk);
        const int32  Version = ReadI32(Bytes, OFS_version, bOk);
        const int32  Checksum= ReadI32(Bytes, OFS_checksum, bOk);
        const int32  Length  = ReadI32(Bytes, OFS_length, bOk);
        if (!bOk) { OutError = TEXT("MDL header read failed"); return false; }

        if (Id != MDL_ID)
        {
            OutError = FString::Printf(TEXT("MDL bad magic 0x%08x (expected 'IDST' 0x%08x)"), Id, MDL_ID);
            return false;
        }
        if (Version < MIN_VERSION || Version > MAX_VERSION)
        {
            OutError = FString::Printf(TEXT("MDL version %d out of supported range (%d..%d)"),
                Version, MIN_VERSION, MAX_VERSION);
            return false;
        }
        if (Length > 0 && Length > Bytes.Num())
        {
            OutError = FString::Printf(TEXT("MDL header.length=%d exceeds file size %d"), Length, Bytes.Num());
            return false;
        }

        Out.Version  = Version;
        Out.Checksum = Checksum;
        ReadAt(Bytes, OFS_flags, Out.Flags);

        FVector HullMin = FVector::ZeroVector, HullMax = FVector::ZeroVector;
        ReadAt(Bytes, OFS_hull_min, HullMin);
        ReadAt(Bytes, OFS_hull_max, HullMax);
        Out.BBox = FBox(HullMin, HullMax);

        // ---- Materials ----
        const int32 NumTextures   = ReadI32(Bytes, OFS_numtextures,   bOk);
        const int32 TextureIndex  = ReadI32(Bytes, OFS_textureindex,  bOk);
        const int32 NumCdTextures = ReadI32(Bytes, OFS_numcdtextures, bOk);
        const int32 CdTextureIndex= ReadI32(Bytes, OFS_cdtextureindex,bOk);
        if (!bOk) { OutError = TEXT("MDL texture-table header read failed"); return false; }
        if (NumTextures < 0 || NumTextures > 4096 || NumCdTextures < 0 || NumCdTextures > 256)
        {
            OutError = TEXT("MDL texture counts out of range");
            return false;
        }

        TArray<FString> CdTextures;
        CdTextures.Reserve(NumCdTextures);
        for (int32 i = 0; i < NumCdTextures; ++i)
        {
            int32 Off = 0;
            if (!ReadAt(Bytes, CdTextureIndex + i * 4, Off))
            {
                OutError = TEXT("MDL cdtexture offset read failed");
                return false;
            }
            FString Cd = ReadCString(Bytes, Off);
            // HL2 ships some cdtexture entries with no trailing slash; strip if present
            // for consistent join.
            Cd = Cd.Replace(TEXT("\\"), TEXT("/"));
            while (Cd.EndsWith(TEXT("/"))) { Cd.LeftChopInline(1, EAllowShrinking::No); }
            CdTextures.Add(MoveTemp(Cd));
        }

        TArray<FString> TextureNames;
        TextureNames.Reserve(NumTextures);
        for (int32 i = 0; i < NumTextures; ++i)
        {
            const int32 TexOfs = TextureIndex + i * TEXTURE_STRIDE;
            int32 NameOfs = 0;
            if (!ReadAt(Bytes, TexOfs + 0, NameOfs))   // sznameindex at offset 0
            {
                OutError = TEXT("MDL texture entry read failed");
                return false;
            }
            const FString TexName = ReadCString(Bytes, TexOfs + NameOfs);
            TextureNames.Add(TexName);
        }

        Out.MaterialCandidates.SetNum(NumTextures);
        for (int32 i = 0; i < NumTextures; ++i)
        {
            const FString& TexName = TextureNames[i];
            if (CdTextures.Num() == 0)
            {
                Out.MaterialCandidates[i].Add(NormaliseTexturePath(TexName));
            }
            else
            {
                for (const FString& Cd : CdTextures)
                {
                    const FString Joined = Cd.IsEmpty() ? TexName : (Cd / TexName);
                    Out.MaterialCandidates[i].Add(NormaliseTexturePath(Joined));
                }
            }
        }

        // ---- Skin table ----
        // `mstudiomesh_t::material` indexes into a per-mesh reference slot. The
        // skin table maps `[family][refSlot] -> textureIdx`. Layout on disk is
        // `int16 skinref[numskinfamilies][numskinref]` at byte offset
        // `skinindex` from the start of `studiohdr_t` (file start). When the
        // table is missing or malformed we leave Out.Skin empty; the builder
        // then treats mesh.MaterialIndex as a direct texture index (identity
        // skin 0).
        const int32 NumSkinRef      = ReadI32(Bytes, OFS_numskinref,      bOk);
        const int32 NumSkinFamilies = ReadI32(Bytes, OFS_numskinfamilies, bOk);
        const int32 SkinIndex       = ReadI32(Bytes, OFS_skinindex,       bOk);
        if (!bOk) { OutError = TEXT("MDL skin-table header read failed"); return false; }
        if (NumSkinRef > 0 && NumSkinFamilies > 0
            && NumSkinRef <= 4096 && NumSkinFamilies <= 256
            && SkinIndex > 0)
        {
            const int64 BytesNeeded = (int64)NumSkinFamilies * (int64)NumSkinRef * 2;
            if ((int64)SkinIndex + BytesNeeded <= Bytes.Num())
            {
                Out.Skin.NumSkinRef      = NumSkinRef;
                Out.Skin.NumSkinFamilies = NumSkinFamilies;
                Out.Skin.SkinFamilies.SetNum(NumSkinFamilies);
                const uint8* SkinPtr = Bytes.GetData() + SkinIndex;
                for (int32 Fam = 0; Fam < NumSkinFamilies; ++Fam)
                {
                    TArray<int32>& Row = Out.Skin.SkinFamilies[Fam];
                    Row.SetNumUninitialized(NumSkinRef);
                    for (int32 Slot = 0; Slot < NumSkinRef; ++Slot)
                    {
                        int16 V = 0;
                        FMemory::Memcpy(&V, SkinPtr + ((int64)Fam * NumSkinRef + Slot) * 2, 2);
                        Row[Slot] = (int32)V;
                    }
                }
            }
            else
            {
                UE_LOG(LogHL2BSPImporter, Verbose,
                    TEXT("MDL skin table extends past EOF (skinindex=%d, families=%d, refs=%d, bytesNeeded=%lld, fileSize=%d); ignoring."),
                    SkinIndex, NumSkinFamilies, NumSkinRef, (long long)BytesNeeded, Bytes.Num());
            }
        }

        // ---- Body parts / models / meshes ----
        const int32 NumBodyParts  = ReadI32(Bytes, OFS_numbodyparts,  bOk);
        const int32 BodyPartIndex = ReadI32(Bytes, OFS_bodypartindex, bOk);
        if (!bOk) { OutError = TEXT("MDL bodypart header read failed"); return false; }
        if (NumBodyParts < 0 || NumBodyParts > 256)
        {
            OutError = FString::Printf(TEXT("MDL bodypart count %d out of range"), NumBodyParts);
            return false;
        }

        Out.BodyParts.Reserve(NumBodyParts);
        for (int32 BpIdx = 0; BpIdx < NumBodyParts; ++BpIdx)
        {
            const int32 BpOfs = BodyPartIndex + BpIdx * BODYPART_STRIDE;
            int32 BpNameOfs = 0, NumModels = 0, BpBase = 0, ModelIndex = 0;
            if (!ReadAt(Bytes, BpOfs + 0,  BpNameOfs)
             || !ReadAt(Bytes, BpOfs + 4,  NumModels)
             || !ReadAt(Bytes, BpOfs + 8,  BpBase)
             || !ReadAt(Bytes, BpOfs + 12, ModelIndex))
            {
                OutError = FString::Printf(TEXT("MDL bodypart %d read failed"), BpIdx);
                return false;
            }
            if (NumModels < 0 || NumModels > 64)
            {
                OutError = FString::Printf(TEXT("MDL bodypart %d nummodels=%d out of range"), BpIdx, NumModels);
                return false;
            }

            HL2Studio::FStudioBodyPart Bp;
            Bp.Name = ReadCString(Bytes, BpOfs + BpNameOfs);
            Bp.Models.Reserve(NumModels);

            for (int32 MdlIdx = 0; MdlIdx < NumModels; ++MdlIdx)
            {
                const int32 ModelOfs = BpOfs + ModelIndex + MdlIdx * MODEL_FILE_SIZE;

                int32 NumMeshes = 0, MeshIndex = 0, NumVerts = 0, VertexByteOfs = 0;
                if (!ReadAt(Bytes, ModelOfs + 72, NumMeshes)        // mstudiomodel_t.nummeshes
                 || !ReadAt(Bytes, ModelOfs + 76, MeshIndex)        // mstudiomodel_t.meshindex (rel to model)
                 || !ReadAt(Bytes, ModelOfs + 80, NumVerts)         // mstudiomodel_t.numvertices
                 || !ReadAt(Bytes, ModelOfs + 84, VertexByteOfs))   // mstudiomodel_t.vertexindex (byte ofs into VVD)
                {
                    OutError = FString::Printf(TEXT("MDL model %d/%d read failed"), BpIdx, MdlIdx);
                    return false;
                }
                if (NumMeshes < 0 || NumMeshes > 256 || NumVerts < 0 || NumVerts > 524288)
                {
                    OutError = FString::Printf(TEXT("MDL model %d/%d counts out of range (meshes=%d verts=%d)"),
                        BpIdx, MdlIdx, NumMeshes, NumVerts);
                    return false;
                }

                HL2Studio::FStudioModel Model;
                Model.Name        = ReadFixedString(Bytes, ModelOfs + 0, 64);
                Model.NumVertices = NumVerts;
                // VVD vertex stride is 48 bytes (mstudiovertex_t). Convert to a vertex index.
                Model.VertexBase  = (VertexByteOfs >= 0) ? (VertexByteOfs / 48) : 0;
                Model.Meshes.Reserve(NumMeshes);

                for (int32 MeshIdx = 0; MeshIdx < NumMeshes; ++MeshIdx)
                {
                    const int32 MeshOfs = ModelOfs + MeshIndex + MeshIdx * MESH_FILE_SIZE;
                    int32 MaterialIdx = 0, MeshNumVerts = 0, MeshVertexOffset = 0;
                    if (!ReadAt(Bytes, MeshOfs + 0,  MaterialIdx)       // mstudiomesh_t.material
                     || !ReadAt(Bytes, MeshOfs + 8,  MeshNumVerts)      // mstudiomesh_t.numvertices
                     || !ReadAt(Bytes, MeshOfs + 12, MeshVertexOffset)) // mstudiomesh_t.vertexoffset (vertex idx, model-relative)
                    {
                        OutError = FString::Printf(TEXT("MDL mesh %d/%d/%d read failed"), BpIdx, MdlIdx, MeshIdx);
                        return false;
                    }
                    if (MaterialIdx < 0 || MaterialIdx >= NumTextures)
                    {
                        UE_LOG(LogHL2BSPImporter, Verbose,
                            TEXT("MDL mesh %d/%d/%d material idx %d out of range (numtex=%d); clamping to 0"),
                            BpIdx, MdlIdx, MeshIdx, MaterialIdx, NumTextures);
                        MaterialIdx = 0;
                    }

                    HL2Studio::FStudioMesh Mesh;
                    Mesh.MaterialIndex = MaterialIdx;
                    Mesh.NumVertices   = MeshNumVerts;
                    Mesh.VertexOffset  = MeshVertexOffset;
                    Model.Meshes.Add(MoveTemp(Mesh));
                }

                Bp.Models.Add(MoveTemp(Model));
            }

            Out.BodyParts.Add(MoveTemp(Bp));
        }

        UE_LOG(LogHL2BSPImporter, Verbose,
            TEXT("MDL parsed: v%d, %d bodyparts, %d textures, %d cdtex, hull (%g..%g)"),
            Version, NumBodyParts, NumTextures, NumCdTextures,
            HullMin.Size(), HullMax.Size());
        return true;
    }
}
