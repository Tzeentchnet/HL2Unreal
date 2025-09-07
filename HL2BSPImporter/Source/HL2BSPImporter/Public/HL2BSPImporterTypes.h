#pragma once
#include "CoreMinimal.h"
#include "HL2BSPImporterTypes.generated.h"

USTRUCT()
struct FHL2Entity
{
    GENERATED_BODY()
    UPROPERTY() FString Name;
    UPROPERTY() FString Class;
    UPROPERTY() FVector Origin;
    UPROPERTY() FRotator Rotation;
    UPROPERTY() FString Model;
};

USTRUCT()
struct FHL2MaterialEntry
{
    GENERATED_BODY()
    UPROPERTY() FString TextureName;
    UPROPERTY() FSoftObjectPath MaterialPath;
};