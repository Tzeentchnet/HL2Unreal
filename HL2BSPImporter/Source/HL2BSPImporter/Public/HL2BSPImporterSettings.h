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

    // -------- Skybox (Phase A1) --------
    // When true, faces flagged SURF_SKY / SURF_SKY2D are dropped from the
    // worldspawn mesh (as before) AND their material names are captured. For
    // each unique skybox base referenced, the importer assembles the six face
    // VTFs into one UTextureCube under <SynthesizedAssetRoot>/Skies/<base>.
    // HDR formats (RGBA16F/16) are skipped with a warning in this phase.
    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis")
    bool bConvertSkyboxes = true;

    // -------- Surface properties (Phase A3) --------
    // When true, each BSP import scans for `<SourceContentRoots>/scripts/
    // surfaceproperties.txt` (KV1 format) and emits one USurfaceProp asset
    // per surface entry under <SynthesizedAssetRoot>/SurfaceProps/<name>.
    // Idempotent — re-imports reuse existing assets.
    UPROPERTY(config, EditAnywhere, Category = "Materials|Synthesis")
    bool bImportSurfaceProperties = true;

    // -------- Mesh build --------
    UPROPERTY(config, EditAnywhere, Category = "Import")
    bool bBuildNanite = true;

    UPROPERTY(config, EditAnywhere, Category = "Import")
    bool bImportCollision = true;

    // -------- Lightmaps (Phase A2) --------
    // When true, each built UStaticMesh's MinLightmapResolution is computed
    // from its surface area × LightmapTexelDensity instead of the legacy
    // hardcoded value (128 for worldspawn / 64 for props). Default-off so
    // existing maps re-import byte-identical until calibration is validated.
    UPROPERTY(config, EditAnywhere, Category = "Import|Lightmaps")
    bool bLightmapResolutionFromArea = false;

    // Texels per Unreal cm of mesh surface area. 0.1 ≈ Source's stock 0.25
    // luxel/inch baseline. Calibrate against your project's Lumen / lightmap
    // budget; raise for higher-quality bakes.
    UPROPERTY(config, EditAnywhere, Category = "Import|Lightmaps", meta = (EditCondition = "bLightmapResolutionFromArea", ClampMin = "0.001", ClampMax = "10.0"))
    float LightmapTexelDensity = 0.1f;

    UPROPERTY(config, EditAnywhere, Category = "Import|Lightmaps", meta = (EditCondition = "bLightmapResolutionFromArea", ClampMin = "16", ClampMax = "2048"))
    int32 MinLightmapResolutionClamp = 32;

    UPROPERTY(config, EditAnywhere, Category = "Import|Lightmaps", meta = (EditCondition = "bLightmapResolutionFromArea", ClampMin = "16", ClampMax = "4096"))
    int32 MaxLightmapResolutionClamp = 512;

    // -------- Visibility (Phase A5) --------
    // When true, emit a sibling <MapName>_VBSPInfo UDataAsset carrying the
    // raw LUMP_VISIBILITY bytes (PVS) and per-leaf bounds. Default-off — the
    // asset has no in-engine consumer today; flip on if integrating with a
    // future PVS-driven culling / streaming system.
    UPROPERTY(config, EditAnywhere, Category = "Import")
    bool bExportVBSPInfoAsset = false;

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
