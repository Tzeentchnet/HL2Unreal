#pragma once
#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"

class UMaterialInterface;
class UMaterialInstanceConstant;
class UTexture2D;
class UPackage;
class UHL2BSPImporterSettings;
class UObject;

namespace HL2VMT { struct FDocument; }

namespace HL2Mat
{
    // Per-import cache. Holds rooted strong pointers to created assets so they
    // survive GC for the duration of the import. Discarded when the import
    // finishes.
    class HL2BSPIMPORTER_API FBuilder
    {
    public:
        explicit FBuilder(const UHL2BSPImporterSettings* InSettings);

        // Add an additional content root searched before the user-configured ones.
        // Used by the factory to inject a per-import temp directory containing files
        // extracted from the BSP's embedded pakfile (LUMP_PAKFILE).
        void AddExtraRoot(const FString& AbsoluteRoot);

        // Resolve a Source texture key (e.g. "concrete/concretefloor028a", lower-case,
        // forward slashes) to a synthesized UMaterialInstanceConstant. Returns null
        // if synthesis is disabled, the .vmt cannot be located, parsing fails, or the
        // selected parent material is unset.
        UMaterialInterface* GetOrCreateMaterial(const FString& TextureKey);

        // Build a UMaterialInstanceConstant from an already-parsed VMT document into
        // a caller-supplied package. Used by the standalone .vmt factory so the user's
        // chosen Content Browser destination is honoured. The MIC's parent material
        // and parameters are bound exactly as in GetOrCreateMaterial; referenced
        // textures are still routed through this builder's GetOrCreateTexture (so they
        // land at the canonical SynthesizedAssetRoot/Textures/<key> locations and are
        // shared with any concurrent BSP-driven import).
        //
        // Increments NumMaterialsCreated / shader-family counters on success; on
        // failure increments NumMaterialsFailed and returns nullptr with OutError set.
        UMaterialInstanceConstant* BuildMICIntoPackage(
            UPackage* TargetPackage,
            FName AssetName,
            const HL2VMT::FDocument& Doc,
            FString& OutError);

        // Resolve+create a UTexture2D for a Source texture key. Public so the
        // standalone .vmt / .vtf factories can route texture references through the
        // same canonical-path / cache pipeline. bIsNormalMap selects sRGB-off +
        // TC_Normalmap + TEXTUREGROUP_WorldNormalMap. Returns null if the .vtf
        // cannot be located or decoded.
        UTexture2D* GetOrCreateTextureForKey(const FString& TextureKey, bool bIsNormalMap)
        {
            return GetOrCreateTexture(TextureKey, bIsNormalMap);
        }

        // Resolve+create a sibling alpha-mask UTexture2D for a Source normal-map
        // texture key. Source frequently packs phong / envmap / specular masks
        // into the alpha channel of `$bumpmap` VTFs, but UE normal-map textures
        // (TC_Normalmap) drop alpha during compression. When the source VTF
        // carries non-uniform alpha this method emits a sibling `<key>_a`
        // UTexture2D (sRGB off, TC_Grayscale) at
        // `<SynthesizedAssetRoot>/Textures/<sub>/<name>_a` and returns it so the
        // MIC can bind a `NormalAlpha` parameter. Returns null when the source
        // VTF carries no meaningful alpha (also caches the negative result so
        // repeated callers don't redecode).
        UTexture2D* GetOrCreateNormalAlphaSibling(const FString& TextureKey);

        // Stats — written by GetOrCreateMaterial; read by the caller for log output.
        int32 NumMaterialsCreated = 0;
        int32 NumMaterialsCached  = 0;
        int32 NumMaterialsFailed  = 0;
        int32 NumTexturesCreated  = 0;
        int32 NumTexturesCached   = 0;
        int32 NumTexturesFailed   = 0;

        // Per-shader-family histogram (Phase 11b). Bumped once per *created* MIC.
        // `Other` covers shaders we don't have a dedicated parent slot for (still synthesised
        // via the LightmappedGeneric fallback chain).
        int32 NumShader_Lit         = 0;
        int32 NumShader_LitMasked   = 0;
        int32 NumShader_LitTrans    = 0;
        int32 NumShader_Wvt         = 0;
        int32 NumShader_VertexLit   = 0;
        int32 NumShader_Unlit       = 0;
        int32 NumShader_Decal       = 0;
        int32 NumShader_Other       = 0;

    private:
        UTexture2D* GetOrCreateTexture(const FString& TextureKey, bool bIsNormalMap);
        UMaterialInterface* PickParent(const FString& ShaderLower, bool bMasked, bool bTranslucent) const;

        // Bind every supported $param from a parsed VMT body onto an MIC and bump
        // the per-shader-family histogram. Pre: MIC has already had its parent
        // material set. Used by both GetOrCreateMaterial and BuildMICIntoPackage.
        void BindMICParameters(UMaterialInstanceConstant* MIC,
                               const HL2VMT::FDocument& Doc,
                               bool bAlphaTest, bool bTranslucent);

        // Locate `materials/<key>.vmt` (or `.vtf` for textures) under the configured roots.
        FString FindMaterialFile(const FString& Key) const;
        FString FindTextureFile (const FString& Key) const;

        // Lower-cased Source keys map to created assets.
        TMap<FString, TObjectPtr<UMaterialInterface>> MaterialCache;
        TMap<FString, TObjectPtr<UTexture2D>>         TextureCache;
        // Per-key cached normal-map alpha sibling. Stores nullptr (sentinel) for
        // keys whose source VTF carried no meaningful alpha so repeated lookups
        // skip the redecode.
        TMap<FString, TObjectPtr<UTexture2D>>         NormalAlphaCache;

        const UHL2BSPImporterSettings* Settings = nullptr;
        TArray<FString>                Roots;        // copied from Settings, normalised
        FString                        AssetRoot;    // e.g. /Game/HL2/Imported
    };
}
