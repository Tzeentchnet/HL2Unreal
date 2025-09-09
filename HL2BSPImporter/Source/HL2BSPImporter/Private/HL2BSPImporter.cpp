// Minimal module implementation for the HL2BSPImporter editor module
#include "HL2BSPImporter.h" // Must be first
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogHL2BSPImporter);

class FHL2BSPImporterModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogHL2BSPImporter, Log, TEXT("HL2BSPImporter module loaded"));
    }
    virtual void ShutdownModule() override
    {
        UE_LOG(LogHL2BSPImporter, Log, TEXT("HL2BSPImporter module unloaded"));
    }
};

IMPLEMENT_MODULE(FHL2BSPImporterModule, HL2BSPImporter)

