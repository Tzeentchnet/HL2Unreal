#pragma once
#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "EditorReimportHandler.h"
#include "HL2AssetFactories.generated.h"

// Standalone Content-Browser factories for Half-Life 2 source assets.
// These complement UHL2BSPImporterFactory: where the BSP factory ingests an
// entire .bsp and synthesises every referenced material/texture as a side
// effect, these factories let the user pre-stage individual .vtf / .vmt files
// (or run them through the Tranche B bulk-import toolbar) without ever
// importing a map.

class UMaterialInterface;
class UTexture;
class UTexture2D;
class UTextureCube;
class UStaticMesh;

// .vtf -> UTexture2D (or UTextureCube when the file is an envmap with >= 6 faces).
//
// Supports v7.0–7.5, mip 0 only (the engine generates its own mip chain),
// frame 0 of multi-frame textures. Cubemaps decode all six faces; the optional
// 7th spheremap face on v7.0–7.4 envmaps is ignored. Re-import preserves the
// user's manually-tweaked LODGroup / SRGB / Compression / AdjustBrightness /
// Address / Filter settings on the existing UTexture asset.
UCLASS()
class HL2BSPIMPORTER_API UHL2VTFFactory : public UFactory, public FReimportHandler
{
    GENERATED_BODY()
public:
    UHL2VTFFactory();

    virtual UObject* FactoryCreateBinary(
        UClass* InClass, UObject* InParent, FName InName,
        EObjectFlags Flags, UObject* Context, const TCHAR* Type,
        const uint8*& Buffer, const uint8* BufferEnd,
        FFeedbackContext* Warn) override;

    virtual bool FactoryCanImport(const FString& Filename) override;

    virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
    virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Obj) override;
    virtual int32 GetPriority() const override;
};

// .vmt -> UMaterialInstanceConstant.
//
// Locates the file in the Source content tree (using the .vmt's path to
// derive the lookup key under `materials/`), parses it via HL2VmtParser,
// resolves any `Patch` indirection, and synthesises a MIC parented to the
// shader-family-appropriate master material exactly as the BSP factory does.
// Referenced textures are imported as siblings on demand. Re-import refreshes
// the MIC parameters from the (potentially edited) source .vmt.
UCLASS()
class HL2BSPIMPORTER_API UHL2VMTFactory : public UFactory, public FReimportHandler
{
    GENERATED_BODY()
public:
    UHL2VMTFactory();

    virtual UObject* FactoryCreateText(
        UClass* InClass, UObject* InParent, FName InName,
        EObjectFlags Flags, UObject* Context, const TCHAR* Type,
        const TCHAR*& Buffer, const TCHAR* BufferEnd,
        FFeedbackContext* Warn) override;

    virtual bool FactoryCanImport(const FString& Filename) override;

    virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
    virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Obj) override;
    virtual int32 GetPriority() const override;
};

// .mdl -> UStaticMesh (LOD-0, default bodygroup, skin 0).
//
// Locates the .mdl + .vvd + .dx90.vtx triple via the file's `models/`
// segment-derived content root (no per-import settings required for a
// single-file import), runs the same HL2Studio::LoadModel +
// HL2Studio::BuildStaticMesh pipeline as the BSP-driven prop synthesis pass,
// and routes referenced materials/textures through HL2Mat::FBuilder so they
// land at the canonical `<SynthesizedAssetRoot>/Materials|Textures/...` paths
// and are shared with any concurrent BSP-driven import. Skin / bodygroup
// variants are not surfaced on the standalone factory; users wanting non-
// default variants should drive the BSP-importer entity table or invoke the
// builder directly.
UCLASS()
class HL2BSPIMPORTER_API UHL2MDLFactory : public UFactory, public FReimportHandler
{
    GENERATED_BODY()
public:
    UHL2MDLFactory();

    virtual UObject* FactoryCreateBinary(
        UClass* InClass, UObject* InParent, FName InName,
        EObjectFlags Flags, UObject* Context, const TCHAR* Type,
        const uint8*& Buffer, const uint8* BufferEnd,
        FFeedbackContext* Warn) override;

    virtual bool FactoryCanImport(const FString& Filename) override;

    virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
    virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Obj) override;
    virtual int32 GetPriority() const override;
};
