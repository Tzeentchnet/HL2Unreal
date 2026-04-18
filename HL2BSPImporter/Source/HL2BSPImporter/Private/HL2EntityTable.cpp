#include "HL2EntityTable.h"
#include "Misc/Paths.h"

UHL2EntityTable* UHL2EntityTable::CreateFromEntities(UObject* Outer, FName Name, EObjectFlags Flags, const TArray<FHL2Entity>& Entities)
{
    const EObjectFlags EffectiveFlags = Flags | RF_Public | RF_Standalone | RF_Transactional;
    UHL2EntityTable* Table = NewObject<UHL2EntityTable>(Outer, Name, EffectiveFlags);
    Table->RowStruct = FHL2EntityTableRow::StaticStruct();

    for (int32 i = 0; i < Entities.Num(); ++i)
    {
        const FHL2Entity& E = Entities[i];
        // Prefer entity targetname for the row id when present; otherwise use index.
        FString RowKey = E.Name.IsEmpty()
            ? FString::Printf(TEXT("%d_%s"), i, *E.Class)
            : E.Name;
        // Sanitize and de-duplicate row name.
        RowKey = RowKey.Replace(TEXT(" "), TEXT("_"));
        FName RowName(*RowKey);
        int32 Suffix = 2;
        while (Table->GetRowMap().Contains(RowName))
        {
            RowName = FName(*FString::Printf(TEXT("%s_%d"), *RowKey, Suffix++));
        }

        FHL2EntityTableRow Row;
        Row.Entity = E;
        Table->AddRow(RowName, Row);
    }
    return Table;
}

UHL2StaticPropTable* UHL2StaticPropTable::CreateFromProps(UObject* Outer, FName Name, EObjectFlags Flags, const TArray<FHL2StaticProp>& Props)
{
    const EObjectFlags EffectiveFlags = Flags | RF_Public | RF_Standalone | RF_Transactional;
    UHL2StaticPropTable* Table = NewObject<UHL2StaticPropTable>(Outer, Name, EffectiveFlags);
    Table->RowStruct = FHL2StaticPropTableRow::StaticStruct();

    for (int32 i = 0; i < Props.Num(); ++i)
    {
        const FHL2StaticProp& P = Props[i];
        // Use a stable per-index key. Static props are anonymous in Source so we can't derive
        // a meaningful name from the model path alone (many duplicates).
        FString Base = FPaths::GetBaseFilename(P.ModelName);
        if (Base.IsEmpty()) { Base = TEXT("prop"); }
        Base = Base.Replace(TEXT(" "), TEXT("_"));
        FString RowKey = FString::Printf(TEXT("%d_%s"), i, *Base);
        FName RowName(*RowKey);
        int32 Suffix = 2;
        while (Table->GetRowMap().Contains(RowName))
        {
            RowName = FName(*FString::Printf(TEXT("%s_%d"), *RowKey, Suffix++));
        }

        FHL2StaticPropTableRow Row;
        Row.Prop = P;
        Table->AddRow(RowName, Row);
    }
    return Table;
}
