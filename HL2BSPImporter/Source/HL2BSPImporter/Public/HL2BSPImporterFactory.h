#pragma once
#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "HL2BSPImporterFactory.generated.h"

UCLASS()
class HL2BSPIMPORTER_API UHL2BSPImporterFactory : public UFactory
{
    GENERATED_BODY()
public:
    UHL2BSPImporterFactory();
    virtual UObject* FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName,
                                       EObjectFlags Flags, const FString& Filename, const TCHAR* Parms,
                                       FFeedbackContext* Warn, bool& bOutOperationCanceled) override;
    virtual bool FactoryCanImport(const FString& Filename) override;
};
