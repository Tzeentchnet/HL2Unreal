using UnrealBuildTool;

public class HL2BSPImporter : ModuleRules
{
    public HL2BSPImporter(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(
            new string[] { "Core", "CoreUObject", "Engine", "UnrealEd",
                           "AssetTools", "Projects", "RenderCore", "RHI",
                           "RawMesh", "StaticMeshDescription", "MeshDescription",
                           "AssetRegistry", "Json", "JsonUtilities" });
    }
}