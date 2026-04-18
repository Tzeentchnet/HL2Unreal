#pragma once
#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"

class UMaterialInterface;
class UMaterialInstanceConstant;
class UTexture2D;
class UHL2BSPImporterSettings;
class UObject;

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

        // Locate `materials/<key>.vmt` (or `.vtf` for textures) under the configured roots.
        FString FindMaterialFile(const FString& Key) const;
        FString FindTextureFile (const FString& Key) const;

        // Lower-cased Source keys map to created assets.
        TMap<FString, TObjectPtr<UMaterialInterface>> MaterialCache;
        TMap<FString, TObjectPtr<UTexture2D>>         TextureCache;

        const UHL2BSPImporterSettings* Settings = nullptr;
        TArray<FString>                Roots;        // copied from Settings, normalised
        FString                        AssetRoot;    // e.g. /Game/HL2/Imported
    };
}
