#include "HL2EntityTable.h"
UHL2EntityTable* UHL2EntityTable::CreateFromEntities(UObject* Outer, const TArray<FHL2Entity>& Entities)
{
    auto* Table = NewObject<UHL2EntityTable>(Outer, NAME_None, RF_Public | RF_Standalone);
    Table->RowStruct = FHL2EntityTableRow::StaticStruct();
    for (int32 i = 0; i < Entities.Num(); ++i)
    {
        FName RowName = *FString::Printf(TEXT("%d"), i);
        FHL2EntityTableRow Row;
        Row.Entity = Entities[i];
        Table->AddRow(RowName, Row);
    }
#if WITH_EDITOR
    Table->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Table);
#endif
    return Table;
}