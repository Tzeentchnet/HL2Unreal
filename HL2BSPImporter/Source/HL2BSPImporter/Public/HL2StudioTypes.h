#pragma once
#include "CoreMinimal.h"

// Shared raw types produced by the MDL / VVD / VTX readers. None of these are
// USTRUCTs — they live entirely in import-time memory and are consumed by the
// static-prop mesh builder which produces UStaticMesh assets.
//
// Coordinates are kept in raw Source space (right-handed, Z-up, inches). The
// builder applies the same Source→Unreal transform used elsewhere in the
// importer (see TransformPos/TransformDir in HL2BSPImporterFactory.cpp).
//
// LOD scope: the readers extract LOD 0 only. Source models may carry up to 8
// LODs but the LOD chain is largely an optimisation Unreal will replace via
// auto-LOD or Nanite; honouring Source's hand-authored LODs is left for a
// future phase.

namespace HL2Studio
{
    // One vertex in a model's LOD-0 vertex stream. Position/Normal are Source
    // coordinates; Tangent's W carries handedness (±1) per Source convention.
    struct FStudioVertex
    {
        FVector   Position = FVector::ZeroVector;   // Source coords (inches)
        FVector   Normal   = FVector::ZeroVector;   // Source coords, unit length
        FVector2D UV       = FVector2D::ZeroVector;
        FVector4  Tangent  = FVector4(1.f, 0.f, 0.f, 1.f);
    };

    // One mesh inside a model. A mesh is the granularity of material assignment:
    // every triangle in a mesh shares one material index (resolved via the parent
    // file's MaterialPath candidate list).
    struct FStudioMesh
    {
        int32 MaterialIndex = 0;          // index into FStudioFile::MaterialPaths
        int32 VertexOffset  = 0;          // first vertex of this mesh, relative to its model's
                                          // vertex base (in vertices, not bytes)
        int32 NumVertices   = 0;          // mesh-local vertex count
        TArray<uint32> TriangleIndices;   // mesh-local indices (0..NumVertices-1), length = 3*numTri
    };

    // One model inside a body part. A body part holds N alternative models that
    // form one bodygroup choice (e.g. a citizen's `head` bodypart carries one
    // model per head variant). The default bodygroup state is "model 0 of every
    // bodypart" — the static-prop builder honours this by emitting only
    // Models[0] per bodypart, since `prop_static` instances carry no bodygroup
    // mask. Other variants are kept on disk for a future per-bodygroup build
    // path (e.g. driven from `prop_dynamic` keyvalues).
    struct FStudioModel
    {
        FString Name;
        int32   VertexBase  = 0;          // first vertex of this model in the LOD-0 vertex array
        int32   NumVertices = 0;
        TArray<FStudioMesh> Meshes;
    };

    struct FStudioBodyPart
    {
        FString Name;
        TArray<FStudioModel> Models;
    };

    // Source skin table. `mstudiomesh_t::material` is an index into a per-mesh
    // *reference* slot (0..numskinref-1), and the skin table remaps that slot
    // to a real `mstudiotexture_t` index per skin family. Identity for skin 0
    // on most props, but materials swap (e.g. Combine soldier variants, prop
    // damage states) for higher skins. Layout: `SkinFamilies[family][refSlot] = textureIdx`.
    // Empty when the model has no skin table — caller treats mesh.MaterialIndex
    // as a direct texture index in that case.
    struct FStudioSkinTable
    {
        int32 NumSkinRef      = 0;        // ref-slot count = inner-array length
        int32 NumSkinFamilies = 0;        // outer-array length
        TArray<TArray<int32>> SkinFamilies;
    };

    // Top-level result of parsing an MDL+VVD+VTX triple.
    //
    // Materials: each entry in MaterialCandidates is a list of `<cdtex>/<name>`
    // (forward-slashed, lower-case) strings to try in priority order against the
    // configured SourceContentRoots. The MDL stores `numcdtextures` candidate
    // directories; the .vmt is whichever combination resolves first.
    struct FStudioFile
    {
        FString MdlPath;
        int32   Version  = 0;             // 44..49 supported
        int32   Checksum = 0;             // shared with VVD + VTX
        int32   Flags    = 0;             // mstudiohdr_t.flags (STUDIOHDR_FLAGS_STATIC_PROP etc.)
        FBox    BBox     = FBox(ForceInit);   // hull_min/max in Source coords

        TArray<FStudioBodyPart>          BodyParts;
        TArray<TArray<FString>>          MaterialCandidates;   // [matIdx][candidate]
        TArray<FStudioVertex>            Vertices;             // LOD-0, post-fixup
        FStudioSkinTable                 Skin;                 // empty when model has no skin remap
    };
}
