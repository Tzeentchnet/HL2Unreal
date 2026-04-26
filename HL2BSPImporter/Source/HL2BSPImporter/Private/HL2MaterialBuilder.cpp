#include "HL2MaterialBuilder.h"
#include "HL2BSPImporter.h"
#include "HL2BSPImporterSettings.h"
#include "HL2VmtParser.h"
#include "HL2VtfReader.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace HL2Mat
{
    namespace
    {
        FString NormalizeKey(const FString& In)
        {
            FString Out = In.Replace(TEXT("\\"), TEXT("/"));
            Out.ToLowerInline();
            // Strip leading 'materials/' / 'textures/' if a VMT wrote one.
            if (Out.StartsWith(TEXT("materials/"))) { Out.RightChopInline(10, EAllowShrinking::No); }
            // VMTs sometimes include a leading '/'.
            while (Out.StartsWith(TEXT("/"))) { Out.RightChopInline(1, EAllowShrinking::No); }
            return Out;
        }

        // Replace characters that are illegal in long package paths / object names.
        FString SanitizePathSegment(const FString& In)
        {
            FString Out; Out.Reserve(In.Len());
            for (TCHAR Ch : In)
            {
                if ((Ch >= TEXT('a') && Ch <= TEXT('z')) ||
                    (Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
                    (Ch >= TEXT('0') && Ch <= TEXT('9')) ||
                    Ch == TEXT('_') || Ch == TEXT('-') || Ch == TEXT('/'))
                {
                    Out.AppendChar(Ch);
                }
                else
                {
                    Out.AppendChar(TEXT('_'));
                }
            }
            return Out;
        }

        // /Game/HL2/Imported + Materials/<key> -> ("/Game/HL2/Imported/Materials/concrete", "concretefloor028a")
        void SplitAssetPath(const FString& AssetRoot, const FString& Subfolder, const FString& Key,
                            FString& OutPackagePath, FString& OutAssetName)
        {
            const FString Sanitised = SanitizePathSegment(Key);
            FString Dir, Leaf;
            if (!Sanitised.Split(TEXT("/"), &Dir, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
            {
                Dir.Reset();
                Leaf = Sanitised;
            }
            OutPackagePath = AssetRoot / Subfolder / Dir;
            // CreatePackage doesn't like trailing slashes.
            while (OutPackagePath.EndsWith(TEXT("/"))) { OutPackagePath.LeftChopInline(1, EAllowShrinking::No); }
            OutAssetName   = Leaf.IsEmpty() ? TEXT("Material") : Leaf;
        }

        UPackage* CreateAssetPackage(const FString& PackagePath, const FString& AssetName, FString& OutFullName)
        {
            OutFullName = PackagePath / AssetName;
            UPackage* Pkg = CreatePackage(*OutFullName);
            if (Pkg) { Pkg->FullyLoad(); }
            return Pkg;
        }

        // Look up an existing asset by long-package + object name. Returns null if absent.
        template<class T>
        T* FindExistingAsset(const FString& PackagePath, const FString& AssetName)
        {
            const FString FullObject = PackagePath / AssetName + TEXT(".") + AssetName;
            FSoftObjectPath SOP(FullObject);
            return Cast<T>(SOP.TryLoad());
        }

        UMaterialInterface* LoadParentMaterial(const TCHAR* SlotName, const FSoftObjectPath& SOP, bool bWarnIfMissing)
        {
            if (!SOP.IsValid())
            {
                if (bWarnIfMissing)
                {
                    UE_LOG(LogHL2BSPImporter, Warning, TEXT("Parent material slot %s is not configured."), SlotName);
                }
                return nullptr;
            }
            UMaterialInterface* Parent = Cast<UMaterialInterface>(SOP.TryLoad());
            if (!Parent && bWarnIfMissing)
            {
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Parent material slot %s failed to load '%s'. Run Scripts/GenerateMasterMaterials.py or assign a valid override."),
                    SlotName, *SOP.ToString());
            }
            return Parent;
        }

        // ---- Phase 11b helpers -------------------------------------------------

        // Parse a Source `$color` / `$color2` / `$selfillumtint` value. Accepted forms:
        //   "[ 1.0 0.5 0.0 ]"  -- floats 0..1
        //   "{ 255 128 0 }"    -- ints 0..255
        //   "0.5"              -- single luminance (replicated to RGB)
        //   "1 1 1"            -- bare floats
        // Returns true and writes Out on success; false leaves Out untouched. Alpha is
        // always 1.0 for color tints; callers needing $alpha set it on a separate scalar.
        bool ParseSourceColor(const FString& InRaw, FLinearColor& Out)
        {
            FString S = InRaw;
            S.TrimStartAndEndInline();
            if (S.IsEmpty()) { return false; }

            const bool bIntForm = S.StartsWith(TEXT("{"));
            // Strip wrapping brackets/braces.
            S.ReplaceInline(TEXT("["), TEXT(" "));
            S.ReplaceInline(TEXT("]"), TEXT(" "));
            S.ReplaceInline(TEXT("{"), TEXT(" "));
            S.ReplaceInline(TEXT("}"), TEXT(" "));
            S.ReplaceInline(TEXT(","), TEXT(" "));

            TArray<FString> Parts; S.ParseIntoArrayWS(Parts);
            if (Parts.Num() == 0) { return false; }

            const float Scale = bIntForm ? (1.f / 255.f) : 1.f;
            float V[4] = { 1.f, 1.f, 1.f, 1.f };
            const int32 N = FMath::Min(Parts.Num(), 4);
            for (int32 i = 0; i < N; ++i)
            {
                V[i] = FCString::Atof(*Parts[i]) * Scale;
            }
            if (N == 1)
            {
                V[1] = V[2] = V[0];
            }
            Out = FLinearColor(V[0], V[1], V[2], (Parts.Num() >= 4) ? V[3] : 1.f);
            return true;
        }

        // Parse a Source `$basetexturetransform` / `$bumptransform` value. Format:
        //   center <cx> <cy> scale <sx> <sy> rotate <deg> translate <tx> <ty>
        // Tokens may appear in any order; missing tokens default to identity. The
        // `center` token is a per-pivot offset for the rotation; we fold it into the
        // translate output so the master only needs one offset parameter.
        struct FUVTransform
        {
            FVector2D Scale     = FVector2D(1.f, 1.f);
            FVector2D Translate = FVector2D::ZeroVector;
            float     Rotate    = 0.f;     // degrees
            bool      bIdentity = true;    // any non-default token sets this false
        };
        bool ParseUVTransform(const FString& InRaw, FUVTransform& Out)
        {
            FString S = InRaw;
            S.TrimStartAndEndInline();
            if (S.IsEmpty()) { return false; }
            TArray<FString> T; S.ParseIntoArrayWS(T);
            FVector2D Center = FVector2D::ZeroVector;
            for (int32 i = 0; i < T.Num(); )
            {
                const FString Tok = T[i].ToLower();
                auto Take = [&](float& Dst)
                {
                    if (T.IsValidIndex(i + 1)) { Dst = FCString::Atof(*T[i + 1]); }
                };
                if (Tok == TEXT("scale") && T.IsValidIndex(i + 2))
                {
                    Out.Scale.X = FCString::Atof(*T[i + 1]);
                    Out.Scale.Y = FCString::Atof(*T[i + 2]);
                    Out.bIdentity = false;
                    i += 3;
                }
                else if (Tok == TEXT("translate") && T.IsValidIndex(i + 2))
                {
                    Out.Translate.X = FCString::Atof(*T[i + 1]);
                    Out.Translate.Y = FCString::Atof(*T[i + 2]);
                    Out.bIdentity = false;
                    i += 3;
                }
                else if (Tok == TEXT("center") && T.IsValidIndex(i + 2))
                {
                    Center.X = FCString::Atof(*T[i + 1]);
                    Center.Y = FCString::Atof(*T[i + 2]);
                    i += 3;
                }
                else if (Tok == TEXT("rotate") && T.IsValidIndex(i + 1))
                {
                    Take(Out.Rotate);
                    if (Out.Rotate != 0.f) { Out.bIdentity = false; }
                    i += 2;
                }
                else
                {
                    ++i;
                }
            }
            // Center is the pivot for the rotate; the master applies (uv - center) * scale +
            // center + translate, so we don't have to bake it here. Currently the master only
            // exposes scale / rotate / translate; we treat any non-zero center as part of the
            // translate to keep the parameter set minimal.
            if (!Center.IsNearlyZero())
            {
                Out.Translate += Center;
                Out.bIdentity = false;
            }
            return !Out.bIdentity;
        }

        // Shader family classification used to pick (a) the parent material slot and
        // (b) the histogram bucket. Returned by ClassifyShader.
        enum class EShaderFamily : uint8
        {
            Lit,
            LitMasked,
            LitTranslucent,
            Decal,
            WorldVertexTransition,
            VertexLit,
            Unlit,
            Other
        };

        EShaderFamily ClassifyShader(const FString& ShaderLower, bool bMasked, bool bTranslucent)
        {
            if (ShaderLower == TEXT("worldvertextransition")) { return EShaderFamily::WorldVertexTransition; }
            if (ShaderLower == TEXT("unlitgeneric"))          { return EShaderFamily::Unlit; }
            if (ShaderLower == TEXT("vertexlitgeneric"))      { return EShaderFamily::VertexLit; }
            if (ShaderLower.Contains(TEXT("decal")))          { return EShaderFamily::Decal; }
            if (ShaderLower == TEXT("lightmappedgeneric"))
            {
                if (bTranslucent) { return EShaderFamily::LitTranslucent; }
                if (bMasked)      { return EShaderFamily::LitMasked; }
                return EShaderFamily::Lit;
            }
            // Unknown shader -> fall through the lightmapped chain by translucency / alpha.
            return EShaderFamily::Other;
        }
    } // namespace

    FBuilder::FBuilder(const UHL2BSPImporterSettings* InSettings)
        : Settings(InSettings)
    {
        if (!Settings) { return; }
        AssetRoot = Settings->SynthesizedAssetRoot;
        if (AssetRoot.IsEmpty() || !AssetRoot.StartsWith(TEXT("/Game/")))
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("SynthesizedAssetRoot '%s' is not a /Game/ path; material synthesis disabled."), *AssetRoot);
            AssetRoot.Reset();
        }
        Roots.Reserve(Settings->SourceContentRoots.Num());
        for (const FString& R : Settings->SourceContentRoots)
        {
            if (!R.IsEmpty()) { Roots.Add(FPaths::ConvertRelativePathToFull(R)); }
        }
        if (Settings->bSynthesizeMaterials)
        {
            int32 MissingParents = 0;
            MissingParents += (LoadParentMaterial(TEXT("ParentMaterial_LightmappedGeneric"), Settings->ParentMaterial_LightmappedGeneric, true) ? 0 : 1);
            MissingParents += (LoadParentMaterial(TEXT("ParentMaterial_LightmappedGeneric_Masked"), Settings->ParentMaterial_LightmappedGeneric_Masked, true) ? 0 : 1);
            MissingParents += (LoadParentMaterial(TEXT("ParentMaterial_LightmappedGeneric_Translucent"), Settings->ParentMaterial_LightmappedGeneric_Translucent, true) ? 0 : 1);
            MissingParents += (LoadParentMaterial(TEXT("ParentMaterial_LightmappedGeneric_Decal"), Settings->ParentMaterial_LightmappedGeneric_Decal, true) ? 0 : 1);
            MissingParents += (LoadParentMaterial(TEXT("ParentMaterial_WorldVertexTransition"), Settings->ParentMaterial_WorldVertexTransition, true) ? 0 : 1);
            MissingParents += (LoadParentMaterial(TEXT("ParentMaterial_VertexLitGeneric"), Settings->ParentMaterial_VertexLitGeneric, true) ? 0 : 1);
            MissingParents += (LoadParentMaterial(TEXT("ParentMaterial_UnlitGeneric"), Settings->ParentMaterial_UnlitGeneric, true) ? 0 : 1);
            if (MissingParents > 0)
            {
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Material synthesis is enabled but %d parent material slot(s) are missing; affected MIC synthesis will be skipped."),
                    MissingParents);
            }
        }
    }

    void FBuilder::AddExtraRoot(const FString& AbsoluteRoot)
    {
        if (AbsoluteRoot.IsEmpty()) { return; }
        // Higher priority than user-configured roots.
        Roots.Insert(FPaths::ConvertRelativePathToFull(AbsoluteRoot), 0);
    }

    FString FBuilder::FindMaterialFile(const FString& Key) const
    {
        IFileManager& FM = IFileManager::Get();
        for (const FString& Root : Roots)
        {
            const FString P = Root / TEXT("materials") / Key + TEXT(".vmt");
            if (FM.FileExists(*P)) { return P; }
        }
        return {};
    }
    FString FBuilder::FindTextureFile(const FString& Key) const
    {
        IFileManager& FM = IFileManager::Get();
        for (const FString& Root : Roots)
        {
            const FString P = Root / TEXT("materials") / Key + TEXT(".vtf");
            if (FM.FileExists(*P)) { return P; }
        }
        return {};
    }

    UMaterialInterface* FBuilder::PickParent(const FString& ShaderLower, bool bMasked, bool bTranslucent) const
    {
        if (!Settings) { return nullptr; }

        const EShaderFamily Family = ClassifyShader(ShaderLower, bMasked, bTranslucent);
        switch (Family)
        {
        case EShaderFamily::WorldVertexTransition:
            if (UMaterialInterface* M = LoadParentMaterial(TEXT("ParentMaterial_WorldVertexTransition"), Settings->ParentMaterial_WorldVertexTransition, false)) { return M; }
            break;
        case EShaderFamily::Unlit:
            if (UMaterialInterface* M = LoadParentMaterial(TEXT("ParentMaterial_UnlitGeneric"), Settings->ParentMaterial_UnlitGeneric, false)) { return M; }
            break;
        case EShaderFamily::VertexLit:
            if (UMaterialInterface* M = LoadParentMaterial(TEXT("ParentMaterial_VertexLitGeneric"), Settings->ParentMaterial_VertexLitGeneric, false)) { return M; }
            break;
        case EShaderFamily::Decal:
            if (UMaterialInterface* M = LoadParentMaterial(TEXT("ParentMaterial_LightmappedGeneric_Decal"), Settings->ParentMaterial_LightmappedGeneric_Decal, false)) { return M; }
            // Fall through to translucent/masked/lit if no decal parent configured.
            break;
        default: break;
        }

        // LightmappedGeneric and unknown shaders -> pick by translucency / alpha test.
        if (bTranslucent)
        {
            if (UMaterialInterface* M = LoadParentMaterial(TEXT("ParentMaterial_LightmappedGeneric_Translucent"), Settings->ParentMaterial_LightmappedGeneric_Translucent, false)) { return M; }
        }
        if (bMasked)
        {
            if (UMaterialInterface* M = LoadParentMaterial(TEXT("ParentMaterial_LightmappedGeneric_Masked"), Settings->ParentMaterial_LightmappedGeneric_Masked, false)) { return M; }
        }
        return LoadParentMaterial(TEXT("ParentMaterial_LightmappedGeneric"), Settings->ParentMaterial_LightmappedGeneric, false);
    }

    UTexture2D* FBuilder::GetOrCreateTexture(const FString& TextureKeyIn, bool bIsNormalMap)
    {
        const FString Key = NormalizeKey(TextureKeyIn);
        if (Key.IsEmpty()) { return nullptr; }

        if (TObjectPtr<UTexture2D>* Found = TextureCache.Find(Key))
        {
            ++NumTexturesCached;
            return Found->Get();
        }

        FString PkgPath, AssetName;
        SplitAssetPath(AssetRoot, TEXT("Textures"), Key, PkgPath, AssetName);

        // Re-use any already-imported asset on disk.
        if (UTexture2D* Existing = FindExistingAsset<UTexture2D>(PkgPath, AssetName))
        {
            TextureCache.Add(Key, Existing);
            ++NumTexturesCached;
            return Existing;
        }

        const FString VtfPath = FindTextureFile(Key);
        if (VtfPath.IsEmpty())
        {
            UE_LOG(LogHL2BSPImporter, Verbose, TEXT("VTF not found for key '%s'"), *Key);
            ++NumTexturesFailed;
            return nullptr;
        }

        HL2VTF::FInfo Info;
        TArray<uint8> BGRA;
        FString Err;
        if (!HL2VTF::LoadAndDecode(VtfPath, Info, BGRA, Err))
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("VTF decode failed (%s): %s"), *Err, *VtfPath);
            ++NumTexturesFailed;
            return nullptr;
        }

        FString FullName;
        UPackage* Pkg = CreateAssetPackage(PkgPath, AssetName, FullName);
        if (!Pkg)
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("CreatePackage failed: %s"), *FullName);
            ++NumTexturesFailed;
            return nullptr;
        }

        UTexture2D* Tex = NewObject<UTexture2D>(Pkg, FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Tex)
        {
            ++NumTexturesFailed;
            return nullptr;
        }

#if WITH_EDITORONLY_DATA
        Tex->Source.Init(Info.Width, Info.Height, /*NumSlices*/1, /*NumMips*/1,
                         TSF_BGRA8, BGRA.GetData());
#endif
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
        Tex->UpdateResource();
        Tex->PostEditChange();

        FAssetRegistryModule::AssetCreated(Tex);
        Pkg->MarkPackageDirty();

        TextureCache.Add(Key, Tex);
        ++NumTexturesCreated;
        return Tex;
    }

    UMaterialInterface* FBuilder::GetOrCreateMaterial(const FString& TextureKeyIn)
    {
        if (!Settings || !Settings->bSynthesizeMaterials || AssetRoot.IsEmpty()) { return nullptr; }

        const FString Key = NormalizeKey(TextureKeyIn);
        if (Key.IsEmpty()) { return nullptr; }

        if (TObjectPtr<UMaterialInterface>* Found = MaterialCache.Find(Key))
        {
            ++NumMaterialsCached;
            return Found->Get();
        }

        FString PkgPath, AssetName;
        SplitAssetPath(AssetRoot, TEXT("Materials"), Key, PkgPath, AssetName);

        if (UMaterialInterface* Existing = FindExistingAsset<UMaterialInstanceConstant>(PkgPath, AssetName))
        {
            MaterialCache.Add(Key, Existing);
            ++NumMaterialsCached;
            return Existing;
        }

        const FString VmtPath = FindMaterialFile(Key);
        if (VmtPath.IsEmpty())
        {
            UE_LOG(LogHL2BSPImporter, Verbose, TEXT("VMT not found for slot '%s'"), *Key);
            ++NumMaterialsFailed;
            return nullptr;
        }

        HL2VMT::FDocument Doc;
        FString Err;
        if (!HL2VMT::ParseFile(VmtPath, Doc, Err))
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT parse failed (%s): %s"), *Err, *VmtPath);
            ++NumMaterialsFailed;
            return nullptr;
        }
        if (!HL2VMT::ResolvePatches(Doc, Roots, Err))
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT patch resolve failed (%s): %s"), *Err, *VmtPath);
            ++NumMaterialsFailed;
            return nullptr;
        }
        if (!Doc.Root.IsValid())
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("VMT has empty body: %s"), *VmtPath);
            ++NumMaterialsFailed;
            return nullptr;
        }

        const HL2VMT::FBlock& Body = *Doc.Root;
        const bool bTranslucent = Body.GetBool(TEXT("$translucent"), false) || Body.GetBool(TEXT("$alpha"), false);
        const bool bAlphaTest   = Body.GetBool(TEXT("$alphatest"), false);
        UMaterialInterface* Parent = PickParent(Doc.ShaderLower, bAlphaTest, bTranslucent);
        if (!Parent)
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("No loadable parent material for shader '%s' (slot '%s'); skipping synthesis."),
                *Doc.ShaderLower, *Key);
            ++NumMaterialsFailed;
            return nullptr;
        }

        // Build the MIC asset.
        FString FullName;
        UPackage* Pkg = CreateAssetPackage(PkgPath, AssetName, FullName);
        if (!Pkg)
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("CreatePackage failed: %s"), *FullName);
            ++NumMaterialsFailed;
            return nullptr;
        }

        UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(Pkg, FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!MIC)
        {
            ++NumMaterialsFailed;
            return nullptr;
        }

#if WITH_EDITOR
        MIC->SetParentEditorOnly(Parent);

        auto SetTexParam = [&](const TCHAR* ParamName, const FString& VmtKey, bool bIsNormal)
        {
            FString TexKey;
            if (!Body.GetString(VmtKey, TexKey) || TexKey.IsEmpty()) { return; }
            if (UTexture2D* Tex = GetOrCreateTexture(TexKey, bIsNormal))
            {
                MIC->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(ParamName), Tex);
            }
        };
        auto SetScalar = [&](const TCHAR* ParamName, float Value)
        {
            MIC->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(ParamName), Value);
        };
        auto SetVector = [&](const TCHAR* ParamName, const FLinearColor& Value)
        {
            MIC->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(ParamName), Value);
        };

        // ---- Phase 5 / 5b textures ----
        SetTexParam(TEXT("BaseColor"),  TEXT("$basetexture"),  /*bIsNormal*/ false);
        SetTexParam(TEXT("BaseColor2"), TEXT("$basetexture2"), /*bIsNormal*/ false);
        SetTexParam(TEXT("Normal"),     TEXT("$bumpmap"),      /*bIsNormal*/ true);
        SetTexParam(TEXT("Normal2"),    TEXT("$bumpmap2"),     /*bIsNormal*/ true);
        SetTexParam(TEXT("Detail"),     TEXT("$detail"),       /*bIsNormal*/ false);
        // WorldVertexTransition blend reshaper: red channel = blend amount remap,
        // green channel = blend softness. Master mat is expected to multiply/clamp
        // VertexColor.A by this texture before doing the BaseColor<->BaseColor2 lerp.
        SetTexParam(TEXT("BlendModulate"), TEXT("$blendmodulatetexture"), /*bIsNormal*/ false);

        if (bAlphaTest)
        {
            const float Ref = Body.GetFloat(TEXT("$alphatestreference"), 0.5f);
            SetScalar(TEXT("AlphaTestRef"), Ref);
        }
        if (const FString* DetailScale = Body.FindString(TEXT("$detailscale")))
        {
            SetScalar(TEXT("DetailScale"), FCString::Atof(**DetailScale));
        }

        // ---- Phase 11b: extended parameters ----
        // Self-illumination. $selfillummask is an explicit RGB mask; when absent the
        // standard Source convention is to use the BaseColor's alpha channel, but
        // the master is expected to handle that fallback so we only bind the mask if
        // the VMT supplies one.
        SetTexParam(TEXT("EmissiveColor"), TEXT("$selfillummask"), /*bIsNormal*/ false);
        if (Body.GetBool(TEXT("$selfillum"), false))
        {
            SetScalar(TEXT("EmissiveStrength"), 1.f);
        }
        if (const FString* SelfIllumTint = Body.FindString(TEXT("$selfillumtint")))
        {
            FLinearColor C;
            if (ParseSourceColor(*SelfIllumTint, C)) { SetVector(TEXT("EmissiveTint"), C); }
        }

        // Color tints. $color and $color2 multiply BaseColor / BaseColor2.
        if (const FString* ColorStr = Body.FindString(TEXT("$color")))
        {
            FLinearColor C;
            if (ParseSourceColor(*ColorStr, C)) { SetVector(TEXT("BaseColorTint"), C); }
        }
        if (const FString* Color2Str = Body.FindString(TEXT("$color2")))
        {
            FLinearColor C;
            if (ParseSourceColor(*Color2Str, C)) { SetVector(TEXT("BaseColor2Tint"), C); }
        }
        if (const FString* AlphaStr = Body.FindString(TEXT("$alpha")))
        {
            SetScalar(TEXT("OpacityScalar"), FCString::Atof(**AlphaStr));
        }

        // Cubemap reflections. The actual TextureCube wiring is deferred to Phase 11c
        // (LUMP_CUBEMAPS investigation); for now the master ships with a neutral grey
        // default Cubemap parameter and the VMT-supplied tint is bound when present.
        // Source's `env_cubemap` literal means "use the per-leaf baked cubemap" — we
        // can't honour that yet, so any non-literal cube path is passed through to the
        // master as a hint (currently unused, future phase will resolve it).
        if (const FString* EnvTint = Body.FindString(TEXT("$envmaptint")))
        {
            FLinearColor C;
            if (ParseSourceColor(*EnvTint, C)) { SetVector(TEXT("EnvmapTint"), C); }
        }

        // Phong specular. Source's `$phong 1` switches a prop to the phong code path;
        // exponent and boost shape the highlight. We translate the flag into a 0/1
        // scalar so the master can multiply the specular contribution down to zero
        // when phong was off in the VMT.
        if (Body.GetBool(TEXT("$phong"), false))
        {
            SetScalar(TEXT("Phong"), 1.f);
            SetScalar(TEXT("PhongExponent"), Body.GetFloat(TEXT("$phongexponent"), 5.f));
            SetScalar(TEXT("PhongBoost"),    Body.GetFloat(TEXT("$phongboost"), 1.f));
        }

        // UV transforms. Source applies a single matrix per channel; the master is
        // expected to expose Scale/Rotate/Offset parameters and multiply CustomUV0
        // before the texture sample.
        if (const FString* BTT = Body.FindString(TEXT("$basetexturetransform")))
        {
            FUVTransform T;
            if (ParseUVTransform(*BTT, T))
            {
                SetVector(TEXT("BaseColorUVScale"),  FLinearColor(T.Scale.X, T.Scale.Y, 0.f, 0.f));
                SetVector(TEXT("BaseColorUVOffset"), FLinearColor(T.Translate.X, T.Translate.Y, 0.f, 0.f));
                SetScalar(TEXT("BaseColorUVRotate"), T.Rotate);
            }
        }
        if (const FString* BMT = Body.FindString(TEXT("$bumptransform")))
        {
            FUVTransform T;
            if (ParseUVTransform(*BMT, T))
            {
                SetVector(TEXT("NormalUVScale"),  FLinearColor(T.Scale.X, T.Scale.Y, 0.f, 0.f));
                SetVector(TEXT("NormalUVOffset"), FLinearColor(T.Translate.X, T.Translate.Y, 0.f, 0.f));
                SetScalar(TEXT("NormalUVRotate"), T.Rotate);
            }
        }

        // Detail blend. Mode is an integer 0..12 in Source; passed through as a scalar
        // so the master can switch on it.
        if (const FString* DBM = Body.FindString(TEXT("$detailblendmode")))
        {
            SetScalar(TEXT("DetailBlendMode"), (float)FCString::Atoi(**DBM));
        }
        if (const FString* DBF = Body.FindString(TEXT("$detailblendfactor")))
        {
            SetScalar(TEXT("DetailBlendFactor"), FCString::Atof(**DBF));
        }
#endif

        MIC->PostEditChange();
        FAssetRegistryModule::AssetCreated(MIC);
        Pkg->MarkPackageDirty();

        MaterialCache.Add(Key, MIC);
        ++NumMaterialsCreated;

        // Phase 11b shader-family histogram. Bumped per *created* MIC (not per cached
        // hit) so the totals reflect distinct material shapes in the map.
        switch (ClassifyShader(Doc.ShaderLower, bAlphaTest, bTranslucent))
        {
        case EShaderFamily::Lit:                   ++NumShader_Lit;       break;
        case EShaderFamily::LitMasked:             ++NumShader_LitMasked; break;
        case EShaderFamily::LitTranslucent:        ++NumShader_LitTrans;  break;
        case EShaderFamily::Decal:                 ++NumShader_Decal;     break;
        case EShaderFamily::WorldVertexTransition: ++NumShader_Wvt;       break;
        case EShaderFamily::VertexLit:             ++NumShader_VertexLit; break;
        case EShaderFamily::Unlit:                 ++NumShader_Unlit;     break;
        default:                                   ++NumShader_Other;     break;
        }
        return MIC;
    }
}
