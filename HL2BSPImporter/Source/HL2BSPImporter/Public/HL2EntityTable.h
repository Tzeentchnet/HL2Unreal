#pragma once
#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "HL2BSPImporterTypes.h"
#include "HL2EntityTable.generated.h"

USTRUCT()
struct HL2BSPIMPORTER_API FHL2EntityTableRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, Category = "HL2") FHL2Entity Entity;
};

USTRUCT()
struct HL2BSPIMPORTER_API FHL2StaticPropTableRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, Category = "HL2") FHL2StaticProp Prop;
};

UCLASS()
class HL2BSPIMPORTER_API UHL2EntityTable : public UDataTable
{
    GENERATED_BODY()
public:
    static UHL2EntityTable* CreateFromEntities(UObject* Outer, FName Name, EObjectFlags Flags, const TArray<FHL2Entity>& Entities);
};

UCLASS()
class HL2BSPIMPORTER_API UHL2StaticPropTable : public UDataTable
{
    GENERATED_BODY()
public:
    // Persists transformed (Unreal-coordinate) static-prop instances. Caller is expected to have
    // applied the bsp coordinate transform (cm, left-handed) to each Prop.Origin/Rotation already.
    static UHL2StaticPropTable* CreateFromProps(UObject* Outer, FName Name, EObjectFlags Flags, const TArray<FHL2StaticProp>& Props);
};
