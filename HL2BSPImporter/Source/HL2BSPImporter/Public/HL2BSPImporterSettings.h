#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HL2BSPImporterSettings.generated.h"

UCLASS(config = HL2BSPImporter, defaultconfig, meta = (DisplayName = "HL2 BSP Importer"))
class UHL2BSPImporterSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UPROPERTY(config, EditAnywhere, Category = "Scale")
    float WorldScale = 2.54f;

    UPROPERTY(config, EditAnywhere, Category = "Axis")
    bool bFlipYZ = true;

    UPROPERTY(config, EditAnywhere, Category = "Materials")
    FString MaterialJsonPath = TEXT("/Game/HL2Importer/materials.json");

    UPROPERTY(config, EditAnywhere, Category = "Import")
    bool bBuildNanite = true;

    UPROPERTY(config, EditAnywhere, Category = "Import")
    bool bImportCollision = true;

    UPROPERTY(config, EditAnywhere, Category = "Props")
    bool bImportPropsAsInstances = true;
};
