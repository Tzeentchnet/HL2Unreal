#include "HL2SkyboxConverter.h"
#include "HL2BSPImporter.h"
#include "HL2VmtParser.h"
#include "HL2VtfReader.h"

#include "Engine/TextureCube.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace HL2Sky
{
    namespace
    {
        // The six skybox face suffixes, in the canonical UE5 cubemap face order
        // expected by UTexture::Source.Init(NumSlices=6, ...): +X, -X, +Y, -Y, +Z, -Z.
        // Source's authoring convention maps to:
        //   rt = +X, lf = -X, bk = +Y, ft = -Y, up = +Z, dn = -Z
        // The per-face rotation column matches AlleyKatPr0/HL2UE4AssetImporter's
        // SkyboxConverter.cpp — those rotations were reverse-engineered against
        // Source's GPU-side cubemap sweep.
        struct FFaceSlot
        {
            const TCHAR* Suffix;
            int32        SliceIndex;       // 0..5 in UE cubemap face order
            int32        Rotation;         // 0=none, 1=90° CCW, 2=180°, 3=90° CW
        };

        constexpr FFaceSlot kFaces[6] = {
            { TEXT("rt"), 0, 1 },   // +X, 90° CCW
            { TEXT("lf"), 1, 3 },   // -X, 90° CW
            { TEXT("bk"), 2, 2 },   // +Y, 180°
            { TEXT("ft"), 3, 0 },   // -Y, no rotation
            { TEXT("up"), 4, 0 },   // +Z, no rotation
            { TEXT("dn"), 5, 2 },   // -Z, 180°
        };

        FString StripFaceSuffix(const FString& In)
        {
            // Skybox face material names are written as "skybox/sky_day01_05bk".
            // Strip the trailing 2-character face suffix if present so we can
            // dedupe the six sides into a single base name.
            if (In.Len() < 2) { return In; }
            const FString Tail = In.Right(2).ToLower();
            for (const FFaceSlot& F : kFaces)
            {
                if (Tail == F.Suffix) { return In.Left(In.Len() - 2); }
            }
            return In;
        }

        FString FindFile(const FString& RelPath, const TArray<FString>& Roots)
        {
            for (const FString& Root : Roots)
            {
                FString Candidate = FPaths::Combine(Root, RelPath);
                FPaths::NormalizeFilename(Candidate);
                if (FPaths::FileExists(Candidate)) { return Candidate; }
            }
            return FString();
        }

        // Resolve <base><suffix>.vmt → its $basetexture path → <root>/materials/<basetexture>.vtf.
        // Falls back to <base><suffix>.vtf at the conventional location when no .vmt exists.
        FString ResolveFaceVtf(const FString& BaseName, const TCHAR* Suffix, const TArray<FString>& Roots)
        {
            const FString FaceBase = FString::Printf(TEXT("materials/skybox/%s%s"), *BaseName, Suffix);

            // Try .vmt first to honour any $basetexture indirection.
            FString VmtAbs = FindFile(FaceBase + TEXT(".vmt"), Roots);
            if (!VmtAbs.IsEmpty())
            {
                HL2VMT::FDocument Doc;
                FString Err;
                if (HL2VMT::ParseFile(VmtAbs, Doc, Err))
                {
                    HL2VMT::ResolvePatches(Doc, Roots, Err);
                    if (Doc.Root.IsValid())
                    {
                        FString BaseTex;
                        if (Doc.Root->GetString(TEXT("$basetexture"), BaseTex))
                        {
                            // $basetexture is relative to materials/, no .vtf suffix.
                            BaseTex.ReplaceInline(TEXT("\\"), TEXT("/"));
                            FString Rel = FString::Printf(TEXT("materials/%s.vtf"), *BaseTex);
                            FString VtfAbs = FindFile(Rel, Roots);
                            if (!VtfAbs.IsEmpty()) { return VtfAbs; }
                        }
                    }
                }
                else
                {
                    UE_LOG(LogHL2BSPImporter, Verbose, TEXT("Skybox face VMT parse failed (%s): %s"), *Err, *VmtAbs);
                }
            }

            // Fallback: same-name .vtf alongside the .vmt.
            return FindFile(FaceBase + TEXT(".vtf"), Roots);
        }

        // Bilinear upscale BGRA8 to (DstW, DstH). Used for the dn-face which
        // ships at a much smaller resolution than the side faces (e.g. 4×4 dn
        // on a 1024×1024 sky).
        void UpscaleBilinear(const uint8* SrcBgra, int32 SrcW, int32 SrcH,
                             TArray<uint8>& DstBgra, int32 DstW, int32 DstH)
        {
            DstBgra.SetNumUninitialized(static_cast<int64>(DstW) * DstH * 4);
            for (int32 Y = 0; Y < DstH; ++Y)
            {
                const float SrcY = (Y + 0.5f) * SrcH / DstH - 0.5f;
                const int32 Y0 = FMath::Clamp(FMath::FloorToInt(SrcY), 0, SrcH - 1);
                const int32 Y1 = FMath::Clamp(Y0 + 1, 0, SrcH - 1);
                const float Fy = FMath::Clamp(SrcY - Y0, 0.f, 1.f);
                for (int32 X = 0; X < DstW; ++X)
                {
                    const float SrcX = (X + 0.5f) * SrcW / DstW - 0.5f;
                    const int32 X0 = FMath::Clamp(FMath::FloorToInt(SrcX), 0, SrcW - 1);
                    const int32 X1 = FMath::Clamp(X0 + 1, 0, SrcW - 1);
                    const float Fx = FMath::Clamp(SrcX - X0, 0.f, 1.f);
                    for (int32 C = 0; C < 4; ++C)
                    {
                        const float A = SrcBgra[(Y0 * SrcW + X0) * 4 + C];
                        const float B = SrcBgra[(Y0 * SrcW + X1) * 4 + C];
                        const float Cc = SrcBgra[(Y1 * SrcW + X0) * 4 + C];
                        const float D = SrcBgra[(Y1 * SrcW + X1) * 4 + C];
                        const float Top = FMath::Lerp(A, B, Fx);
                        const float Bot = FMath::Lerp(Cc, D, Fx);
                        DstBgra[(Y * DstW + X) * 4 + C] = static_cast<uint8>(FMath::Clamp(FMath::Lerp(Top, Bot, Fy), 0.f, 255.f));
                    }
                }
            }
        }

        // Extrude the bottom row downward to fill the missing rows when a side
        // face is authored at half-height (e.g. 1024×512 on a 1024-tall sky).
        void ExtrudeBottomRow(const uint8* SrcBgra, int32 SrcW, int32 SrcH,
                              TArray<uint8>& DstBgra, int32 DstH)
        {
            DstBgra.SetNumUninitialized(static_cast<int64>(SrcW) * DstH * 4);
            const int64 RowBytes = static_cast<int64>(SrcW) * 4;
            FMemory::Memcpy(DstBgra.GetData(), SrcBgra, static_cast<int64>(SrcH) * RowBytes);
            // Replicate last source row across the remaining rows.
            const uint8* LastRow = SrcBgra + (SrcH - 1) * RowBytes;
            for (int32 Y = SrcH; Y < DstH; ++Y)
            {
                FMemory::Memcpy(DstBgra.GetData() + Y * RowBytes, LastRow, RowBytes);
            }
        }

        // Rotate a BGRA8 image. Rotation enum: 0=none, 1=90° CCW, 2=180°, 3=90° CW.
        void RotateBgra(const uint8* Src, int32 W, int32 H, int32 Rotation, TArray<uint8>& Dst)
        {
            const int64 NumPixels = static_cast<int64>(W) * H;
            Dst.SetNumUninitialized(NumPixels * 4);
            const int32 DstW = (Rotation == 1 || Rotation == 3) ? H : W;
            for (int32 Y = 0; Y < H; ++Y)
            {
                for (int32 X = 0; X < W; ++X)
                {
                    int32 Dx = X, Dy = Y;
                    switch (Rotation)
                    {
                        case 1: Dx = Y;             Dy = (W - 1) - X; break; // 90° CCW
                        case 2: Dx = (W - 1) - X;   Dy = (H - 1) - Y; break; // 180°
                        case 3: Dx = (H - 1) - Y;   Dy = X;           break; // 90° CW
                        default: break;
                    }
                    FMemory::Memcpy(Dst.GetData() + (Dy * DstW + Dx) * 4,
                                    Src + (Y * W + X) * 4, 4);
                }
            }
        }
    } // namespace

    UTextureCube* ConvertSkybox(const FConvertParams& Params, EConvertResult& OutResult, FString& OutError)
    {
        OutResult = EConvertResult::Created;
        OutError.Reset();

        if (Params.BaseName.IsEmpty() || Params.MaterialsRoots.Num() == 0 || Params.DestPackagePath.IsEmpty())
        {
            OutResult = EConvertResult::SaveFailed;
            OutError = TEXT("ConvertSkybox: invalid params");
            return nullptr;
        }

        // Sanitise asset name (no slashes, lower-case for consistency with material assets).
        FString AssetName = FPaths::GetCleanFilename(Params.BaseName);
        AssetName.ToLowerInline();

        const FString PkgFullName = Params.DestPackagePath / AssetName;

        // Cache hit?
        {
            FSoftObjectPath SOP(PkgFullName + TEXT(".") + AssetName);
            if (UTextureCube* Existing = Cast<UTextureCube>(SOP.TryLoad()))
            {
                OutResult = EConvertResult::Cached;
                return Existing;
            }
        }

        // Resolve all six faces up front so we know the cube dimensions.
        struct FResolvedFace
        {
            FString       VtfPath;
            HL2VTF::FInfo Info;
            TArray<uint8> Bgra;
        };
        FResolvedFace ResolvedFaces[6];
        int32 MaxW = 0, MaxH = 0;
        int32 MissingCount = 0;

        for (const FFaceSlot& F : kFaces)
        {
            FResolvedFace& R = ResolvedFaces[F.SliceIndex];
            R.VtfPath = ResolveFaceVtf(Params.BaseName, F.Suffix, Params.MaterialsRoots);
            if (R.VtfPath.IsEmpty())
            {
                ++MissingCount;
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Skybox '%s': face '%s' not found under any MaterialsRoot."),
                    *Params.BaseName, F.Suffix);
                continue;
            }

            FString DecodeErr;
            if (!HL2VTF::LoadAndDecode(R.VtfPath, R.Info, R.Bgra, DecodeErr))
            {
                // RGBA16161616F (HDR sky) is the dominant unsupported case.
                if (R.Info.Format == HL2VTF::EImageFormat::RGBA16161616F ||
                    R.Info.Format == HL2VTF::EImageFormat::RGBA16161616)
                {
                    OutResult = EConvertResult::UnsupportedFormat;
                    OutError = FString::Printf(TEXT("Skybox '%s': HDR format (%d) not supported in A1; skipping."),
                        *Params.BaseName, (int32)R.Info.Format);
                    UE_LOG(LogHL2BSPImporter, Warning, TEXT("%s"), *OutError);
                    return nullptr;
                }
                OutResult = EConvertResult::DecodeFailed;
                OutError = FString::Printf(TEXT("Skybox '%s' face '%s' decode failed: %s"),
                    *Params.BaseName, F.Suffix, *DecodeErr);
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("%s"), *OutError);
                return nullptr;
            }

            MaxW = FMath::Max(MaxW, R.Info.Width);
            MaxH = FMath::Max(MaxH, R.Info.Height);
        }

        if (MissingCount > 0)
        {
            OutResult = EConvertResult::MissingFaces;
            OutError = FString::Printf(TEXT("Skybox '%s': %d/6 face files missing"), *Params.BaseName, MissingCount);
            return nullptr;
        }

        // Cubemap faces must be square and same size. Pick the larger of the
        // four side faces (rt/lf/bk/ft) as the canonical edge length; up/dn
        // sometimes ship at a smaller resolution.
        const int32 CubeEdge = FMath::Max(MaxW, MaxH);
        if (CubeEdge <= 0)
        {
            OutResult = EConvertResult::DecodeFailed;
            OutError = TEXT("Skybox: zero-sized faces");
            return nullptr;
        }

        // Per-face: pad/upscale to (CubeEdge, CubeEdge), then rotate per AlleyKat table.
        const int64 SliceBytes = static_cast<int64>(CubeEdge) * CubeEdge * 4;
        TArray<uint8> CubeData;
        CubeData.SetNumUninitialized(SliceBytes * 6);

        for (const FFaceSlot& F : kFaces)
        {
            FResolvedFace& R = ResolvedFaces[F.SliceIndex];

            TArray<uint8> Square; // (CubeEdge, CubeEdge)
            if (R.Info.Width == CubeEdge && R.Info.Height == CubeEdge)
            {
                Square = MoveTemp(R.Bgra);
            }
            else if (R.Info.Width == CubeEdge && R.Info.Height < CubeEdge && R.Info.Height * 2 == CubeEdge)
            {
                // Cropped half-height side face — extrude the bottom row.
                ExtrudeBottomRow(R.Bgra.GetData(), R.Info.Width, R.Info.Height, Square, CubeEdge);
            }
            else
            {
                // Different aspect or much smaller (typical for `dn`): bilinear upscale to square.
                UpscaleBilinear(R.Bgra.GetData(), R.Info.Width, R.Info.Height, Square, CubeEdge, CubeEdge);
            }

            TArray<uint8> Rotated;
            if (F.Rotation == 0)
            {
                Rotated = MoveTemp(Square);
            }
            else
            {
                RotateBgra(Square.GetData(), CubeEdge, CubeEdge, F.Rotation, Rotated);
            }

            FMemory::Memcpy(CubeData.GetData() + F.SliceIndex * SliceBytes, Rotated.GetData(), SliceBytes);
        }

        // Build the asset.
        UPackage* Pkg = CreatePackage(*PkgFullName);
        if (!Pkg)
        {
            OutResult = EConvertResult::SaveFailed;
            OutError = FString::Printf(TEXT("CreatePackage failed: %s"), *PkgFullName);
            return nullptr;
        }
        Pkg->FullyLoad();

        UTextureCube* Cube = NewObject<UTextureCube>(Pkg, FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Cube)
        {
            OutResult = EConvertResult::SaveFailed;
            OutError = TEXT("NewObject<UTextureCube> failed");
            return nullptr;
        }

#if WITH_EDITORONLY_DATA
        Cube->Source.Init(CubeEdge, CubeEdge, /*NumSlices=*/6, /*NumMips=*/1,
                          TSF_BGRA8, CubeData.GetData());
#endif
        Cube->SRGB = true;
        Cube->LODGroup = TEXTUREGROUP_Skybox;
        Cube->UpdateResource();
        Cube->PostEditChange();

        FAssetRegistryModule::AssetCreated(Cube);
        Pkg->MarkPackageDirty();

        OutResult = EConvertResult::Created;
        return Cube;
    }
}

namespace HL2Sky
{
    // Helper visible to factory code: strip the face suffix from a sky face
    // texture name captured by FBspFile, returning the base stem suitable for
    // ConvertSkybox::FConvertParams::BaseName.
    HL2BSPIMPORTER_API FString DeriveBaseName(const FString& SkyFaceTextureName)
    {
        // Texture names captured by BspFile look like "skybox/sky_day01_05bk".
        // Drop any leading directory segment and strip the trailing 2-char suffix.
        FString Local = SkyFaceTextureName;
        Local.ReplaceInline(TEXT("\\"), TEXT("/"));
        int32 Slash = INDEX_NONE;
        if (Local.FindLastChar(TEXT('/'), Slash))
        {
            Local = Local.RightChop(Slash + 1);
        }
        return StripFaceSuffix(Local);
    }
}
