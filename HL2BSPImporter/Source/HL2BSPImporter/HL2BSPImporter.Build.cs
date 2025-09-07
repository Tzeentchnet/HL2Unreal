using UnrealBuildTool;

public class HL2BSPImporter : ModuleRules
{
    public HL2BSPImporter(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(
            new string[] {
                "Core", "CoreUObject", "Engine",
                // Public because UFactory is referenced in a public header
                "UnrealEd",
                "AssetRegistry",
                "Json", "JsonUtilities",
                "StaticMeshDescription", "MeshDescription",
                "RenderCore", "RHI",
                "AssetTools", "Projects"
            });
    }
}
