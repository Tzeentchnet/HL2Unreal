#pragma once
#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "HL2BSPImporterTypes.h"
#include "HL2EntityTable.generated.h"

USTRUCT()
struct FHL2EntityTableRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, Category = "HL2") FHL2Entity Entity;
};

UCLASS()
class UHL2EntityTable : public UDataTable
{
    GENERATED_BODY()
public:
    static UHL2EntityTable* CreateFromEntities(UObject* Outer, const TArray<FHL2Entity>& Entities);
};