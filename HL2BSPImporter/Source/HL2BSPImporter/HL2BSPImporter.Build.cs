using System.IO;
using UnrealBuildTool;

public class HL2BSPImporter : ModuleRules
{
    public HL2BSPImporter(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // Bundled LZMA SDK (decoder only). Header path so HL2Lzma.cpp can include "ThirdParty/Lzma/...".
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

        // Engine-bundled zlib for raw DEFLATE decompression of pakfile entries (HL2PakFile.cpp).
        AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

        PublicDependencyModuleNames.AddRange(
            new string[] {
                "Core",
                "CoreUObject",
                "Engine",
                // UFactory is referenced from a public header (UHL2BSPImporterFactory).
                "UnrealEd",
                "DeveloperSettings"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[] {
                "AssetRegistry",
                "AssetTools",
                "DesktopPlatform",
                "Json",
                "JsonUtilities",
                "MeshDescription",
                "StaticMeshDescription",
                "MeshUtilities",
                "PhysicsCore",
                "Projects",
                "RenderCore",
                "RHI",
                "Slate",
                "SlateCore",
                "ToolMenus"
            });
    }
}
