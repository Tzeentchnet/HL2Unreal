#pragma once
#include "CoreMinimal.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "HL2SurfaceProp.generated.h"

// Phase A3 — Source per-surface property asset.
//
// Imported from `<SourceContentRoots>/scripts/surfaceproperties.txt` (KV1
// format, parsed via HL2KV). One asset per `<surface_name> { ... }` block.
// Inherits from UPhysicalMaterial so a UStaticMesh / USkeletalMesh body
// setup can reference it directly via PhysMaterial.
//
// Source's `friction` is stored on the parent UPhysicalMaterial::Friction
// slot — the rest are surface-extra slots. The `base` parent reference is
// resolved post-pass to a sibling USurfaceProp soft path.
UCLASS(BlueprintType)
class HL2BSPIMPORTER_API USurfaceProp : public UPhysicalMaterial
{
    GENERATED_BODY()
public:
    // Original Source surface name (lower-cased on import). e.g. "concrete".
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
    FName SourceName;

    // Source's `base` keyvalue: name of the parent surface in the script.
    // Empty when this is a root surface ("default" in HL2's script).
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
    FName SourceParent;

    // Source physical params not covered by UPhysicalMaterial.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Physics")
    float Elasticity = 0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Physics")
    float SourceDensity = 2000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Physics")
    float Dampening = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    float AudioReflectivity = 0.66f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    float AudioHardnessFactor = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    float AudioRoughnessFactor = 1.0f;

    // Sound script names from the `impactSoft`/`impactHard`/`scrape*`/`step*`
    // keyvalues. Resolved against the imported sound script DataTable in a
    // future phase (D1); recorded here verbatim for now.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    FString ImpactSoftSound;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    FString ImpactHardSound;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    FString ScrapeSmoothSound;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    FString ScrapeRoughSound;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    FString StepLeftSound;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source|Audio")
    FString StepRightSound;
};
