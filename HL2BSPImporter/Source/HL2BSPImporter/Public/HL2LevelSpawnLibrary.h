#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "HL2LevelSpawnLibrary.generated.h"

class UDataTable;
class AActor;

USTRUCT(BlueprintType)
struct HL2BSPIMPORTER_API FHL2LevelSpawnOptions
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2")
    bool bSpawnStaticProps = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2")
    bool bSpawnBrushEntities = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2")
    bool bSpawnEntityProps = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2")
    bool bSpawnBasicLights = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2")
    bool bSelectSpawnedActors = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2")
    FName FolderRoot = TEXT("HL2Imported");
};

USTRUCT(BlueprintType)
struct HL2BSPIMPORTER_API FHL2LevelSpawnResult
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HL2")
    int32 NumStaticPropsSpawned = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HL2")
    int32 NumBrushEntitiesSpawned = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HL2")
    int32 NumEntityPropsSpawned = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HL2")
    int32 NumLightsSpawned = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HL2")
    int32 NumRowsSkipped = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HL2")
    TArray<TObjectPtr<AActor>> SpawnedActors;
};

UCLASS()
class HL2BSPIMPORTER_API UHL2LevelSpawnLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, CallInEditor, Category = "HL2|Level", meta = (WorldContext = "WorldContextObject"))
    static FHL2LevelSpawnResult SpawnImportedActors(
        UObject* WorldContextObject,
        UDataTable* EntityTable,
        UDataTable* StaticPropTable,
        FHL2LevelSpawnOptions Options);
};
