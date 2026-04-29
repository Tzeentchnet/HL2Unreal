#include "HL2SurfacePropBuilder.h"
#include "HL2SurfaceProp.h"
#include "HL2KeyValues.h"
#include "HL2BSPImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace HL2SP
{
    namespace
    {
        FString MakeAssetName(const FString& SurfaceKey)
        {
            // KV1 keys are already lower-cased by HL2KV. Sanitise to a UE asset name.
            FString Out;
            Out.Reserve(SurfaceKey.Len());
            for (TCHAR Ch : SurfaceKey)
            {
                if ((Ch >= TEXT('a') && Ch <= TEXT('z')) ||
                    (Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
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
            return Out.IsEmpty() ? FString(TEXT("Surface")) : Out;
        }

        UPackage* CreateOrLoadPackage(const FString& FullPkgName)
        {
            UPackage* Pkg = CreatePackage(*FullPkgName);
            if (Pkg) { Pkg->FullyLoad(); }
            return Pkg;
        }

        // Apply leaf values from a single KV1 surface block onto a USurfaceProp.
        // Source surfaceproperties.txt key set (per Valve SDK):
        //   base, friction, elasticity, density, dampening,
        //   audioReflectivity, audioHardnessFactor, audioRoughnessFactor,
        //   thickness, jumpFactor,
        //   stepleft, stepright, impactSoft, impactHard,
        //   scrapeSmooth, scrapeRough, bulletImpact, scrapeRough,
        //   strain, break, roll, snd_*  (a long tail; we lift the high-impact ones)
        void ApplyBlock(USurfaceProp* Out, const HL2KV::FNode& Block)
        {
            // Friction lives on the parent UPhysicalMaterial slot.
            const FString* FrictionStr = Block.FindString(TEXT("friction"));
            if (FrictionStr) { Out->Friction = FCString::Atof(**FrictionStr); }

            const FString* ElasticityStr = Block.FindString(TEXT("elasticity"));
            if (ElasticityStr) { Out->Elasticity = FCString::Atof(**ElasticityStr); }

            const FString* DensityStr = Block.FindString(TEXT("density"));
            if (DensityStr)   { Out->SourceDensity = FCString::Atof(**DensityStr); }

            const FString* DampStr = Block.FindString(TEXT("dampening"));
            if (DampStr)      { Out->Dampening = FCString::Atof(**DampStr); }

            const FString* AudioRefl = Block.FindString(TEXT("audioreflectivity"));
            if (AudioRefl)    { Out->AudioReflectivity = FCString::Atof(**AudioRefl); }

            const FString* AudioHard = Block.FindString(TEXT("audiohardnessfactor"));
            if (AudioHard)    { Out->AudioHardnessFactor = FCString::Atof(**AudioHard); }

            const FString* AudioRough = Block.FindString(TEXT("audioroughnessfactor"));
            if (AudioRough)   { Out->AudioRoughnessFactor = FCString::Atof(**AudioRough); }

            if (const FString* S = Block.FindString(TEXT("impactsoft")))   { Out->ImpactSoftSound   = *S; }
            if (const FString* S = Block.FindString(TEXT("impacthard")))   { Out->ImpactHardSound   = *S; }
            if (const FString* S = Block.FindString(TEXT("scrapesmooth"))) { Out->ScrapeSmoothSound = *S; }
            if (const FString* S = Block.FindString(TEXT("scraperough")))  { Out->ScrapeRoughSound  = *S; }
            if (const FString* S = Block.FindString(TEXT("stepleft")))     { Out->StepLeftSound     = *S; }
            if (const FString* S = Block.FindString(TEXT("stepright")))    { Out->StepRightSound    = *S; }

            if (const FString* B = Block.FindString(TEXT("base")))
            {
                FString BaseLower = *B; BaseLower.TrimStartAndEndInline(); BaseLower.ToLowerInline();
                Out->SourceParent = FName(*BaseLower);
            }
        }
    } // namespace

    FBuildResult BuildAllFromScript(const FString& AbsoluteScriptPath, const FString& DestPackagePath, FString& OutError)
    {
        FBuildResult R;
        OutError.Reset();

        HL2KV::FNode Root;
        FString ParseErr;
        if (!HL2KV::ParseFile(AbsoluteScriptPath, Root, ParseErr))
        {
            OutError = ParseErr;
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("HL2SP: parse failed for '%s': %s"), *AbsoluteScriptPath, *ParseErr);
            return R;
        }

        // First pass: create/refresh assets per top-level block.
        TMap<FString, USurfaceProp*> Created;
        Created.Reserve(Root.Children.Num());

        for (const TSharedPtr<HL2KV::FNode>& Child : Root.Children)
        {
            if (!Child.IsValid() || Child->IsLeaf()) { continue; }

            const FString& Key = Child->Key; // already lower-cased by HL2KV
            const FString  AssetName = MakeAssetName(Key);
            const FString  PkgFullName = DestPackagePath / AssetName;

            // Cached?
            FSoftObjectPath SOP(PkgFullName + TEXT(".") + AssetName);
            USurfaceProp* Sp = Cast<USurfaceProp>(SOP.TryLoad());
            if (Sp)
            {
                ++R.NumCached;
                Created.Add(Key, Sp);
                continue;
            }

            UPackage* Pkg = CreateOrLoadPackage(PkgFullName);
            if (!Pkg)
            {
                ++R.NumFailed;
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("HL2SP: CreatePackage failed: %s"), *PkgFullName);
                continue;
            }

            Sp = NewObject<USurfaceProp>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
            if (!Sp)
            {
                ++R.NumFailed;
                continue;
            }
            Sp->SourceName = FName(*Key);
            ApplyBlock(Sp, *Child);
            Sp->PostEditChange();

            FAssetRegistryModule::AssetCreated(Sp);
            Pkg->MarkPackageDirty();

            Created.Add(Key, Sp);
            ++R.NumCreated;
        }

        // Second pass: log unresolved base parents (the soft FName already
        // points at the sibling asset's stem; consumers can construct the
        // FSoftObjectPath via DestPackagePath/SourceParent at runtime).
        int32 OrphanParents = 0;
        for (const auto& Pair : Created)
        {
            const USurfaceProp* Sp = Pair.Value;
            if (!Sp || Sp->SourceParent.IsNone()) { continue; }
            const FString ParentKey = Sp->SourceParent.ToString().ToLower();
            if (!Created.Contains(ParentKey))
            {
                ++OrphanParents;
                UE_LOG(LogHL2BSPImporter, Verbose,
                    TEXT("HL2SP: surface '%s' references missing parent '%s'."),
                    *Pair.Key, *ParentKey);
            }
        }

        UE_LOG(LogHL2BSPImporter, Log,
            TEXT("HL2SP: surfaceproperties imported from '%s': created=%d cached=%d failed=%d orphan-parents=%d"),
            *AbsoluteScriptPath, R.NumCreated, R.NumCached, R.NumFailed, OrphanParents);

        return R;
    }
}
