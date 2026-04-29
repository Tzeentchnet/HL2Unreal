#pragma once
#include "CoreMinimal.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "HL2BSPImporterFactory.generated.h"

UCLASS()
class HL2BSPIMPORTER_API UHL2BSPImporterFactory : public UFactory, public FReimportHandler
{
    GENERATED_BODY()
public:
    UHL2BSPImporterFactory();
    virtual UObject* FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName,
                                       EObjectFlags Flags, const FString& Filename, const TCHAR* Parms,
                                       FFeedbackContext* Warn, bool& bOutOperationCanceled) override;
    virtual bool FactoryCanImport(const FString& Filename) override;

    virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
    virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Obj) override;
    virtual int32 GetPriority() const override;
};
