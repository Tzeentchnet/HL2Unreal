#pragma once
#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "HL2BSPImporterFactory.generated.h"

UCLASS()
class UHL2BSPImporterFactory : public UFactory
{
    GENERATED_BODY()
public:
    UHL2BSPImporterFactory();
    virtual UObject* FactoryCreateFile(UClass*, UObject*, FName, const FString&, const TCHAR*,
                                       FFeedbackContext*, bool&) override;
    virtual bool FactoryCanImport(const FString& Filename) override;
};