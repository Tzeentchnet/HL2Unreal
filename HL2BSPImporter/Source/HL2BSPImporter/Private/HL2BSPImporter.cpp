// Module implementation for the HL2BSPImporter editor module.
#include "HL2BSPImporter.h"
#include "HL2BSPImporterToolbar.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogHL2BSPImporter);

class FHL2BSPImporterModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogHL2BSPImporter, Log, TEXT("HL2BSPImporter module loaded"));
        HL2Toolbar::Register();
    }
    virtual void ShutdownModule() override
    {
        HL2Toolbar::Unregister();
        UE_LOG(LogHL2BSPImporter, Log, TEXT("HL2BSPImporter module unloaded"));
    }
};

IMPLEMENT_MODULE(FHL2BSPImporterModule, HL2BSPImporter)

