#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HL2BSPImporterSettings.generated.h"

class UMaterialInterface;

UCLASS(config = HL2BSPImporter, defaultconfig, meta = (DisplayName = "HL2 BSP Importer"))
class HL2BSPIMPORTER_API UHL2BSPImporterSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    // -------- Coordinate / scale --------
    UPROPERTY(config, EditAnywhere, Category = "Scale")
    float WorldScale = 2.54f;

    UPROPERTY(config, EditAnywhere, Category = "Axis")
    bool bFlipYZ = true;

    // -------- Material map (legacy explicit overrides) --------
    // Leave empty to use plugin fallback at Plugins/HL2BSPImporter/Resources/Materials.json
    UPROPERTY(config, EditAnywhere, Category = "Materials|Override Map")
    FString MaterialJsonPath = TEXT("");

    // -------- Material synthesis (VMT/VTF) --------
    // Master switch. When true, slots with no explicit JSON mapping are synthesized
    // from .vmt files found under SourceContentRoots.
    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis")
    bool bSynthesizeMaterials = true;

    // Filesystem directories that contain a `materials/` (and optionally `textures/`) subtree.
    // Typical entries: an extracted `hl2_misc_dir.vpk`, a mod's `materials` parent folder, or
    // a flattened `<game>/<mod>/` directory. Searched in order; first hit wins.
    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis")
    TArray<FString> SourceContentRoots;

    // Long-package path under which synthesized UTexture2D / UMaterialInstanceConstant assets
    // are created. Subfolders mirror the Source `materials/<path>` hierarchy. Must start with
    // `/Game/`. Example: `/Game/HL2/Imported`.
    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis")
    FString SynthesizedAssetRoot = TEXT("/Game/HL2/Imported");

    // Parent materials. Each should be a UMaterial (or UMaterialInstance) with the parameter
    // contract documented in Resources/MasterMaterials/README.md. Defaults point at the assets
    // shipped in the plugin's Content/MasterMaterials folder (Phase 11a); override per project
    // by repointing the slot at your own asset. Parameters not exposed by an overridden parent
    // are silently skipped during MIC synthesis.
    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis|Parents", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
    FSoftObjectPath ParentMaterial_LightmappedGeneric = FSoftObjectPath(TEXT("/HL2BSPImporter/MasterMaterials/M_HL2_Lit.M_HL2_Lit"));

    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis|Parents", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
    FSoftObjectPath ParentMaterial_LightmappedGeneric_Masked = FSoftObjectPath(TEXT("/HL2BSPImporter/MasterMaterials/M_HL2_LitMasked.M_HL2_LitMasked"));

    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis|Parents", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
    FSoftObjectPath ParentMaterial_LightmappedGeneric_Translucent = FSoftObjectPath(TEXT("/HL2BSPImporter/MasterMaterials/M_HL2_LitTranslucent.M_HL2_LitTranslucent"));

    // Phase 11b: Deferred Decal domain parent for shaders in the Source decal family
    // (LightmappedGeneric_DecalGroup, decalmodulate). The parent is expected to declare
    // the standard BaseColor / Normal parameters and use the Deferred Decal material domain.
    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis|Parents", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
    FSoftObjectPath ParentMaterial_LightmappedGeneric_Decal = FSoftObjectPath(TEXT("/HL2BSPImporter/MasterMaterials/M_HL2_LitDecal.M_HL2_LitDecal"));

    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis|Parents", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
    FSoftObjectPath ParentMaterial_WorldVertexTransition = FSoftObjectPath(TEXT("/HL2BSPImporter/MasterMaterials/M_HL2_WorldVertexBlend.M_HL2_WorldVertexBlend"));

    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis|Parents", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
    FSoftObjectPath ParentMaterial_VertexLitGeneric = FSoftObjectPath(TEXT("/HL2BSPImporter/MasterMaterials/M_HL2_VertexLit.M_HL2_VertexLit"));

    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis|Parents", meta = (AllowedClasses = "/Script/Engine.MaterialInterface"))
    FSoftObjectPath ParentMaterial_UnlitGeneric = FSoftObjectPath(TEXT("/HL2BSPImporter/MasterMaterials/M_HL2_Unlit.M_HL2_Unlit"));

    // -------- Mesh build --------
    UPROPERTY(config, EditAnywhere, Category = "Import")
    bool bBuildNanite = true;

    UPROPERTY(config, EditAnywhere, Category = "Import")
    bool bImportCollision = true;

    // -------- Displacements --------
    // When true, vertices on the perimeter of every displacement grid are clustered with a
    // small spatial epsilon and snapped to the cluster centroid. This eliminates the hairline
    // T-junction cracks that appear at terrain seams when neighbouring displacements were
    // authored without a perfect Hammer "Sew" pass.
    UPROPERTY(config, EditAnywhere, Category = "Displacements")
    bool bStitchDisplacementSeams = true;

    // Maximum distance (in Unreal centimetres) between two displacement-perimeter vertices
    // for them to be welded together. 1.0 covers float-precision noise on a 2.54 cm/inch map
    // without distorting intentionally-separated edges.
    UPROPERTY(config, EditAnywhere, Category = "Displacements", meta = (EditCondition = "bStitchDisplacementSeams", ClampMin = "0.0001", ClampMax = "16.0"))
    float DisplacementSeamWeldDistance = 1.0f;

    UPROPERTY(config, EditAnywhere, Category = "Props")
    bool bImportPropsAsInstances = false;

    // Phase 12: synthesise per-unique-model UStaticMesh assets from the BSP's
    // static-prop instance table. When true the factory walks every
    // `prop_static`'s ModelName, locates its .mdl/.vvd/.dx90.vtx triple under
    // SourceContentRoots (and the per-import pakfile extract dir), and creates
    // one UStaticMesh per unique model under
    // `<SynthesizedAssetRoot>/Props/<model path>`. Each FHL2StaticProp row's
    // StaticMeshAsset field is then populated so a Blueprint / editor utility
    // can spawn AStaticMeshActors at the recorded transforms.
    //
    // Default off until enough community maps have been validated to stabilise
    // the importer; failures fall through to leaving StaticMeshAsset empty.
    UPROPERTY(config, EditAnywhere, Category = "Props")
    bool bImportStaticPropMeshes = false;
};
