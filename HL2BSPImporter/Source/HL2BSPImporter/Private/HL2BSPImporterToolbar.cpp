// Editor toolbar + bulk-import menu (Tranche B1).
//
// Registers a "BSP Importer" combo button on the level-editor toolbar. The
// menu contains three actions:
//
//   Asset Import
//     - Bulk Import Textures   (.vtf -> UHL2VTFFactory)
//     - Bulk Import Materials  (.vmt -> UHL2VMTFactory)
//
//   Map Import
//     - Import BSP             (single .bsp -> UHL2BSPImporterFactory)
//
// Bulk handlers prompt for a source directory (typically a Source `materials/`
// parent folder), recursively scan for files with the relevant extension, then
// route each file to a per-file destination derived from the path's
// `materials/<sub>/...` layout under `<SynthesizedAssetRoot>/Textures` or
// `/Materials`. Per-directory progress is reported via FScopedSlowTask.

#include "HL2BSPImporterToolbar.h"
#include "HL2BSPImporter.h"
#include "HL2AssetFactories.h"
#include "HL2BSPImporterFactory.h"
#include "HL2BSPImporterSettings.h"

#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "IDesktopPlatform.h"
#include "DesktopPlatformModule.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Brushes/SlateVectorImageBrush.h"
#include "Templates/UnrealTemplate.h"
#include "ToolMenus.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "HL2Toolbar"

namespace HL2Toolbar
{
    namespace
    {
        const FName kOwnerName(TEXT("HL2BSPImporterToolbar"));
        const FName kStyleSetName(TEXT("HL2BSPImporterStyle"));
        const FName kToolbarIconName(TEXT("HL2BSPImporter.MenuIcon"));

        // Slate style set carrying the plugin's custom toolbar SVG. Created in
        // Register(), torn down in Unregister(). Held by TSharedPtr because
        // FSlateStyleRegistry takes a reference rather than ownership.
        TSharedPtr<FSlateStyleSet> GStyleSet;

        void RegisterStyle()
        {
            if (GStyleSet.IsValid()) { return; }
            const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HL2BSPImporter"));
            if (!Plugin.IsValid())
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Toolbar style: plugin descriptor not found; falling back to engine icon."));
                return;
            }

            GStyleSet = MakeShared<FSlateStyleSet>(kStyleSetName);
            GStyleSet->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));

            const FString IconAbs = GStyleSet->RootToContentDir(TEXT("Icons/HL2BSPImporter"), TEXT(".svg"));
            if (!FPaths::FileExists(IconAbs))
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Toolbar style: SVG missing at '%s'; falling back to engine icon."), *IconAbs);
                GStyleSet.Reset();
                return;
            }

            const FVector2D IconSize20(20.0f, 20.0f);
            GStyleSet->Set(kToolbarIconName, new FSlateVectorImageBrush(IconAbs, IconSize20));

            FSlateStyleRegistry::RegisterSlateStyle(*GStyleSet);
        }

        void UnregisterStyle()
        {
            if (!GStyleSet.IsValid()) { return; }
            FSlateStyleRegistry::UnRegisterSlateStyle(*GStyleSet);
            GStyleSet.Reset();
        }

        FSlateIcon GetToolbarIcon()
        {
            if (GStyleSet.IsValid())
            {
                return FSlateIcon(GStyleSet->GetStyleSetName(), kToolbarIconName);
            }
            return FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import");
        }

        FString PromptForDirectory(const FText& Title)
        {
            FString Out;
            if (!FSlateApplication::IsInitialized()) { return Out; }
            IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
            if (!Desktop) { return Out; }
            const TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
            const void* ParentHandle = (ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid())
                ? ParentWindow->GetNativeWindow()->GetOSWindowHandle()
                : nullptr;
            if (!Desktop->OpenDirectoryDialog(ParentHandle, Title.ToString(), TEXT(""), Out))
            {
                Out.Reset();
            }
            return Out;
        }

        FString PromptForBSPFile(const FText& Title)
        {
            if (!FSlateApplication::IsInitialized()) { return FString(); }
            IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
            if (!Desktop) { return FString(); }
            const TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
            const void* ParentHandle = (ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid())
                ? ParentWindow->GetNativeWindow()->GetOSWindowHandle()
                : nullptr;
            TArray<FString> OpenFilenames;
            const FString FileTypes(TEXT("Half-Life 2 BSP (*.bsp)|*.bsp"));
            const uint32 SelectionFlag = 0;
            if (Desktop->OpenFileDialog(ParentHandle, Title.ToString(), TEXT(""), TEXT(""), FileTypes, SelectionFlag, OpenFilenames)
                && OpenFilenames.Num() > 0)
            {
                return OpenFilenames[0];
            }
            return FString();
        }

        // Walk a normalised absolute path looking for a `/materials/` segment
        // (case-insensitive). Mirror of DeriveSourceKeyAndRoot in HL2AssetFactories.cpp;
        // duplicated here to keep the toolbar self-contained.
        bool DeriveKeyAndSub(const FString& AbsFilenameIn, FString& OutKey, FString& OutSub, FString& OutLeaf)
        {
            FString Norm = AbsFilenameIn;
            Norm.ReplaceInline(TEXT("\\"), TEXT("/"));
            const FString NormLower = Norm.ToLower();
            const TCHAR* Needle = TEXT("/materials/");
            const int32 Pos = NormLower.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            if (Pos == INDEX_NONE) { return false; }
            FString After = Norm.RightChop(Pos + 11);
            const int32 Dot = After.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
            if (Dot != INDEX_NONE) { After.LeftInline(Dot, EAllowShrinking::No); }
            OutKey = After.ToLower();
            int32 SlashIdx = INDEX_NONE;
            if (OutKey.FindLastChar(TEXT('/'), SlashIdx))
            {
                OutSub  = OutKey.Left(SlashIdx);
                OutLeaf = OutKey.RightChop(SlashIdx + 1);
            }
            else
            {
                OutSub.Reset();
                OutLeaf = OutKey;
            }
            return !OutLeaf.IsEmpty();
        }

        // Same shape as DeriveKeyAndSub but anchors on `/models/`. Used by the
        // Bulk Import Models handler so .mdl files land under
        // `<SynthesizedAssetRoot>/Props/<sub>` per the prop-mesh asset layout.
        bool DeriveModelKeyAndSub(const FString& AbsFilenameIn, FString& OutKey, FString& OutSub, FString& OutLeaf)
        {
            FString Norm = AbsFilenameIn;
            Norm.ReplaceInline(TEXT("\\"), TEXT("/"));
            const FString NormLower = Norm.ToLower();
            const TCHAR* Needle = TEXT("/models/");
            const int32 Pos = NormLower.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            if (Pos == INDEX_NONE) { return false; }
            FString After = Norm.RightChop(Pos + 8);
            const int32 Dot = After.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
            if (Dot != INDEX_NONE) { After.LeftInline(Dot, EAllowShrinking::No); }
            OutKey = After.ToLower();
            int32 SlashIdx = INDEX_NONE;
            if (OutKey.FindLastChar(TEXT('/'), SlashIdx))
            {
                OutSub  = OutKey.Left(SlashIdx);
                OutLeaf = OutKey.RightChop(SlashIdx + 1);
            }
            else
            {
                OutSub.Reset();
                OutLeaf = OutKey;
            }
            return !OutLeaf.IsEmpty();
        }

        // Sanitise a path segment (mirror of HL2MaterialBuilder::SanitizePathSegment).
        FString Sanitise(const FString& In)
        {
            FString Out; Out.Reserve(In.Len());
            for (TCHAR Ch : In)
            {
                if ((Ch >= TEXT('a') && Ch <= TEXT('z')) ||
                    (Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
                    (Ch >= TEXT('0') && Ch <= TEXT('9')) ||
                    Ch == TEXT('_') || Ch == TEXT('-') || Ch == TEXT('/'))
                {
                    Out.AppendChar(Ch);
                }
                else
                {
                    Out.AppendChar(TEXT('_'));
                }
            }
            return Out;
        }

        // Compose `<SynthesizedAssetRoot>/<Subfolder>/<sub>` plus the leaf name
        // derived from a Source-engine path. SynthesizedAssetRoot is read from
        // settings; if invalid (not /Game/...) returns false.
        bool ComputeDestination(const FString& AbsFile, const TCHAR* Subfolder,
                                FString& OutPackagePath, FString& OutAssetName)
        {
            const UHL2BSPImporterSettings* Settings = GetDefault<UHL2BSPImporterSettings>();
            if (!Settings || Settings->SynthesizedAssetRoot.IsEmpty() ||
                !Settings->SynthesizedAssetRoot.StartsWith(TEXT("/Game/")))
            {
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Bulk import: SynthesizedAssetRoot is invalid; skipping '%s'"), *AbsFile);
                return false;
            }

            FString Key, Sub, Leaf;
            if (DeriveKeyAndSub(AbsFile, Key, Sub, Leaf))
            {
                OutPackagePath = Settings->SynthesizedAssetRoot / Subfolder / Sanitise(Sub);
                while (OutPackagePath.EndsWith(TEXT("/"))) { OutPackagePath.LeftChopInline(1, EAllowShrinking::No); }
                OutAssetName = Sanitise(Leaf);
            }
            else
            {
                OutPackagePath = Settings->SynthesizedAssetRoot / Subfolder;
                OutAssetName   = Sanitise(FPaths::GetBaseFilename(AbsFile).ToLower());
            }
            return !OutAssetName.IsEmpty();
        }

        // Same shape as ComputeDestination but anchors on the path's `/models/`
        // segment instead of `/materials/`. Output destination subfolder is the
        // `Props` tree (matching the BSP-importer prop-mesh asset layout).
        bool ComputeModelDestination(const FString& AbsFile,
                                     FString& OutPackagePath, FString& OutAssetName)
        {
            const UHL2BSPImporterSettings* Settings = GetDefault<UHL2BSPImporterSettings>();
            if (!Settings || Settings->SynthesizedAssetRoot.IsEmpty() ||
                !Settings->SynthesizedAssetRoot.StartsWith(TEXT("/Game/")))
            {
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Bulk import: SynthesizedAssetRoot is invalid; skipping '%s'"), *AbsFile);
                return false;
            }

            FString Key, Sub, Leaf;
            if (DeriveModelKeyAndSub(AbsFile, Key, Sub, Leaf))
            {
                OutPackagePath = Settings->SynthesizedAssetRoot / TEXT("Props") / Sanitise(Sub);
                while (OutPackagePath.EndsWith(TEXT("/"))) { OutPackagePath.LeftChopInline(1, EAllowShrinking::No); }
                OutAssetName = Sanitise(Leaf);
            }
            else
            {
                OutPackagePath = Settings->SynthesizedAssetRoot / TEXT("Props");
                OutAssetName   = Sanitise(FPaths::GetBaseFilename(AbsFile).ToLower());
            }
            return !OutAssetName.IsEmpty();
        }

        // Save every package on the dirty list (synchronously, no prompt).
        void SaveDirtyPackages(const TSet<UPackage*>& Pkgs)
        {
            if (Pkgs.Num() == 0) { return; }
            TArray<UPackage*> List = Pkgs.Array();
            FEditorFileUtils::PromptForCheckoutAndSave(List, /*bCheckDirty*/ false, /*bPromptToSave*/ false);
        }
    } // namespace

    // ----- Public scan / bulk-import helpers -----

    void ScanDirectoryForExtension(const FString& AbsRoot, const TCHAR* Extension, TArray<FString>& OutAbsFiles)
    {
        OutAbsFiles.Reset();
        if (AbsRoot.IsEmpty() || !Extension) { return; }
        const FString Pattern = FString::Printf(TEXT("*.%s"), Extension);
        IFileManager::Get().FindFilesRecursive(OutAbsFiles, *AbsRoot, *Pattern, /*Files*/ true, /*Directories*/ false);
        OutAbsFiles.Sort();
    }

    int32 BulkImportVTF(const TArray<FString>& AbsFiles)
    {
        if (AbsFiles.Num() == 0) { return 0; }

        UHL2VTFFactory* Factory = NewObject<UHL2VTFFactory>();
        Factory->AddToRoot();
        ON_SCOPE_EXIT { Factory->RemoveFromRoot(); };

        FScopedSlowTask Task((float)AbsFiles.Num(),
            LOCTEXT("BulkImportVTF", "Bulk-importing Source textures..."));
        Task.MakeDialog();

        int32 Imported = 0;
        TSet<UPackage*> DirtyPkgs;

        for (const FString& AbsFile : AbsFiles)
        {
            Task.EnterProgressFrame(1.f, FText::FromString(FPaths::GetCleanFilename(AbsFile)));

            FString PkgPath, AssetName;
            if (!ComputeDestination(AbsFile, TEXT("Textures"), PkgPath, AssetName)) { continue; }

            TArray<uint8> Bytes;
            if (!FFileHelper::LoadFileToArray(Bytes, *AbsFile))
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Bulk VTF: failed to read '%s'"), *AbsFile);
                continue;
            }

            UPackage* Pkg = CreatePackage(*(PkgPath / AssetName));
            if (!Pkg)
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Bulk VTF: CreatePackage failed for '%s/%s'"), *PkgPath, *AssetName);
                continue;
            }
            Pkg->FullyLoad();

            const uint8* Buffer = Bytes.GetData();
            const uint8* BufferEnd = Buffer + Bytes.Num();
            Factory->SetCurrentFilename(AbsFile);
            UObject* Result = Factory->FactoryCreateBinary(
                UTexture::StaticClass(), Pkg, FName(*AssetName),
                RF_Public | RF_Standalone | RF_Transactional,
                /*Context*/ nullptr, TEXT("vtf"),
                Buffer, BufferEnd, GWarn);
            Factory->SetCurrentFilename(FString());

            if (Result)
            {
                ++Imported;
                DirtyPkgs.Add(Pkg);
            }
        }

        SaveDirtyPackages(DirtyPkgs);
        UE_LOG(LogHL2BSPImporter, Log, TEXT("Bulk VTF import: %d/%d files imported."), Imported, AbsFiles.Num());
        return Imported;
    }

    int32 BulkImportVMT(const TArray<FString>& AbsFiles)
    {
        if (AbsFiles.Num() == 0) { return 0; }

        UHL2VMTFactory* Factory = NewObject<UHL2VMTFactory>();
        Factory->AddToRoot();
        ON_SCOPE_EXIT { Factory->RemoveFromRoot(); };

        FScopedSlowTask Task((float)AbsFiles.Num(),
            LOCTEXT("BulkImportVMT", "Bulk-importing Source materials..."));
        Task.MakeDialog();

        int32 Imported = 0;
        TSet<UPackage*> DirtyPkgs;

        for (const FString& AbsFile : AbsFiles)
        {
            Task.EnterProgressFrame(1.f, FText::FromString(FPaths::GetCleanFilename(AbsFile)));

            FString PkgPath, AssetName;
            if (!ComputeDestination(AbsFile, TEXT("Materials"), PkgPath, AssetName)) { continue; }

            FString Source;
            if (!FFileHelper::LoadFileToString(Source, *AbsFile))
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Bulk VMT: failed to read '%s'"), *AbsFile);
                continue;
            }

            UPackage* Pkg = CreatePackage(*(PkgPath / AssetName));
            if (!Pkg)
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Bulk VMT: CreatePackage failed for '%s/%s'"), *PkgPath, *AssetName);
                continue;
            }
            Pkg->FullyLoad();

            const TCHAR* Buffer    = *Source;
            const TCHAR* BufferEnd = Buffer + Source.Len();
            Factory->SetCurrentFilename(AbsFile);
            UObject* Result = Factory->FactoryCreateText(
                UMaterialInstanceConstant::StaticClass(), Pkg, FName(*AssetName),
                RF_Public | RF_Standalone | RF_Transactional,
                /*Context*/ nullptr, TEXT("vmt"),
                Buffer, BufferEnd, GWarn);
            Factory->SetCurrentFilename(FString());

            if (Result)
            {
                ++Imported;
                DirtyPkgs.Add(Pkg);
                if (UPackage* TextureOuter = Result->GetOutermost())
                {
                    DirtyPkgs.Add(TextureOuter);
                }
            }
        }

        SaveDirtyPackages(DirtyPkgs);
        UE_LOG(LogHL2BSPImporter, Log, TEXT("Bulk VMT import: %d/%d files imported."), Imported, AbsFiles.Num());
        return Imported;
    }

    int32 BulkImportMDL(const TArray<FString>& AbsFiles)
    {
        if (AbsFiles.Num() == 0) { return 0; }

        UHL2MDLFactory* Factory = NewObject<UHL2MDLFactory>();
        Factory->AddToRoot();
        ON_SCOPE_EXIT { Factory->RemoveFromRoot(); };

        FScopedSlowTask Task((float)AbsFiles.Num(),
            LOCTEXT("BulkImportMDL", "Bulk-importing Source models..."));
        Task.MakeDialog();

        int32 Imported = 0;
        TSet<UPackage*> DirtyPkgs;

        for (const FString& AbsFile : AbsFiles)
        {
            Task.EnterProgressFrame(1.f, FText::FromString(FPaths::GetCleanFilename(AbsFile)));

            FString PkgPath, AssetName;
            if (!ComputeModelDestination(AbsFile, PkgPath, AssetName)) { continue; }

            UPackage* Pkg = CreatePackage(*(PkgPath / AssetName));
            if (!Pkg)
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Bulk MDL: CreatePackage failed for '%s/%s'"), *PkgPath, *AssetName);
                continue;
            }
            Pkg->FullyLoad();

            // The standalone .mdl factory pivots from the path (it needs the
            // .vvd / .dx90.vtx siblings on disk), so the byte buffer is
            // unused. Pass a sentinel pair to satisfy the binary-factory
            // contract.
            const uint8 Sentinel = 0;
            const uint8* Buffer    = &Sentinel;
            const uint8* BufferEnd = Buffer;
            Factory->SetCurrentFilename(AbsFile);
            UObject* Result = Factory->FactoryCreateBinary(
                UStaticMesh::StaticClass(), Pkg, FName(*AssetName),
                RF_Public | RF_Standalone | RF_Transactional,
                /*Context*/ nullptr, TEXT("mdl"),
                Buffer, BufferEnd, GWarn);
            Factory->SetCurrentFilename(FString());

            if (Result)
            {
                ++Imported;
                DirtyPkgs.Add(Pkg);
                if (UPackage* MeshOuter = Result->GetOutermost())
                {
                    DirtyPkgs.Add(MeshOuter);
                }
            }
        }

        SaveDirtyPackages(DirtyPkgs);
        UE_LOG(LogHL2BSPImporter, Log, TEXT("Bulk MDL import: %d/%d files imported."), Imported, AbsFiles.Num());
        return Imported;
    }

    namespace
    {
        // ----- Menu actions -----

        void OnBulkImportTextures()
        {
            const FString Dir = PromptForDirectory(LOCTEXT("PickVTFDir", "Pick a Source content folder containing materials/*.vtf"));
            if (Dir.IsEmpty()) { return; }
            TArray<FString> Files;
            ScanDirectoryForExtension(Dir, TEXT("vtf"), Files);
            if (Files.Num() == 0)
            {
                UE_LOG(LogHL2BSPImporter, Log, TEXT("Bulk VTF: no .vtf files found under '%s'"), *Dir);
                return;
            }
            BulkImportVTF(Files);
        }

        void OnBulkImportMaterials()
        {
            const FString Dir = PromptForDirectory(LOCTEXT("PickVMTDir", "Pick a Source content folder containing materials/*.vmt"));
            if (Dir.IsEmpty()) { return; }
            TArray<FString> Files;
            ScanDirectoryForExtension(Dir, TEXT("vmt"), Files);
            if (Files.Num() == 0)
            {
                UE_LOG(LogHL2BSPImporter, Log, TEXT("Bulk VMT: no .vmt files found under '%s'"), *Dir);
                return;
            }
            BulkImportVMT(Files);
        }

        void OnBulkImportModels()
        {
            const FString Dir = PromptForDirectory(LOCTEXT("PickMDLDir", "Pick a Source content folder containing models/*.mdl"));
            if (Dir.IsEmpty()) { return; }
            TArray<FString> Files;
            ScanDirectoryForExtension(Dir, TEXT("mdl"), Files);
            if (Files.Num() == 0)
            {
                UE_LOG(LogHL2BSPImporter, Log, TEXT("Bulk MDL: no .mdl files found under '%s'"), *Dir);
                return;
            }
            BulkImportMDL(Files);
        }

        void OnImportBSP()
        {
            const FString File = PromptForBSPFile(LOCTEXT("PickBSPFile", "Pick a Half-Life 2 .bsp map to import"));
            if (File.IsEmpty()) { return; }

            FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
            const UHL2BSPImporterSettings* Settings = GetDefault<UHL2BSPImporterSettings>();
            const FString DestPath = (Settings && !Settings->SynthesizedAssetRoot.IsEmpty() &&
                                       Settings->SynthesizedAssetRoot.StartsWith(TEXT("/Game/")))
                ? Settings->SynthesizedAssetRoot / TEXT("Maps")
                : FString(TEXT("/Game/HL2/Imported/Maps"));

            TArray<FString> Files = { File };
            AssetTools.Get().ImportAssets(Files, DestPath);
        }

        // ----- Menu construction -----

        TSharedRef<SWidget> MakeMenu()
        {
            FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection*/ true, nullptr);

            Menu.BeginSection("AssetImport", LOCTEXT("SectionAssetImport", "Asset Import"));
            {
                Menu.AddMenuEntry(
                    LOCTEXT("BulkImportTextures",        "Bulk Import Textures (.vtf)"),
                    LOCTEXT("BulkImportTexturesTooltip", "Recursively scan a folder for .vtf files and import them as UTexture2D / UTextureCube assets at <SynthesizedAssetRoot>/Textures/<sub>/<file>."),
                    FSlateIcon(FAppStyle::GetAppStyleSetName(), "ContentBrowser.AssetActions.Import"),
                    FUIAction(FExecuteAction::CreateStatic(&OnBulkImportTextures)));

                Menu.AddMenuEntry(
                    LOCTEXT("BulkImportMaterials",        "Bulk Import Materials (.vmt)"),
                    LOCTEXT("BulkImportMaterialsTooltip", "Recursively scan a folder for .vmt files and import them as UMaterialInstanceConstant assets at <SynthesizedAssetRoot>/Materials/<sub>/<file>. Referenced .vtf textures are imported as siblings on demand."),
                    FSlateIcon(FAppStyle::GetAppStyleSetName(), "ContentBrowser.AssetActions.Import"),
                    FUIAction(FExecuteAction::CreateStatic(&OnBulkImportMaterials)));

                Menu.AddMenuEntry(
                    LOCTEXT("BulkImportModels",        "Bulk Import Models (.mdl)"),
                    LOCTEXT("BulkImportModelsTooltip", "Recursively scan a folder for .mdl files and import them as UStaticMesh assets at <SynthesizedAssetRoot>/Props/<sub>/<file>. Each .mdl is built from its .vvd + .dx90.vtx siblings; referenced materials/textures are imported as siblings on demand. Skin / bodygroup variants are not surfaced — drive those through a BSP entity import."),
                    FSlateIcon(FAppStyle::GetAppStyleSetName(), "ContentBrowser.AssetActions.Import"),
                    FUIAction(FExecuteAction::CreateStatic(&OnBulkImportModels)));
            }
            Menu.EndSection();

            Menu.BeginSection("MapImport", LOCTEXT("SectionMapImport", "Map Import"));
            {
                Menu.AddMenuEntry(
                    LOCTEXT("ImportBSP",        "Import BSP..."),
                    LOCTEXT("ImportBSPTooltip", "Pick a .bsp file and run it through the full HL2 BSP importer (parity with Content Browser drag-drop)."),
                    FSlateIcon(FAppStyle::GetAppStyleSetName(), "ContentBrowser.AssetActions.Import"),
                    FUIAction(FExecuteAction::CreateStatic(&OnImportBSP)));
            }
            Menu.EndSection();

            return Menu.MakeWidget();
        }

        void RegisterMenus()
        {
            FToolMenuOwnerScoped OwnerScoped(kOwnerName);

            UToolMenu* ToolBar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
            if (!ToolBar)
            {
                // Fallback: older UE menu name.
                ToolBar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar");
            }
            if (!ToolBar) { return; }

            FToolMenuSection& Section = ToolBar->FindOrAddSection("HL2BSPImporter");

            FToolMenuEntry Entry = FToolMenuEntry::InitComboButton(
                "HL2BSPImporter",
                FUIAction(),
                FOnGetContent::CreateStatic(&MakeMenu),
                LOCTEXT("HL2ToolbarLabel",   "BSP Importer"),
                LOCTEXT("HL2ToolbarTooltip", "Half-Life 2 / Source asset and map importer"),
                GetToolbarIcon(),
                /*bInSimpleComboBox*/ false);
            Section.AddEntry(Entry);
        }
    } // namespace

    void Register()
    {
        if (!FSlateApplication::IsInitialized()) { return; }
        RegisterStyle();
        UToolMenus::RegisterStartupCallback(
            FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenus));
    }

    void Unregister()
    {
        UToolMenus::UnRegisterStartupCallback(kOwnerName);
        UToolMenus::UnregisterOwner(kOwnerName);
        UnregisterStyle();
    }
}

#undef LOCTEXT_NAMESPACE
