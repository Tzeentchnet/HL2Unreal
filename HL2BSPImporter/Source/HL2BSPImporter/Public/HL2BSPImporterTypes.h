#pragma once
#include "CoreMinimal.h"
#include "HL2BSPImporterTypes.generated.h"

// One Source entity I/O output connection. Source entities expose outputs whose keys begin with
// "On" (e.g. `OnTrigger`, `OnPressed`, `OnStartTouch`, `OnUser1`). Each connection's value is a
// 5-tuple `target,input,parameter,delay,timesToFire` separated by either ',' (HL2-era) or 0x1B
// (later Source). A single output key can appear many times on one entity — one row per fire.
USTRUCT()
struct HL2BSPIMPORTER_API FHL2EntityIO
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FString OutputName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FString TargetName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FString InputName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FString Parameter;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") float   Delay = 0.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") int32   TimesToFire = -1;
};

USTRUCT()
struct HL2BSPIMPORTER_API FHL2Entity
{
    GENERATED_BODY()
    FHL2Entity()
        : Origin(FVector::ZeroVector)
        , Rotation(FRotator::ZeroRotator)
    {}

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FString  Name;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FString  Class;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FVector  Origin;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FRotator Rotation;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FString  Model;

    // For brush entities (model is "*N"), this points at the per-brush-model UStaticMesh asset
    // synthesised by the importer. Empty for point entities or when no asset was produced.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FSoftObjectPath BrushMesh;

    // Source I/O output connections fired by this entity. Preserves duplicate output keys so
    // a single `OnTrigger` with three downstream targets produces three rows here.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") TArray<FHL2EntityIO> Outputs;
};

USTRUCT()
struct HL2BSPIMPORTER_API FHL2MaterialEntry
{
    GENERATED_BODY()
    UPROPERTY() FString TextureName;
    UPROPERTY() FSoftObjectPath MaterialPath;
};

// One row per `prop_static` instance parsed out of the BSP `sprp` GameLump (35).
// The model path is the raw Source-side path, e.g. `models/props_c17/oildrum001.mdl`.
// Origin / Rotation are already in Unreal coordinates (cm, left-handed, Z-up).
// UniformScale is 1.0 unless the BSP carries a v11 static-prop lump.
USTRUCT()
struct HL2BSPIMPORTER_API FHL2StaticProp
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FString  ModelName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FVector  Origin = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FRotator Rotation = FRotator::ZeroRotator;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") float    UniformScale = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") int32    Skin = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") uint8    Solid = 0;

    // Phase 12: synthesised UStaticMesh asset for ModelName, populated when the
    // factory's bImportStaticPropMeshes setting is true and the .mdl/.vvd/.vtx
    // triple resolves under SourceContentRoots / pakfile. Empty otherwise; the
    // user is expected to supply a mesh through their own pipeline.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HL2") FSoftObjectPath StaticMeshAsset;
};
