// Standalone Content-Browser factories for .vtf / .vmt source assets.
//
// These factories let users import individual Source assets without going
// through the .bsp pipeline, and underpin the Tranche B bulk-import toolbar.
// Both honour the user's chosen Content Browser destination (InParent) for
// the directly-imported asset; texture references inside a .vmt still route
// through HL2Mat::FBuilder so they land at the canonical
// `<SynthesizedAssetRoot>/Textures/...` location and are shared with any
// future BSP-driven import.

#include "HL2AssetFactories.h"
#include "HL2BSPImporter.h"
#include "HL2BSPImporterSettings.h"
#include "HL2MaterialBuilder.h"
#include "HL2StaticPropMeshBuilder.h"
#include "HL2StudioLoader.h"
#include "HL2StudioTypes.h"
#include "HL2VmtParser.h"
#include "HL2VtfReader.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Subsystems/ImportSubsystem.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "HL2AssetFactories"

namespace
{
    // Walk a normalised absolute path looking for a `/materials/` segment
    // (case-insensitive). On hit, returns the portion AFTER `materials/`
    // (sans extension, lower-cased, forward slashes) as `OutKey` and the
    // portion BEFORE `materials/` as `OutContentRoot`. Returns false if no
    // such segment exists.
    //
    // Examples:
    //   C:/hl2/materials/concrete/concretefloor028a.vtf
    //     -> OutKey="concrete/concretefloor028a", OutContentRoot="C:/hl2"
    //   D:/MyMod/materials/decals/blood01.vmt
    //     -> OutKey="decals/blood01",            OutContentRoot="D:/MyMod"
    bool DeriveSourceKeyAndRoot(const FString& AbsFilenameIn, FString& OutKey, FString& OutContentRoot)
    {
        FString Norm = AbsFilenameIn;
        Norm.ReplaceInline(TEXT("\\"), TEXT("/"));
        const FString NormLower = Norm.ToLower();

        const TCHAR* Needle = TEXT("/materials/");
        const int32 NeedleLen = 11; // "/materials/"
        const int32 Pos = NormLower.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (Pos == INDEX_NONE) { return false; }

        OutContentRoot = Norm.Left(Pos); // no trailing slash
        FString After = Norm.RightChop(Pos + NeedleLen); // e.g. "concrete/Foo.Vtf"
        // Drop the extension.
        const int32 Dot = After.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (Dot != INDEX_NONE)
        {
            After.LeftInline(Dot, EAllowShrinking::No);
        }
        OutKey = After.ToLower();
        return !OutKey.IsEmpty();
    }

    // Same shape as DeriveSourceKeyAndRoot but anchors on a `/models/` segment.
    // Used by the standalone .mdl factory and bulk-import-models toolbar handler.
    //
    //   C:/hl2/models/props_c17/oildrum001.mdl
    //     -> OutKey="props_c17/oildrum001", OutContentRoot="C:/hl2"
    bool DeriveModelKeyAndRoot(const FString& AbsFilenameIn, FString& OutKey, FString& OutContentRoot)
    {
        FString Norm = AbsFilenameIn;
        Norm.ReplaceInline(TEXT("\\"), TEXT("/"));
        const FString NormLower = Norm.ToLower();
        const TCHAR* Needle = TEXT("/models/");
        const int32 NeedleLen = 8;
        const int32 Pos = NormLower.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (Pos == INDEX_NONE) { return false; }

        OutContentRoot = Norm.Left(Pos); // no trailing slash
        FString After = Norm.RightChop(Pos + NeedleLen); // e.g. "props_c17/oildrum001.mdl"
        const int32 Dot = After.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (Dot != INDEX_NONE)
        {
            After.LeftInline(Dot, EAllowShrinking::No);
        }
        OutKey = After.ToLower();
        return !OutKey.IsEmpty();
    }

    // Build a Source-key style asset name from a raw filename (drop ext, lower-case,
    // sanitise). Used when no `materials/` segment is present.
    FString FallbackAssetName(const FString& AbsFilename)
    {
        FString Stem = FPaths::GetBaseFilename(AbsFilename);
        Stem.ToLowerInline();
        // Asset names must be a valid object name; replace anything outside [a-z0-9_-].
        FString Out; Out.Reserve(Stem.Len());
        for (TCHAR Ch : Stem)
        {
            if ((Ch >= TEXT('a') && Ch <= TEXT('z')) ||
                (Ch >= TEXT('0') && Ch <= TEXT('9')) ||
                Ch == TEXT('_') || Ch == TEXT('-'))
            {
                Out.AppendChar(Ch);
            }
            else
            {
                Out.AppendChar(TEXT('_'));
            }
        }
        return Out.IsEmpty() ? FString(TEXT("Asset")) : Out;
    }

    // Capture the user-tweaked import settings on an existing UTexture so they
    // survive a re-import. Only the fields the user typically adjusts in the
    // Texture Editor are preserved; engine-generated state (mips, build data,
    // platform data) is rebuilt from the source bytes.
    struct FPreservedTextureSettings
    {
        TextureCompressionSettings  CompressionSettings = TC_Default;
        TextureGroup                LODGroup            = TEXTUREGROUP_World;
        TextureFilter               Filter              = TF_Default;
        TextureAddress              AddressX            = TA_Wrap;
        TextureAddress              AddressY            = TA_Wrap;
        TextureMipGenSettings       MipGenSettings      = TMGS_FromTextureGroup;
        bool                        bSRGB               = true;
        bool                        bFlipGreenChannel   = false;
        float                       AdjustBrightness    = 1.f;
        float                       AdjustBrightnessCurve = 1.f;
        float                       AdjustVibrance      = 0.f;
        float                       AdjustSaturation    = 1.f;
        float                       AdjustRGBCurve      = 1.f;
        float                       AdjustHue           = 0.f;
        float                       AdjustMinAlpha      = 0.f;
        float                       AdjustMaxAlpha      = 1.f;
        bool                        bValid              = false;
    };

    FPreservedTextureSettings CapturePreservedSettings(UTexture* Existing)
    {
        FPreservedTextureSettings Out;
        if (!Existing) { return Out; }
        Out.CompressionSettings   = Existing->CompressionSettings;
        Out.LODGroup              = Existing->LODGroup;
        Out.Filter                = Existing->Filter;
#if WITH_EDITORONLY_DATA
        Out.MipGenSettings        = Existing->MipGenSettings;
        Out.AdjustBrightness      = Existing->AdjustBrightness;
        Out.AdjustBrightnessCurve = Existing->AdjustBrightnessCurve;
        Out.AdjustVibrance        = Existing->AdjustVibrance;
        Out.AdjustSaturation      = Existing->AdjustSaturation;
        Out.AdjustRGBCurve        = Existing->AdjustRGBCurve;
        Out.AdjustHue             = Existing->AdjustHue;
        Out.AdjustMinAlpha        = Existing->AdjustMinAlpha;
        Out.AdjustMaxAlpha        = Existing->AdjustMaxAlpha;
        Out.bFlipGreenChannel     = Existing->bFlipGreenChannel != 0;
#endif
        Out.bSRGB                 = Existing->SRGB != 0;
        if (UTexture2D* T2 = Cast<UTexture2D>(Existing))
        {
            Out.AddressX = T2->AddressX;
            Out.AddressY = T2->AddressY;
        }
        Out.bValid = true;
        return Out;
    }

    void ApplyPreservedSettings(UTexture* Tex, const FPreservedTextureSettings& S)
    {
        if (!Tex || !S.bValid) { return; }
        Tex->CompressionSettings = S.CompressionSettings;
        Tex->LODGroup            = S.LODGroup;
        Tex->Filter              = S.Filter;
        Tex->SRGB                = S.bSRGB ? 1 : 0;
#if WITH_EDITORONLY_DATA
        Tex->MipGenSettings        = S.MipGenSettings;
        Tex->AdjustBrightness      = S.AdjustBrightness;
        Tex->AdjustBrightnessCurve = S.AdjustBrightnessCurve;
        Tex->AdjustVibrance        = S.AdjustVibrance;
        Tex->AdjustSaturation      = S.AdjustSaturation;
        Tex->AdjustRGBCurve        = S.AdjustRGBCurve;
        Tex->AdjustHue             = S.AdjustHue;
        Tex->AdjustMinAlpha        = S.AdjustMinAlpha;
        Tex->AdjustMaxAlpha        = S.AdjustMaxAlpha;
        Tex->bFlipGreenChannel     = S.bFlipGreenChannel ? 1 : 0;
#endif
        if (UTexture2D* T2 = Cast<UTexture2D>(Tex))
        {
            T2->AddressX = S.AddressX;
            T2->AddressY = S.AddressY;
        }
    }

    void StampReimportData(UObject* Asset, const FString& AbsFilename)
    {
#if WITH_EDITORONLY_DATA
        if (UTexture* Tex = Cast<UTexture>(Asset))
        {
            if (!Tex->AssetImportData) { Tex->AssetImportData = NewObject<UAssetImportData>(Tex); }
            Tex->AssetImportData->Update(AbsFilename);
            return;
        }
        if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Asset))
        {
            if (!MIC->AssetImportData) { MIC->AssetImportData = NewObject<UAssetImportData>(MIC); }
            MIC->AssetImportData->Update(AbsFilename);
            return;
        }
        if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
        {
            if (!SM->AssetImportData) { SM->AssetImportData = NewObject<UAssetImportData>(SM); }
            SM->AssetImportData->Update(AbsFilename);
        }
#endif
    }

    // Pull the absolute source filename for a previously-imported asset back
    // out of its UAssetImportData.
    bool ReadSourceFilename(UObject* Asset, FString& OutFilename)
    {
#if WITH_EDITORONLY_DATA
        if (UTexture* Tex = Cast<UTexture>(Asset))
        {
            if (!Tex->AssetImportData) { return false; }
            OutFilename = Tex->AssetImportData->GetFirstFilename();
            return !OutFilename.IsEmpty();
        }
        if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Asset))
        {
            if (!MIC->AssetImportData) { return false; }
            OutFilename = MIC->AssetImportData->GetFirstFilename();
            return !OutFilename.IsEmpty();
        }
        if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
        {
            if (!SM->AssetImportData) { return false; }
            OutFilename = SM->AssetImportData->GetFirstFilename();
            return !OutFilename.IsEmpty();
        }
#endif
        return false;
    }
} // namespace

// =====================================================================
// UHL2VTFFactory
// =====================================================================

UHL2VTFFactory::UHL2VTFFactory()
{
    bCreateNew    = false;
    bEditorImport = true;
    bText         = false;
    SupportedClass = UTexture::StaticClass();
    Formats.Add(TEXT("vtf;Half-Life 2 / Source Texture"));
}

bool UHL2VTFFactory::FactoryCanImport(const FString& Filename)
{
    return FPaths::GetExtension(Filename).Equals(TEXT("vtf"), ESearchCase::IgnoreCase);
}

int32 UHL2VTFFactory::GetPriority() const
{
    return DefaultImportPriority + 1;
}

UObject* UHL2VTFFactory::FactoryCreateBinary(
    UClass* InClass, UObject* InParent, FName InName,
    EObjectFlags Flags, UObject* /*Context*/, const TCHAR* /*Type*/,
    const uint8*& Buffer, const uint8* BufferEnd,
    FFeedbackContext* Warn)
{
    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPreImport(this, InClass, InParent, InName, TEXT("vtf"));

    if (!Buffer || BufferEnd <= Buffer)
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VTF factory: empty buffer for '%s'"), *InName.ToString());
        return nullptr;
    }

    TArray<uint8> Bytes;
    const int64 Len = BufferEnd - Buffer;
    Bytes.Append(Buffer, Len);
    // Mark the entire buffer consumed (UE convention for binary factories).
    Buffer = BufferEnd;

    HL2VTF::FInfo Info;
    FString Err;
    if (!HL2VTF::ReadHeader(Bytes, Info, Err))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VTF header parse failed for '%s': %s"), *InName.ToString(), *Err);
        return nullptr;
    }

    const FString AbsFilename = GetCurrentFilename();
    const bool bIsNormalMap = (Info.Flags & HL2VTF::TEXTUREFLAGS_NORMAL) != 0;
    const bool bIsCubemap   = Info.NumFaces >= 6;

    // Re-import: if an asset already lives at InParent/InName, capture the user's
    // tweaked settings and reuse the same UObject (NewObject with a colliding name
    // would assert/fail). Type mismatch (e.g. existing UTexture2D being replaced
    // with a UTextureCube) is handled by NewObject overwriting the slot.
    UObject* ExistingObj = StaticFindObject(UObject::StaticClass(), InParent, *InName.ToString());
    UTexture* ExistingTex = Cast<UTexture>(ExistingObj);
    FPreservedTextureSettings Preserved = CapturePreservedSettings(ExistingTex);
    const bool bExistingTypeMatches = bIsCubemap
        ? (Cast<UTextureCube>(ExistingObj) != nullptr)
        : (Cast<UTexture2D>(ExistingObj) != nullptr);
    if (ExistingTex && bExistingTypeMatches)
    {
        ExistingTex->ReleaseResource();
    }
    else if (ExistingObj)
    {
        // Different type — rename the stale object out of the way so NewObject can claim the name.
        ExistingObj->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
        ExistingObj = nullptr;
        ExistingTex = nullptr;
    }

    UTexture* Result = nullptr;

    if (bIsCubemap)
    {
        // Decode all six faces into a single contiguous buffer for Source.Init.
        // VTF stores face order +X, -X, +Y, -Y, +Z, -Z (matching DirectX cubemap
        // convention used by UE). We do not rotate per-face — VTF envmaps are
        // already authored in cube-sample-space.
        const int64 SliceBytes = static_cast<int64>(Info.Width) * Info.Height * 4;
        TArray<uint8> AllFaces;
        AllFaces.SetNumUninitialized(SliceBytes * 6);
        for (int32 Face = 0; Face < 6; ++Face)
        {
            TArray<uint8> FaceBgra;
            if (!HL2VTF::DecodeBGRAFace(Bytes, Info, Face, FaceBgra, Err))
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("VTF cube face %d decode failed for '%s': %s"),
                    Face, *InName.ToString(), *Err);
                return nullptr;
            }
            if (FaceBgra.Num() != SliceBytes)
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("VTF cube face %d size mismatch (%d vs %lld) for '%s'"),
                    Face, FaceBgra.Num(), SliceBytes, *InName.ToString());
                return nullptr;
            }
            FMemory::Memcpy(AllFaces.GetData() + Face * SliceBytes, FaceBgra.GetData(), SliceBytes);
        }

        UTextureCube* Cube = Cast<UTextureCube>(ExistingTex);
        if (!Cube)
        {
            Cube = NewObject<UTextureCube>(InParent, InName, Flags);
        }
        if (!Cube) { return nullptr; }
#if WITH_EDITORONLY_DATA
        Cube->Source.Init(Info.Width, Info.Height, /*NumSlices=*/6, /*NumMips=*/1,
                          TSF_BGRA8, AllFaces.GetData());
#endif
        if (!Preserved.bValid)
        {
            Cube->SRGB = !bIsNormalMap;
            Cube->LODGroup = TEXTUREGROUP_World;
        }
        ApplyPreservedSettings(Cube, Preserved);
        Cube->UpdateResource();
        Cube->PostEditChange();
        Result = Cube;
    }
    else
    {
        TArray<uint8> BGRA;
        if (!HL2VTF::DecodeBGRA(Bytes, Info, BGRA, Err))
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("VTF decode failed for '%s': %s"), *InName.ToString(), *Err);
            return nullptr;
        }

        UTexture2D* Tex = Cast<UTexture2D>(ExistingTex);
        if (!Tex)
        {
            Tex = NewObject<UTexture2D>(InParent, InName, Flags);
        }
        if (!Tex) { return nullptr; }
#if WITH_EDITORONLY_DATA
        Tex->Source.Init(Info.Width, Info.Height, /*NumSlices=*/1, /*NumMips=*/1,
                         TSF_BGRA8, BGRA.GetData());
#endif
        if (!Preserved.bValid)
        {
            Tex->SRGB = !bIsNormalMap;
            if (bIsNormalMap)
            {
                Tex->CompressionSettings = TC_Normalmap;
                Tex->LODGroup            = TEXTUREGROUP_WorldNormalMap;
            }
            else
            {
                Tex->LODGroup            = TEXTUREGROUP_World;
            }
        }
        ApplyPreservedSettings(Tex, Preserved);
        Tex->UpdateResource();
        Tex->PostEditChange();
        Result = Tex;

        // Normal-map alpha sibling. Source frequently packs phong / envmap /
        // specular masks into the alpha channel of `$bumpmap` VTFs, but UE
        // normal-map textures (TC_Normalmap is BC5 RG) drop alpha. When the
        // source VTF carries non-uniform alpha we emit a sibling `<name>_a`
        // UTexture2D in a sibling package next to the parent, configured as a
        // grayscale mask (sRGB off, TC_Grayscale). Master materials wire this
        // into the phong/envmap mask path via the `NormalAlpha` parameter.
        if (bIsNormalMap && InParent && HL2VTF::HasMeaningfulAlpha(BGRA))
        {
            TArray<uint8> AlphaBGRA;
            if (HL2VTF::ExtractAlphaToBGRA(BGRA, AlphaBGRA))
            {
                const FString ParentPkgPath = InParent->GetOutermost()->GetName();
                FString PkgDir; FString PkgLeaf;
                if (!ParentPkgPath.Split(TEXT("/"), &PkgDir, &PkgLeaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
                {
                    PkgDir = ParentPkgPath;
                    PkgLeaf = InName.ToString();
                }
                const FString AlphaName    = InName.ToString() + TEXT("_a");
                const FString AlphaPkgPath = PkgDir / AlphaName;

                // Re-import: reuse an existing sibling at the same path if present.
                UPackage* AlphaPkg = CreatePackage(*AlphaPkgPath);
                if (AlphaPkg)
                {
                    AlphaPkg->FullyLoad();
                    UObject* ExistingAlphaObj = StaticFindObject(UObject::StaticClass(), AlphaPkg, *AlphaName);
                    UTexture2D* AlphaTex = Cast<UTexture2D>(ExistingAlphaObj);
                    if (!AlphaTex && ExistingAlphaObj)
                    {
                        ExistingAlphaObj->Rename(nullptr, GetTransientPackage(),
                            REN_DontCreateRedirectors | REN_NonTransactional);
                    }
                    if (!AlphaTex)
                    {
                        AlphaTex = NewObject<UTexture2D>(AlphaPkg, FName(*AlphaName), Flags);
                    }
                    else
                    {
                        AlphaTex->ReleaseResource();
                    }
                    if (AlphaTex)
                    {
#if WITH_EDITORONLY_DATA
                        AlphaTex->Source.Init(Info.Width, Info.Height, /*NumSlices=*/1, /*NumMips=*/1,
                                              TSF_BGRA8, AlphaBGRA.GetData());
#endif
                        AlphaTex->SRGB                = false;
                        AlphaTex->CompressionSettings = TC_Grayscale;
                        AlphaTex->LODGroup            = TEXTUREGROUP_World;
                        AlphaTex->UpdateResource();
                        AlphaTex->PostEditChange();
                        StampReimportData(AlphaTex, AbsFilename);
                        FAssetRegistryModule::AssetCreated(AlphaTex);
                        AlphaPkg->MarkPackageDirty();
                    }
                }
            }
        }
    }

    if (Result)
    {
        StampReimportData(Result, AbsFilename);
        FAssetRegistryModule::AssetCreated(Result);
        if (UPackage* Pkg = Result->GetOutermost()) { Pkg->MarkPackageDirty(); }
    }

    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPostImport(this, Result);
    return Result;
}

bool UHL2VTFFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
    if (!Cast<UTexture2D>(Obj) && !Cast<UTextureCube>(Obj)) { return false; }
    FString Filename;
    if (!ReadSourceFilename(Obj, Filename)) { return false; }
    if (!FPaths::GetExtension(Filename).Equals(TEXT("vtf"), ESearchCase::IgnoreCase)) { return false; }
    OutFilenames.Add(Filename);
    return true;
}

void UHL2VTFFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
#if WITH_EDITORONLY_DATA
    if (NewReimportPaths.Num() != 1) { return; }
    if (UTexture* Tex = Cast<UTexture>(Obj))
    {
        if (!Tex->AssetImportData) { Tex->AssetImportData = NewObject<UAssetImportData>(Tex); }
        Tex->AssetImportData->Update(NewReimportPaths[0]);
    }
#endif
}

EReimportResult::Type UHL2VTFFactory::Reimport(UObject* Obj)
{
    FString Filename;
    if (!ReadSourceFilename(Obj, Filename) || !FPaths::FileExists(Filename))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VTF reimport: source file missing for '%s'"), *Obj->GetName());
        return EReimportResult::Failed;
    }

    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VTF reimport: failed to read '%s'"), *Filename);
        return EReimportResult::Failed;
    }

    UPackage* Outer = Obj->GetOutermost();
    const FName ObjName = Obj->GetFName();
    bool bCancelled = false;
    SetCurrentFilename(Filename);
    const uint8* Buffer = Bytes.GetData();
    const uint8* BufferEnd = Buffer + Bytes.Num();
    UObject* Result = FactoryCreateBinary(Obj->GetClass(), Outer, ObjName,
        Obj->GetFlags(), nullptr, *FPaths::GetExtension(Filename), Buffer, BufferEnd, GWarn);
    SetCurrentFilename(FString());
    return Result ? EReimportResult::Succeeded : EReimportResult::Failed;
}

// =====================================================================
// UHL2VMTFactory
// =====================================================================

UHL2VMTFactory::UHL2VMTFactory()
{
    bCreateNew    = false;
    bEditorImport = true;
    bText         = true;
    SupportedClass = UMaterialInstanceConstant::StaticClass();
    Formats.Add(TEXT("vmt;Half-Life 2 / Source Material"));
}

bool UHL2VMTFactory::FactoryCanImport(const FString& Filename)
{
    return FPaths::GetExtension(Filename).Equals(TEXT("vmt"), ESearchCase::IgnoreCase);
}

int32 UHL2VMTFactory::GetPriority() const
{
    return DefaultImportPriority + 1;
}

UObject* UHL2VMTFactory::FactoryCreateText(
    UClass* InClass, UObject* InParent, FName InName,
    EObjectFlags Flags, UObject* /*Context*/, const TCHAR* /*Type*/,
    const TCHAR*& Buffer, const TCHAR* BufferEnd,
    FFeedbackContext* /*Warn*/)
{
    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPreImport(this, InClass, InParent, InName, TEXT("vmt"));

    if (!Buffer || BufferEnd <= Buffer)
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT factory: empty buffer for '%s'"), *InName.ToString());
        return nullptr;
    }

    const FString Source(static_cast<int32>(BufferEnd - Buffer), Buffer);
    Buffer = BufferEnd;

    HL2VMT::FDocument Doc;
    FString Err;
    if (!HL2VMT::Parse(Source, Doc, Err))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT parse failed for '%s': %s"), *InName.ToString(), *Err);
        return nullptr;
    }

    const FString AbsFilename = GetCurrentFilename();
    const UHL2BSPImporterSettings* Settings = GetDefault<UHL2BSPImporterSettings>();
    HL2Mat::FBuilder Builder(Settings);

    // Auto-derive a content root from the .vmt's path (the directory above its
    // `materials/` segment) and inject it into the builder so $basetexture refs
    // and Patch include resolution work even when SourceContentRoots is empty.
    FString DerivedKey, DerivedRoot;
    if (DeriveSourceKeyAndRoot(AbsFilename, DerivedKey, DerivedRoot))
    {
        Builder.AddExtraRoot(DerivedRoot);
    }

    // Resolve Patch indirection. Use the same root list the builder was seeded
    // with (configured + derived) so includes resolve consistently.
    TArray<FString> PatchRoots;
    if (!DerivedRoot.IsEmpty()) { PatchRoots.Add(DerivedRoot); }
    if (Settings)
    {
        for (const FString& R : Settings->SourceContentRoots)
        {
            if (!R.IsEmpty()) { PatchRoots.AddUnique(FPaths::ConvertRelativePathToFull(R)); }
        }
    }
    if (!HL2VMT::ResolvePatches(Doc, PatchRoots, Err))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT patch resolve failed for '%s': %s"), *InName.ToString(), *Err);
        return nullptr;
    }

    UMaterialInstanceConstant* MIC = Builder.BuildMICIntoPackage(Cast<UPackage>(InParent), InName, Doc, Err);
    if (!MIC)
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT MIC build failed for '%s': %s"), *InName.ToString(), *Err);
        return nullptr;
    }

    StampReimportData(MIC, AbsFilename);

    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("VMT '%s' imported as MIC; textures resolved=%d cached=%d failed=%d"),
        *AbsFilename, Builder.NumTexturesCreated, Builder.NumTexturesCached, Builder.NumTexturesFailed);

    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPostImport(this, MIC);
    return MIC;
}

bool UHL2VMTFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
    if (!Cast<UMaterialInstanceConstant>(Obj)) { return false; }
    FString Filename;
    if (!ReadSourceFilename(Obj, Filename)) { return false; }
    if (!FPaths::GetExtension(Filename).Equals(TEXT("vmt"), ESearchCase::IgnoreCase)) { return false; }
    OutFilenames.Add(Filename);
    return true;
}

void UHL2VMTFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
#if WITH_EDITORONLY_DATA
    if (NewReimportPaths.Num() != 1) { return; }
    if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Obj))
    {
        if (!MIC->AssetImportData) { MIC->AssetImportData = NewObject<UAssetImportData>(MIC); }
        MIC->AssetImportData->Update(NewReimportPaths[0]);
    }
#endif
}

EReimportResult::Type UHL2VMTFactory::Reimport(UObject* Obj)
{
    FString Filename;
    if (!ReadSourceFilename(Obj, Filename) || !FPaths::FileExists(Filename))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT reimport: source file missing for '%s'"), *Obj->GetName());
        return EReimportResult::Failed;
    }

    FString Source;
    if (!FFileHelper::LoadFileToString(Source, *Filename))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT reimport: failed to read '%s'"), *Filename);
        return EReimportResult::Failed;
    }

    UPackage* Outer = Obj->GetOutermost();
    const FName ObjName = Obj->GetFName();
    SetCurrentFilename(Filename);
    const TCHAR* Buffer    = *Source;
    const TCHAR* BufferEnd = Buffer + Source.Len();
    UObject* Result = FactoryCreateText(Obj->GetClass(), Outer, ObjName,
        Obj->GetFlags(), nullptr, TEXT("vmt"), Buffer, BufferEnd, GWarn);
    SetCurrentFilename(FString());
    return Result ? EReimportResult::Succeeded : EReimportResult::Failed;
}

// =====================================================================
// UHL2MDLFactory
// =====================================================================

UHL2MDLFactory::UHL2MDLFactory()
{
    bCreateNew    = false;
    bEditorImport = true;
    bText         = false;
    SupportedClass = UStaticMesh::StaticClass();
    Formats.Add(TEXT("mdl;Half-Life 2 / Source Model"));
}

bool UHL2MDLFactory::FactoryCanImport(const FString& Filename)
{
    return FPaths::GetExtension(Filename).Equals(TEXT("mdl"), ESearchCase::IgnoreCase);
}

int32 UHL2MDLFactory::GetPriority() const
{
    return DefaultImportPriority + 1;
}

UObject* UHL2MDLFactory::FactoryCreateBinary(
    UClass* InClass, UObject* InParent, FName InName,
    EObjectFlags Flags, UObject* /*Context*/, const TCHAR* /*Type*/,
    const uint8*& Buffer, const uint8* BufferEnd,
    FFeedbackContext* /*Warn*/)
{
    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPreImport(this, InClass, InParent, InName, TEXT("mdl"));

    // The .mdl byte buffer alone is not sufficient — HL2Studio::LoadModel
    // needs the .vvd + .dx90.vtx siblings on disk. Mark the buffer as consumed
    // (UE convention for binary factories) and pivot to the path-based loader.
    Buffer = BufferEnd;

    const FString AbsFilename = GetCurrentFilename();
    if (AbsFilename.IsEmpty() || !FPaths::FileExists(AbsFilename))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("MDL factory: source filename missing for '%s'"), *InName.ToString());
        return nullptr;
    }

    // Auto-derive a content root from the .mdl's path and seed the studio
    // loader's roots with it. This lets a single-file Content Browser import
    // succeed even when no SourceContentRoots are configured. Configured roots
    // are still searched as a fallback for sibling files (e.g. when the user
    // imports an extracted .mdl while the .vvd/.vtx live in a Steam content
    // tree).
    FString DerivedKey, DerivedRoot;
    if (!DeriveModelKeyAndRoot(AbsFilename, DerivedKey, DerivedRoot))
    {
        UE_LOG(LogHL2BSPImporter, Warning,
            TEXT("MDL factory: '%s' is not under a 'models/' folder; cannot derive a content root."),
            *AbsFilename);
        return nullptr;
    }
    const FString ModelKey = DerivedKey + TEXT(".mdl");

    const UHL2BSPImporterSettings* Settings = GetDefault<UHL2BSPImporterSettings>();
    TArray<FString> StudioRoots;
    StudioRoots.Add(FPaths::ConvertRelativePathToFull(DerivedRoot));
    if (Settings)
    {
        for (const FString& R : Settings->SourceContentRoots)
        {
            if (!R.IsEmpty()) { StudioRoots.AddUnique(FPaths::ConvertRelativePathToFull(R)); }
        }
    }

    HL2Studio::FStudioFile Studio;
    FString Err;
    if (!HL2Studio::LoadModel(ModelKey, StudioRoots, Studio, Err))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("MDL load failed for '%s': %s"), *AbsFilename, *Err);
        return nullptr;
    }

    HL2Mat::FBuilder MatBuilder(Settings);
    MatBuilder.AddExtraRoot(DerivedRoot);

    // Re-import: rename a stale UObject at the destination out of the way so
    // BuildStaticMesh's NewObject doesn't collide. Type mismatch is unlikely
    // for an MDL re-import (UStaticMesh -> UStaticMesh) but the rename is
    // cheap and matches the VTF/VMT factories' shape.
    UObject* ExistingObj = StaticFindObject(UObject::StaticClass(), InParent, *InName.ToString());
    if (ExistingObj && !ExistingObj->IsA<UStaticMesh>())
    {
        ExistingObj->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    }

    UStaticMesh* Mesh = HL2Studio::BuildStaticMesh(
        Studio, Settings, MatBuilder, InParent, InName, Flags, /*SkinIndex*/ 0, /*BodyMask*/ 0);
    if (!Mesh)
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("MDL build failed for '%s'"), *AbsFilename);
        return nullptr;
    }

    StampReimportData(Mesh, AbsFilename);
    if (UPackage* Pkg = Mesh->GetOutermost()) { Pkg->MarkPackageDirty(); }

    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("MDL '%s' imported as UStaticMesh '%s'; materials created=%d cached=%d failed=%d, textures created=%d cached=%d failed=%d"),
        *AbsFilename, *Mesh->GetPathName(),
        MatBuilder.NumMaterialsCreated, MatBuilder.NumMaterialsCached, MatBuilder.NumMaterialsFailed,
        MatBuilder.NumTexturesCreated,  MatBuilder.NumTexturesCached,  MatBuilder.NumTexturesFailed);

    GEditor->GetEditorSubsystem<UImportSubsystem>()->BroadcastAssetPostImport(this, Mesh);
    return Mesh;
}

bool UHL2MDLFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
    if (!Cast<UStaticMesh>(Obj)) { return false; }
    FString Filename;
    if (!ReadSourceFilename(Obj, Filename)) { return false; }
    if (!FPaths::GetExtension(Filename).Equals(TEXT("mdl"), ESearchCase::IgnoreCase)) { return false; }
    OutFilenames.Add(Filename);
    return true;
}

void UHL2MDLFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
#if WITH_EDITORONLY_DATA
    if (NewReimportPaths.Num() != 1) { return; }
    if (UStaticMesh* SM = Cast<UStaticMesh>(Obj))
    {
        if (!SM->AssetImportData) { SM->AssetImportData = NewObject<UAssetImportData>(SM); }
        SM->AssetImportData->Update(NewReimportPaths[0]);
    }
#endif
}

EReimportResult::Type UHL2MDLFactory::Reimport(UObject* Obj)
{
    FString Filename;
    if (!ReadSourceFilename(Obj, Filename) || !FPaths::FileExists(Filename))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("MDL reimport: source file missing for '%s'"), *Obj->GetName());
        return EReimportResult::Failed;
    }

    UPackage* Outer = Obj->GetOutermost();
    const FName ObjName = Obj->GetFName();
    SetCurrentFilename(Filename);
    // The buffer is unused by FactoryCreateBinary (we pivot to the path),
    // but we still need a non-null pair to satisfy the contract.
    const uint8 Sentinel = 0;
    const uint8* Buffer    = &Sentinel;
    const uint8* BufferEnd = Buffer;
    UObject* Result = FactoryCreateBinary(Obj->GetClass(), Outer, ObjName,
        Obj->GetFlags(), nullptr, TEXT("mdl"), Buffer, BufferEnd, GWarn);
    SetCurrentFilename(FString());
    return Result ? EReimportResult::Succeeded : EReimportResult::Failed;
}

#undef LOCTEXT_NAMESPACE
