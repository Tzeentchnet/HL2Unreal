#pragma once
#include "CoreMinimal.h"
#include "HL2BSPImporterTypes.generated.h"

USTRUCT()
struct HL2BSPIMPORTER_API FHL2Entity
{
    GENERATED_BODY()
    FHL2Entity()
        : Origin(FVector::ZeroVector)
        , Rotation(FRotator::ZeroRotator)
    {}

    UPROPERTY() FString Name;
    UPROPERTY() FString Class;
    UPROPERTY() FVector Origin;
    UPROPERTY() FRotator Rotation;
    UPROPERTY() FString Model;
};

USTRUCT()
struct HL2BSPIMPORTER_API FHL2MaterialEntry
{
    GENERATED_BODY()
    UPROPERTY() FString TextureName;
    UPROPERTY() FSoftObjectPath MaterialPath;
};
