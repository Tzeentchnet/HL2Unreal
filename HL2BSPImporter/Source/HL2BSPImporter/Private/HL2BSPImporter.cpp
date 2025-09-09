// Minimal module implementation for the HL2BSPImporter editor module
#include "HL2BSPImporter.h" // Must be first
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogHL2BSPImporter);

class FHL2BSPImporterModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FHL2BSPImporterModule, HL2BSPImporter)

