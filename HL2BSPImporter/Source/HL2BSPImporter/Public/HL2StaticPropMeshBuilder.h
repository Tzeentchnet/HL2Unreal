#pragma once
#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"

class UStaticMesh;
class UMaterialInterface;
class UObject;
class UHL2BSPImporterSettings;

namespace HL2Studio { struct FStudioFile; }
namespace HL2Mat    { class  FBuilder;     }

namespace HL2Studio
{
    /**
     * Synthesise a `UStaticMesh` from a parsed FStudioFile.
     *
     * Creates one polygon group per Source `mstudiomesh_t`, named after the
     * resolved material slot (lower-case, forward-slashed Source path). The
     * Source coordinate transform from the BSP importer (Y/Z swap + Y negate
     * + WorldScale) is applied to positions and normals; tangents are
     * recomputed via MikkTSpace. LOD chain is left to Unreal's auto-LOD or
     * Nanite.
     *
     * Bodygroup scope: when `BodyMask == 0` (default) emits only `Models[0]` of every
     * bodypart (the default bodygroup state, used by `prop_static`). For non-zero masks
     * (driven by the `body` / `bodygroup` keyvalue on `prop_dynamic` / `prop_physics`),
     * the per-bodypart model index is decoded as
     * `idx = (BodyMask / base[bp]) % BodyParts[bp].Models.Num()`, where
     * `base[0] = 1` and `base[bp+1] = base[bp] * BodyParts[bp].Models.Num()`. Out-of-
     * range computed indices fall back to `Models[0]` for that bodypart.
     *
     * Skin: when `SkinIndex` is in range and the model carries a skin table,
     * each `mstudiomesh_t::material` ref-slot is remapped through
     * `FStudioFile::Skin.SkinFamilies[SkinIndex]` before resolving the
     * material. Out-of-range / missing skin tables fall back to identity (the
     * raw mesh.MaterialIndex is used as a direct texture index).
     *
     * Materials are resolved per polygon-group slot via:
     *   1. MaterialBuilder->GetOrCreateMaterial(candidate) for each candidate
     *      path on the FStudioMesh (most-likely first), stopping at the first
     *      hit.
     *   2. Engine default surface material as fallback.
     *
     * @param Studio        Parsed model.
     * @param Settings      Coordinate / Nanite settings.
     * @param MaterialBuilder  Re-uses the BSP-shared MIC/Texture cache so a
     *                         prop sharing a VMT with worldspawn produces one
     *                         MIC, not two.
     * @param Outer         UPackage / UObject that owns the new asset.
     * @param Name          Asset name (also the package leaf name).
     * @param Flags         Object flags from the import call.
     * @param SkinIndex     Skin family to bake (0 = default). Out-of-range
     *                      values silently fall back to identity skin 0.
     * @param BodyMask      Source `body` keyvalue (0 = default bodygroup).
     * @return  The created UStaticMesh, or null on failure.
     */
    HL2BSPIMPORTER_API UStaticMesh* BuildStaticMesh(
        const FStudioFile& Studio,
        const UHL2BSPImporterSettings* Settings,
        HL2Mat::FBuilder& MaterialBuilder,
        UObject* Outer,
        FName Name,
        EObjectFlags Flags,
        int32 SkinIndex = 0,
        int32 BodyMask = 0);
}
