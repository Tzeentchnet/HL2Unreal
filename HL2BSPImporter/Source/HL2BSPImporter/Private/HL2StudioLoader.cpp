#include "HL2StudioLoader.h"
#include "HL2BSPImporter.h"
#include "HL2MdlReader.h"
#include "HL2VvdReader.h"
#include "HL2VtxReader.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace HL2Studio
{
    namespace
    {
        FString NormaliseModelPath(const FString& In)
        {
            FString P = In.Replace(TEXT("\\"), TEXT("/"));
            P.ToLowerInline();
            if (!P.EndsWith(TEXT(".mdl"))) { P += TEXT(".mdl"); }
            // Source paths are conventionally rooted at "models/" but some tools
            // strip it; accept either.
            while (P.StartsWith(TEXT("/"))) { P.RightChopInline(1, EAllowShrinking::No); }
            return P;
        }

        // Find `<root>/<rel>` under the roots; returns first existing match.
        // Source content is case-insensitive; we trust the platform FS here
        // (Windows is the dominant editor host; Linux users with ext4 will
        // need lower-cased mirrors, which is how community VPK extractors
        // already write them).
        FString FindFile(const TArray<FString>& Roots, const FString& Rel)
        {
            IFileManager& FM = IFileManager::Get();
            for (const FString& Root : Roots)
            {
                const FString P = Root / Rel;
                if (FM.FileExists(*P)) { return P; }
            }
            return {};
        }
    }

    bool LoadModel(
        const FString& ModelPath,
        const TArray<FString>& Roots,
        FStudioFile& OutFile,
        FString& OutError)
    {
        OutFile = FStudioFile{};

        const FString MdlRel = NormaliseModelPath(ModelPath);
        // Source's `.dx90.vtx` is the canonical triangulated vertex-index file
        // for desktop targets; `.vtx` (no `.dx90`) is the older fallback. We
        // try both in order.
        const FString VvdRel  = FPaths::ChangeExtension(MdlRel, TEXT(".vvd"));
        const FString Vtx1Rel = MdlRel.LeftChop(4) + TEXT(".dx90.vtx");
        const FString Vtx2Rel = FPaths::ChangeExtension(MdlRel, TEXT(".vtx"));

        const FString MdlAbs = FindFile(Roots, MdlRel);
        if (MdlAbs.IsEmpty())
        {
            OutError = FString::Printf(TEXT("MDL not found: %s"), *MdlRel);
            return false;
        }
        const FString VvdAbs = FindFile(Roots, VvdRel);
        if (VvdAbs.IsEmpty())
        {
            OutError = FString::Printf(TEXT("VVD not found: %s"), *VvdRel);
            return false;
        }
        FString VtxAbs = FindFile(Roots, Vtx1Rel);
        if (VtxAbs.IsEmpty()) { VtxAbs = FindFile(Roots, Vtx2Rel); }
        if (VtxAbs.IsEmpty())
        {
            OutError = FString::Printf(TEXT("VTX not found: %s (or %s)"), *Vtx1Rel, *Vtx2Rel);
            return false;
        }

        TArray<uint8> MdlBytes, VvdBytes, VtxBytes;
        if (!FFileHelper::LoadFileToArray(MdlBytes, *MdlAbs))
        { OutError = FString::Printf(TEXT("MDL read failed: %s"), *MdlAbs); return false; }
        if (!FFileHelper::LoadFileToArray(VvdBytes, *VvdAbs))
        { OutError = FString::Printf(TEXT("VVD read failed: %s"), *VvdAbs); return false; }
        if (!FFileHelper::LoadFileToArray(VtxBytes, *VtxAbs))
        { OutError = FString::Printf(TEXT("VTX read failed: %s"), *VtxAbs); return false; }

        if (!HL2Mdl::Parse(MdlBytes, OutFile, OutError))
        {
            OutError = FString::Printf(TEXT("MDL parse: %s (%s)"), *OutError, *MdlAbs);
            return false;
        }
        OutFile.MdlPath = MdlAbs;

        if (!HL2Vvd::Parse(VvdBytes, OutFile.Checksum, OutFile.Vertices, OutError))
        {
            OutError = FString::Printf(TEXT("VVD parse: %s (%s)"), *OutError, *VvdAbs);
            return false;
        }
        if (!HL2Vtx::Parse(VtxBytes, OutFile.Checksum, OutFile, OutError))
        {
            OutError = FString::Printf(TEXT("VTX parse: %s (%s)"), *OutError, *VtxAbs);
            return false;
        }

        // Verify at least one triangle landed somewhere.
        int64 TotalTris = 0;
        for (const FStudioBodyPart& Bp : OutFile.BodyParts)
        {
            for (const FStudioModel& M : Bp.Models)
            {
                for (const FStudioMesh& Me : M.Meshes)
                {
                    TotalTris += Me.TriangleIndices.Num() / 3;
                }
            }
        }
        if (TotalTris == 0)
        {
            OutError = FString::Printf(TEXT("Model produced no triangles: %s"), *MdlAbs);
            return false;
        }

        UE_LOG(LogHL2BSPImporter, Verbose,
            TEXT("Studio model loaded: %s (v%d, %d verts, %lld tris, %d materials)"),
            *MdlAbs, OutFile.Version, OutFile.Vertices.Num(),
            (long long)TotalTris, OutFile.MaterialCandidates.Num());
        return true;
    }
}
