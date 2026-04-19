#include "HL2BSPImporterFactory.h"
#include "HL2BSPImporter.h"
#include "BspFile.h"
#include "HL2EntityTable.h"
#include "HL2BSPImporterSettings.h"
#include "HL2MaterialBuilder.h"
#include "HL2PakFile.h"
#include "HL2StudioLoader.h"
#include "HL2StaticPropMeshBuilder.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "UObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/SoftObjectPath.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/FileManager.h"
#include "Misc/FeedbackContext.h"

#define LOCTEXT_NAMESPACE "HL2BSPImporter"

namespace
{
    // Normalize a Source-engine texture path to the canonical lookup key.
    // Source paths are case-insensitive and may use either '\' or '/'.
    FString NormalizeTextureKey(const FString& In)
    {
        FString Out = In.Replace(TEXT("\\"), TEXT("/"));
        Out.ToLowerInline();
        return Out;
    }

    using FMaterialMap = TMap<FString, TObjectPtr<UMaterialInterface>>;

    FMaterialMap LoadMaterialMap(const UHL2BSPImporterSettings* Sets)
    {
        FMaterialMap Map;

        TArray<FString> Candidates;

        auto AddIfFile = [&](const FString& Path)
        {
            if (!Path.IsEmpty() && FPaths::FileExists(Path))
            {
                Candidates.Add(Path);
            }
        };

        if (Sets && !Sets->MaterialJsonPath.IsEmpty())
        {
            FString P = Sets->MaterialJsonPath;
            if (P.StartsWith(TEXT("/Game/")))
            {
                FString Rel = P.RightChop(6);
                if (!Rel.EndsWith(TEXT(".json"))) { Rel += TEXT(".json"); }
                FString Abs = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Rel);
                if (FPaths::FileExists(Abs)) { AddIfFile(Abs); }
                else
                {
                    UE_LOG(LogHL2BSPImporter, Warning,
                        TEXT("MaterialJsonPath points to '/Game/...' but file was not found: %s"), *Abs);
                }
            }
            else
            {
                AddIfFile(P);
            }
        }

        // Fallback to plugin Resources/Materials.json
        if (Candidates.Num() == 0)
        {
            if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HL2BSPImporter")))
            {
                AddIfFile(Plugin->GetBaseDir() / TEXT("Resources/Materials.json"));
            }
        }

        if (Candidates.Num() == 0)
        {
            UE_LOG(LogHL2BSPImporter, Log, TEXT("No material JSON found; using empty material map (default material for every slot)."));
            return Map;
        }

        const FString& Chosen = Candidates[0];
        FString JsonStr;
        if (!FFileHelper::LoadFileToString(JsonStr, *Chosen))
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("Failed to read material JSON: %s"), *Chosen);
            return Map;
        }

        TSharedPtr<FJsonValue> RootValue;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
        if (!FJsonSerializer::Deserialize(Reader, RootValue) || !RootValue.IsValid() || RootValue->Type != EJson::Array)
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("Material JSON is not an array: %s"), *Chosen);
            return Map;
        }

        for (const TSharedPtr<FJsonValue>& V : RootValue->AsArray())
        {
            if (!V.IsValid() || V->Type != EJson::Object) { continue; }
            const TSharedPtr<FJsonObject> Obj = V->AsObject();
            FString TextureName, MaterialPath;
            if (!Obj->TryGetStringField(TEXT("TextureName"),  TextureName))  { continue; }
            if (!Obj->TryGetStringField(TEXT("MaterialPath"), MaterialPath)) { continue; }

            FSoftObjectPath Path(MaterialPath);
            if (UObject* Loaded = Path.TryLoad())
            {
                if (auto* MI = Cast<UMaterialInterface>(Loaded))
                {
                    Map.Add(NormalizeTextureKey(TextureName), MI);
                }
            }
        }
        UE_LOG(LogHL2BSPImporter, Log, TEXT("Loaded %d material mappings from: %s"), Map.Num(), *Chosen);
        return Map;
    }

    // Source XYZ (right-handed, Z-up, inches) -> Unreal XYZ (left-handed, Z-up, centimetres).
    // Standard transform: swap Y/Z if requested, negate Y to flip handedness, scale.
    // Returns the resulting position. Note: this transform is sign(determinant) = -1, so triangle
    // winding must be reversed in the mesh builder to keep faces front-facing.
    FORCEINLINE FVector TransformPos(const FVector& In, const UHL2BSPImporterSettings* Sets)
    {
        FVector P = In;
        if (Sets && Sets->bFlipYZ) { P = FVector(In.X, In.Z, In.Y); }
        P.Y *= -1.f;
        const float Scale = Sets ? Sets->WorldScale : 1.f;
        P *= Scale;
        return P;
    }

    FORCEINLINE FVector TransformDir(const FVector& In, const UHL2BSPImporterSettings* Sets)
    {
        // Direction vectors get the same linear transform as positions; translation does not apply.
        FVector P = In;
        if (Sets && Sets->bFlipYZ) { P = FVector(In.X, In.Z, In.Y); }
        P.Y *= -1.f;
        const float Scale = Sets ? Sets->WorldScale : 1.f;
        P *= Scale;
        return P;
    }

    // The handedness flip means triangle winding must be reversed. Returns true if so.
    FORCEINLINE bool ShouldReverseWinding(const UHL2BSPImporterSettings* /*Sets*/)
    {
        // bFlipYZ flips one axis, Y *= -1 flips another. With both: parity = +1 (no swap needed).
        // Without bFlipYZ: only Y *= -1, parity = -1 (swap needed).
        // We always do Y *= -1; with bFlipYZ we also do a YZ swap.
        // Net parity = -1 in both cases (even number of independent flips for swap is 1, plus 1 negation = 2 -> +1...)
        // To be safe and consistent with reference (which observes inside-out faces), reverse winding.
        return true;
    }

    // Find which corner of the displacement's base quad is closest to its StartPosition.
    // Returns 0..3. Used to rotate the quad so that corner becomes (0,0) of the disp grid.
    int32 FindDispStartCorner(const FVector (&Corners)[4], const FVector& StartPos)
    {
        int32 BestIdx = 0;
        double BestDist = TNumericLimits<double>::Max();
        for (int32 i = 0; i < 4; ++i)
        {
            const double D = FVector::DistSquared(Corners[i], StartPos);
            if (D < BestDist) { BestDist = D; BestIdx = i; }
        }
        return BestIdx;
    }

    struct FBuildResult
    {
        FMeshDescription   MeshDesc;
        TArray<FName>      SlotNames;
        int32              FacesEmitted     = 0;
        int32              DispsEmitted     = 0;
        int32              DispsSkipped     = 0;
        int32              DispSeamClusters = 0; // # of weld clusters (size>=2) snapped together
        int32              DispSeamVertsWelded = 0; // # of perimeter verts moved to a cluster centroid
        int32              DispEdgeSnapsPowerMismatch = 0; // # of intermediate perimeter verts snapped onto a neighbour's lower-power edge segment (T-junction fix)
    };

    FBuildResult BuildMeshDescriptionFromBSP(
        const FBspFile& Bsp,
        const UHL2BSPImporterSettings* Sets,
        int32 FirstFace,
        int32 NumFaces,
        bool bIncludeDisplacements,
        const FVector& OriginOffsetSrc = FVector::ZeroVector)
    {
        FBuildResult R;
        FMeshDescription& MD = R.MeshDesc;
        FStaticMeshAttributes Attrs(MD);
        Attrs.Register();

        TVertexAttributesRef<FVector3f>          VertexPositions  = Attrs.GetVertexPositions();
        TVertexInstanceAttributesRef<FVector3f>  InstanceNormals  = Attrs.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f>  InstanceTangents = Attrs.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float>      InstanceBinormalSigns = Attrs.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector4f>  InstanceColors   = Attrs.GetVertexInstanceColors();
        TVertexInstanceAttributesRef<FVector2f>  InstanceUVs      = Attrs.GetVertexInstanceUVs();
        InstanceUVs.SetNumChannels(2); // 0 = base, 1 = lightmap UVs (also used as src for auto lightmap UV gen)

        TPolygonGroupAttributesRef<FName> PolyGroupSlots = Attrs.GetPolygonGroupMaterialSlotNames();

        TMap<FName, FPolygonGroupID> PolyGroups;
        auto GetOrCreatePG = [&](const FString& TextureName) -> FPolygonGroupID
        {
            const FName Slot = TextureName.IsEmpty() ? FName(TEXT("Default")) : FName(*NormalizeTextureKey(TextureName));
            if (FPolygonGroupID* Found = PolyGroups.Find(Slot)) { return *Found; }
            FPolygonGroupID NewId = MD.CreatePolygonGroup();
            PolyGroups.Add(Slot, NewId);
            PolyGroupSlots[NewId] = Slot;
            R.SlotNames.AddUnique(Slot);
            return NewId;
        };

        const bool bReverseWinding = ShouldReverseWinding(Sets);

        // Helper: configure a freshly-created vertex instance with default attributes.
        auto InitInstance = [&](FVertexInstanceID VI, const FVector2f& UV0, const FVector2f& UV1)
        {
            InstanceUVs.Set(VI, 0, UV0);
            InstanceUVs.Set(VI, 1, UV1);
            InstanceNormals[VI]        = FVector3f::ZeroVector;   // recomputed by ComputeTangentsAndNormals
            InstanceTangents[VI]       = FVector3f::ZeroVector;
            InstanceBinormalSigns[VI]  = 1.0f;
            InstanceColors[VI]         = FVector4f(1, 1, 1, 1);
        };

        const auto& Verts = Bsp.GetVertices();
        const auto& Faces = Bsp.GetFaces();

        // Clamp face range to the actual array.
        const int32 EndFace = FMath::Clamp(FirstFace + NumFaces, 0, Faces.Num());
        const int32 StartFace = FMath::Clamp(FirstFace, 0, Faces.Num());

        // ---------- Brush faces ----------
        for (int32 FaceIdx = StartFace; FaceIdx < EndFace; ++FaceIdx)
        {
            const FBspFace& F = Faces[FaceIdx];
            if (F.NumVertices < 3) { continue; }

            // 1) Allocate one vertex + one vertex instance per polygon corner (no per-triangle dup).
            TArray<FVertexInstanceID, TInlineAllocator<8>> Corners;
            Corners.SetNumUninitialized(F.NumVertices);
            for (uint32 i = 0; i < F.NumVertices; ++i)
            {
                const FVertexID V = MD.CreateVertex();
                const FVector LocalSrc = Verts[F.FirstVertex + i].Position - OriginOffsetSrc;
                VertexPositions[V] = (FVector3f)TransformPos(LocalSrc, Sets);
                const FVertexInstanceID VI = MD.CreateVertexInstance(V);
                InitInstance(VI,
                    (FVector2f)Verts[F.FirstVertex + i].UV,
                    (FVector2f)Verts[F.FirstVertex + i].LightmapUV);
                Corners[i] = VI;
            }

            const FPolygonGroupID PGID = GetOrCreatePG(F.TextureName);
            // 2) Fan-triangulate, reversing winding if the coordinate transform inverted handedness.
            for (uint32 t = 0; t < F.NumVertices - 2; ++t)
            {
                const FVertexInstanceID J0 = Corners[0];
                const FVertexInstanceID J1 = Corners[t + 1];
                const FVertexInstanceID J2 = Corners[t + 2];
                TArray<FVertexInstanceID, TFixedAllocator<3>> Tri;
                if (bReverseWinding) { Tri = { J0, J2, J1 }; }
                else                 { Tri = { J0, J1, J2 }; }
                MD.CreateTriangle(PGID, Tri);
            }
            ++R.FacesEmitted;
        }

        // ---------- Displacements ----------
        // Perimeter vertices of every disp grid are collected so we can spatially weld
        // coincident edges across neighbouring displacements after all grids are built.
        // We additionally track each disp's four edges as ordered FVertexID sequences
        // (corner-to-corner, length = side) so the power-mismatch T-junction snap pass
        // below can iterate per-edge sub-segments instead of binary-parsing CDispNeighbor.
        TArray<FVertexID> DispPerimeterVerts;
        TArray<int32>     DispPerimeterDispIdx; // parallel to DispPerimeterVerts; index into DispEdgeBundles
        struct FDispEdgeBundle
        {
            int32 DispIndex = INDEX_NONE;
            // Edges in CCW order: 0=y=0, 1=x=side-1, 2=y=side-1, 3=x=0. Each is length-`side`.
            TArray<FVertexID> Edges[4];
        };
        TArray<FDispEdgeBundle> DispEdgeBundles;
        if (bIncludeDisplacements)
        {
        const auto& Disps = Bsp.GetDispInfos();
        const auto& DV    = Bsp.GetDispVerts();
        for (const FDispInfo& DI : Disps)
        {
            // Skip dispinfos that never had a base face populated (would all be zero corners).
            const bool bHasCorners =
                !(DI.Corners[0].IsZero() && DI.Corners[1].IsZero() &&
                  DI.Corners[2].IsZero() && DI.Corners[3].IsZero());
            if (!bHasCorners) { ++R.DispsSkipped; continue; }

            const int32 Side  = (1 << DI.Power) + 1;
            const int32 Total = Side * Side;
            if (DI.VertStart < 0 || DI.VertStart + Total > DV.Num()) { ++R.DispsSkipped; continue; }

            // Rotate quad so the corner closest to StartPosition becomes (0,0).
            const int32 StartIdx = FindDispStartCorner(DI.Corners, DI.StartPosition);
            const FVector C00s = DI.Corners[(StartIdx + 0) & 3];
            const FVector C10s = DI.Corners[(StartIdx + 1) & 3];
            const FVector C11s = DI.Corners[(StartIdx + 2) & 3];
            const FVector C01s = DI.Corners[(StartIdx + 3) & 3];
            const FVector2D T00 = DI.CornerUVs[(StartIdx + 0) & 3];
            const FVector2D T10 = DI.CornerUVs[(StartIdx + 1) & 3];
            const FVector2D T11 = DI.CornerUVs[(StartIdx + 2) & 3];
            const FVector2D T01 = DI.CornerUVs[(StartIdx + 3) & 3];

            const FVector C00 = TransformPos(C00s, Sets);
            const FVector C10 = TransformPos(C10s, Sets);
            const FVector C11 = TransformPos(C11s, Sets);
            const FVector C01 = TransformPos(C01s, Sets);

            auto Bilinear = [&](float u, float v) -> FVector
            {
                const FVector A = FMath::Lerp(C00, C10, u);
                const FVector B = FMath::Lerp(C01, C11, u);
                return FMath::Lerp(A, B, v);
            };
            auto BilinearUV = [&](float u, float v) -> FVector2D
            {
                const FVector2D A = FMath::Lerp(T00, T10, u);
                const FVector2D B = FMath::Lerp(T01, T11, u);
                return FMath::Lerp(A, B, v);
            };

            // 1) Allocate one vertex + one instance per grid node.
            TArray<FVertexInstanceID> GridVI;  GridVI.SetNumUninitialized(Total);
            FDispEdgeBundle Bundle;
            Bundle.DispIndex = DispEdgeBundles.Num();
            for (int32 e = 0; e < 4; ++e) { Bundle.Edges[e].SetNumUninitialized(Side); }
            const int32 BundleIdx = Bundle.DispIndex;
            for (int32 y = 0; y < Side; ++y)
            {
                for (int32 x = 0; x < Side; ++x)
                {
                    const float u = static_cast<float>(x) / static_cast<float>(Side - 1);
                    const float v = static_cast<float>(y) / static_cast<float>(Side - 1);
                    const FVector Base = Bilinear(u, v);
                    const FDispVert& SrcDV = DV[DI.VertStart + y * Side + x];
                    const FVector OffsetSrc(SrcDV.Vector[0] * SrcDV.Dist,
                                            SrcDV.Vector[1] * SrcDV.Dist,
                                            SrcDV.Vector[2] * SrcDV.Dist);
                    const FVector P = Base + TransformDir(OffsetSrc, Sets);

                    const FVertexID V = MD.CreateVertex();
                    VertexPositions[V] = (FVector3f)P;
                    const FVertexInstanceID VI = MD.CreateVertexInstance(V);
                    const FVector2D UV = BilinearUV(u, v);
                    InitInstance(VI, (FVector2f)UV, FVector2f(u, v));
                    // Vertex-colour alpha drives WorldVertexTransition blends (range 0..1).
                    InstanceColors[VI] = FVector4f(1, 1, 1, FMath::Clamp(SrcDV.Alpha / 255.0f, 0.0f, 1.0f));
                    GridVI[y * Side + x] = VI;

                    // Track perimeter vertices for the cross-disp seam-weld pass below.
                    const bool bOnEdge0 = (y == 0);
                    const bool bOnEdge1 = (x == Side - 1);
                    const bool bOnEdge2 = (y == Side - 1);
                    const bool bOnEdge3 = (x == 0);
                    if (bOnEdge0 || bOnEdge1 || bOnEdge2 || bOnEdge3)
                    {
                        DispPerimeterVerts.Add(V);
                        DispPerimeterDispIdx.Add(BundleIdx);
                        if (bOnEdge0) { Bundle.Edges[0][x] = V; }
                        if (bOnEdge1) { Bundle.Edges[1][y] = V; }
                        if (bOnEdge2) { Bundle.Edges[2][x] = V; }
                        if (bOnEdge3) { Bundle.Edges[3][y] = V; }
                    }
                }
            }
            DispEdgeBundles.Add(MoveTemp(Bundle));

            const FPolygonGroupID PGID = GetOrCreatePG(DI.TextureName);
            for (int32 y = 0; y < Side - 1; ++y)
            {
                for (int32 x = 0; x < Side - 1; ++x)
                {
                    const FVertexInstanceID A = GridVI[(y    ) * Side + (x    )];
                    const FVertexInstanceID B = GridVI[(y    ) * Side + (x + 1)];
                    const FVertexInstanceID C = GridVI[(y + 1) * Side + (x + 1)];
                    const FVertexInstanceID D = GridVI[(y + 1) * Side + (x    )];

                    TArray<FVertexInstanceID, TFixedAllocator<3>> T1;
                    TArray<FVertexInstanceID, TFixedAllocator<3>> T2;
                    if (bReverseWinding) { T1 = { A, C, B }; T2 = { A, D, C }; }
                    else                 { T1 = { A, B, C }; T2 = { A, C, D }; }
                    MD.CreateTriangle(PGID, T1);
                    MD.CreateTriangle(PGID, T2);
                }
            }
            ++R.DispsEmitted;
        }
        } // if (bIncludeDisplacements)

        // ---------- Displacement seam stitching ----------
        // Cluster perimeter vertices across all displacement grids that lie within
        // `WeldDist` of each other and snap each cluster (size>=2) to its centroid.
        // This fixes the hairline T-junction cracks visible at terrain seams when
        // the source map wasn't perfectly "sewn" in Hammer. The interior of every
        // displacement is left untouched, so sculpted detail is preserved.
        const bool  bStitch  = Sets ? Sets->bStitchDisplacementSeams : true;
        const float WeldDist = Sets ? FMath::Max(KINDA_SMALL_NUMBER, Sets->DisplacementSeamWeldDistance) : 1.0f;
        if (bStitch && DispPerimeterVerts.Num() >= 2)
        {
            const float WeldDistSq = WeldDist * WeldDist;
            const float InvCell    = 1.0f / WeldDist;

            // Spatial hash: cell size = weld distance. Each vertex sits in exactly one cell;
            // potential cluster mates live in the 3x3x3 neighbourhood around it.
            TMap<FIntVector, TArray<int32>> Buckets;
            Buckets.Reserve(DispPerimeterVerts.Num());

            const int32 N = DispPerimeterVerts.Num();
            TArray<FVector3f> Pos;
            Pos.SetNumUninitialized(N);
            for (int32 i = 0; i < N; ++i)
            {
                Pos[i] = VertexPositions[DispPerimeterVerts[i]];
                const FIntVector Key(
                    FMath::FloorToInt(Pos[i].X * InvCell),
                    FMath::FloorToInt(Pos[i].Y * InvCell),
                    FMath::FloorToInt(Pos[i].Z * InvCell));
                Buckets.FindOrAdd(Key).Add(i);
            }

            // Union-find over indices into DispPerimeterVerts.
            TArray<int32> Parent; Parent.SetNumUninitialized(N);
            for (int32 i = 0; i < N; ++i) { Parent[i] = i; }
            auto Find = [&](int32 a) -> int32
            {
                while (Parent[a] != a)
                {
                    Parent[a] = Parent[Parent[a]];
                    a = Parent[a];
                }
                return a;
            };
            auto Union = [&](int32 a, int32 b)
            {
                const int32 ra = Find(a), rb = Find(b);
                if (ra != rb) { Parent[ra] = rb; }
            };

            for (int32 i = 0; i < N; ++i)
            {
                const FIntVector K(
                    FMath::FloorToInt(Pos[i].X * InvCell),
                    FMath::FloorToInt(Pos[i].Y * InvCell),
                    FMath::FloorToInt(Pos[i].Z * InvCell));
                for (int32 dz = -1; dz <= 1; ++dz)
                for (int32 dy = -1; dy <= 1; ++dy)
                for (int32 dx = -1; dx <= 1; ++dx)
                {
                    const FIntVector NK(K.X + dx, K.Y + dy, K.Z + dz);
                    const TArray<int32>* Cell = Buckets.Find(NK);
                    if (!Cell) { continue; }
                    for (int32 j : *Cell)
                    {
                        if (j <= i) { continue; } // each pair once, skip self
                        if (FVector3f::DistSquared(Pos[i], Pos[j]) <= WeldDistSq)
                        {
                            Union(i, j);
                        }
                    }
                }
            }

            // Aggregate cluster centroids and apply.
            struct FCluster { FVector3f Sum = FVector3f::ZeroVector; int32 Count = 0; };
            TMap<int32, FCluster> Clusters;
            for (int32 i = 0; i < N; ++i)
            {
                FCluster& C = Clusters.FindOrAdd(Find(i));
                C.Sum += Pos[i];
                ++C.Count;
            }
            for (int32 i = 0; i < N; ++i)
            {
                const FCluster& C = Clusters.FindChecked(Find(i));
                if (C.Count >= 2)
                {
                    const FVector3f Centroid = C.Sum / static_cast<float>(C.Count);
                    VertexPositions[DispPerimeterVerts[i]] = Centroid;
                }
            }

            int32 ClusterCount = 0;
            int32 WeldedVerts  = 0;
            for (const TPair<int32, FCluster>& P : Clusters)
            {
                if (P.Value.Count >= 2) { ++ClusterCount; WeldedVerts += P.Value.Count; }
            }
            R.DispSeamClusters    = ClusterCount;
            R.DispSeamVertsWelded = WeldedVerts;

            // ---------- Power-mismatch T-junction snap ----------
            // After the cluster pass, any perimeter vertex that ended up alone in its
            // cluster (size 1) is a candidate for a power-mismatch T-junction: a
            // higher-power disp's intermediate vertex along an edge it shares with a
            // lower-power neighbour, where the neighbour has no matching vertex.
            //
            // For each lonely vertex P on disp A, we look at every other disp B's edge
            // sub-segments (between two consecutive perimeter vertices on the same
            // edge of B) and snap P onto the closest sub-segment when P projects
            // strictly inside that sub-segment within `WeldDist`. Snapping onto a
            // post-weld neighbour edge eliminates the hairline split.
            //
            // The geometric pass intentionally avoids parsing CDispNeighbor /
            // CDispCornerNeighbors: the spatial cluster pass above has already
            // aligned coincident corners, so neighbour edges are well-defined by the
            // neighbour disp's own perimeter vertex sequence.
            int32 PowerSnaps = 0;
            if (DispEdgeBundles.Num() >= 2)
            {
                // Build a flat list of candidate snap segments: for each disp's edge,
                // each consecutive (Va, Vb) pair contributes one segment carrying the
                // owning disp index. Segments are short (one grid step), so segment
                // distance tests are tight enough to avoid spurious snaps.
                struct FSnapSeg { FVector3f A; FVector3f B; int32 DispIdx; };
                TArray<FSnapSeg> Segs;
                Segs.Reserve(DispEdgeBundles.Num() * 4 * 4);
                for (const FDispEdgeBundle& B : DispEdgeBundles)
                {
                    for (int32 e = 0; e < 4; ++e)
                    {
                        const TArray<FVertexID>& Edge = B.Edges[e];
                        for (int32 i = 0; i + 1 < Edge.Num(); ++i)
                        {
                            FSnapSeg S;
                            S.A = VertexPositions[Edge[i]];
                            S.B = VertexPositions[Edge[i + 1]];
                            S.DispIdx = B.DispIndex;
                            Segs.Add(S);
                        }
                    }
                }

                // Bin segments into a spatial hash (cell size = WeldDist) by the
                // cells their bbox (expanded by WeldDist) covers, so each lonely
                // vertex query touches only nearby segments.
                TMap<FIntVector, TArray<int32>> SegBuckets;
                SegBuckets.Reserve(Segs.Num());
                auto CellOf = [InvCell](const FVector3f& P) -> FIntVector
                {
                    return FIntVector(
                        FMath::FloorToInt(P.X * InvCell),
                        FMath::FloorToInt(P.Y * InvCell),
                        FMath::FloorToInt(P.Z * InvCell));
                };
                for (int32 si = 0; si < Segs.Num(); ++si)
                {
                    const FSnapSeg& S = Segs[si];
                    const FVector3f Lo(FMath::Min(S.A.X, S.B.X) - WeldDist,
                                       FMath::Min(S.A.Y, S.B.Y) - WeldDist,
                                       FMath::Min(S.A.Z, S.B.Z) - WeldDist);
                    const FVector3f Hi(FMath::Max(S.A.X, S.B.X) + WeldDist,
                                       FMath::Max(S.A.Y, S.B.Y) + WeldDist,
                                       FMath::Max(S.A.Z, S.B.Z) + WeldDist);
                    const FIntVector C0 = CellOf(Lo);
                    const FIntVector C1 = CellOf(Hi);
                    for (int32 cz = C0.Z; cz <= C1.Z; ++cz)
                    for (int32 cy = C0.Y; cy <= C1.Y; ++cy)
                    for (int32 cx = C0.X; cx <= C1.X; ++cx)
                    {
                        SegBuckets.FindOrAdd(FIntVector(cx, cy, cz)).Add(si);
                    }
                }

                for (int32 i = 0; i < N; ++i)
                {
                    const FCluster& C = Clusters.FindChecked(Find(i));
                    if (C.Count != 1) { continue; } // already merged with a neighbour

                    const FVertexID V    = DispPerimeterVerts[i];
                    const int32 OwnDisp  = DispPerimeterDispIdx[i];
                    const FVector3f Pf   = VertexPositions[V];

                    // Find the closest qualifying segment from a different disp.
                    float    BestDistSq = WeldDistSq;
                    FVector3f BestSnap  = Pf;
                    bool     bFound     = false;

                    const TArray<int32>* Cell = SegBuckets.Find(CellOf(Pf));
                    if (!Cell) { continue; }
                    for (int32 si : *Cell)
                    {
                        const FSnapSeg& S = Segs[si];
                        if (S.DispIdx == OwnDisp) { continue; } // skip own disp's own edges

                        const FVector3f AB = S.B - S.A;
                        const float ABLenSq = FVector3f::DotProduct(AB, AB);
                        if (ABLenSq <= KINDA_SMALL_NUMBER) { continue; }
                        const float t = FMath::Clamp(FVector3f::DotProduct(Pf - S.A, AB) / ABLenSq, 0.f, 1.f);
                        // Require strict interior (not at endpoints) so we only fix
                        // T-junctions, not coincident-corner cases (which were
                        // already handled by the centroid pass).
                        constexpr float EndpointEps = 0.05f;
                        if (t <= EndpointEps || t >= 1.f - EndpointEps) { continue; }
                        const FVector3f Closest = S.A + AB * t;
                        const float DSq = FVector3f::DistSquared(Pf, Closest);
                        if (DSq <= BestDistSq)
                        {
                            BestDistSq = DSq;
                            BestSnap   = Closest;
                            bFound     = true;
                        }
                    }

                    if (bFound)
                    {
                        VertexPositions[V] = BestSnap;
                        ++PowerSnaps;
                    }
                }
            }
            R.DispEdgeSnapsPowerMismatch = PowerSnaps;
        }

        UE_LOG(LogHL2BSPImporter, Log,
            TEXT("BSP build: Faces=%d Disps=%d SkippedDisps=%d V=%d VI=%d T=%d PG=%d Slots=%d SeamClusters=%d SeamVertsWelded=%d DispEdgeSnapsPowerMismatch=%d"),
            R.FacesEmitted, R.DispsEmitted, R.DispsSkipped,
            MD.Vertices().Num(), MD.VertexInstances().Num(), MD.Triangles().Num(),
            MD.PolygonGroups().Num(), R.SlotNames.Num(),
            R.DispSeamClusters, R.DispSeamVertsWelded, R.DispEdgeSnapsPowerMismatch);
        return R;
    }
} // namespace

UHL2BSPImporterFactory::UHL2BSPImporterFactory()
{
    bEditorImport   = true;
    bText           = false;
    bCreateNew      = false;
    bEditAfterNew   = false;
    SupportedClass  = UStaticMesh::StaticClass();
    ImportPriority  = DefaultImportPriority + 1;
    Formats.Add(TEXT("bsp;HL2 Map"));
}

bool UHL2BSPImporterFactory::FactoryCanImport(const FString& Filename)
{
    return Filename.EndsWith(TEXT(".bsp"), ESearchCase::IgnoreCase);
}

UObject* UHL2BSPImporterFactory::FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName,
                                                   EObjectFlags Flags, const FString& Filename, const TCHAR* /*Parms*/,
                                                   FFeedbackContext* Warn, bool& bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    UE_LOG(LogHL2BSPImporter, Log, TEXT("FactoryCreateFile: '%s' Parent=%s Name=%s"),
        *Filename, *GetNameSafe(InParent), *InName.ToString());

    FScopedSlowTask SlowTask(6.f, LOCTEXT("ImportBSP", "Importing HL2 BSP map..."));
    SlowTask.MakeDialog();

    SlowTask.EnterProgressFrame(1.f, LOCTEXT("ImportBSP_Validate", "Validating file..."));
    if (Filename.EndsWith(TEXT(".bz2"), ESearchCase::IgnoreCase))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("Input appears compressed (.bz2). Decompress before importing: %s"), *Filename);
    }
    if (!FPaths::FileExists(Filename))
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Input file does not exist: %s"), *Filename);
        return nullptr;
    }

    SlowTask.EnterProgressFrame(1.f, LOCTEXT("ImportBSP_Parse", "Parsing BSP lumps..."));
    FBspFile Bsp;
    if (!Bsp.LoadFromFile(Filename))
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed to load BSP from file: %s"), *Filename);
        return nullptr;
    }

    SlowTask.EnterProgressFrame(1.f, LOCTEXT("ImportBSP_Materials", "Resolving materials..."));
    const UHL2BSPImporterSettings* Sets = GetDefault<UHL2BSPImporterSettings>();
    if (!Sets)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("HL2BSPImporter settings CDO unavailable; aborting."));
        return nullptr;
    }
    FMaterialMap MaterialMap = LoadMaterialMap(Sets);

    // Extract any embedded pakfile (LUMP_PAKFILE / 40) into a per-import temp directory so
    // the material builder can resolve textures / VMTs that ship inside the BSP itself.
    FString PakExtractDir;
    if (Bsp.GetPakfileBytes().Num() > 0)
    {
        const FString BaseName = FPaths::GetBaseFilename(Filename);
        PakExtractDir = FPaths::Combine(
            FPaths::ProjectIntermediateDir(),
            TEXT("HL2BSPImporter"),
            TEXT("Pak"),
            FString::Printf(TEXT("%s_%s"), *BaseName, *FGuid::NewGuid().ToString(EGuidFormats::Short)));

        HL2Pak::FExtractStats PakStats;
        const bool bOk = HL2Pak::ExtractToDirectory(
            TArrayView<const uint8>(Bsp.GetPakfileBytes().GetData(), Bsp.GetPakfileBytes().Num()),
            PakExtractDir, PakStats);
        UE_LOG(LogHL2BSPImporter, Log,
            TEXT("Pakfile %s: extracted=%d skipped(deflate=%d, other=%d, unsafe=%d) failed=%d bytes=%lld dir='%s'"),
            bOk ? TEXT("OK") : TEXT("FAILED"),
            PakStats.NumExtracted, PakStats.NumSkippedDeflate, PakStats.NumSkippedOther,
            PakStats.NumSkippedUnsafe, PakStats.NumFailed,
            (long long)PakStats.TotalBytesExtracted, *PakExtractDir);
        if (!bOk || PakStats.NumExtracted == 0)
        {
            PakExtractDir.Reset();
        }
    }

    SlowTask.EnterProgressFrame(1.f, LOCTEXT("ImportBSP_Mesh", "Building mesh description..."));
    FBuildResult Built = BuildMeshDescriptionFromBSP(
        Bsp, Sets,
        Bsp.GetWorldFirstFace(), Bsp.GetWorldNumFaces(),
        /*bIncludeDisplacements=*/true);

    if (Built.MeshDesc.Triangles().Num() == 0)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("BSP produced zero triangles; nothing to import."));
        return nullptr;
    }

    SlowTask.EnterProgressFrame(1.f, LOCTEXT("ImportBSP_Build", "Building static mesh..."));

    // Shared material builder + JSON map across worldspawn and brush sub-models so caches dedupe.
    HL2Mat::FBuilder MatBuilder(Sets);
    if (!PakExtractDir.IsEmpty()) { MatBuilder.AddExtraRoot(PakExtractDir); }

    // Build & finalise a single UStaticMesh from a FBuildResult. Returns null on failure.
    auto FinalizeMesh = [&](FBuildResult& In, UObject* Outer, FName Name, EObjectFlags MeshFlags) -> UStaticMesh*
    {
        UStaticMesh* M = NewObject<UStaticMesh>(Outer, UStaticMesh::StaticClass(), Name, MeshFlags);
        if (!M)
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("NewObject<UStaticMesh> returned null (outer=%s, name=%s)."),
                *GetNameSafe(Outer), *Name.ToString());
            return nullptr;
        }
        M->NaniteSettings.bEnabled = Sets->bBuildNanite;

        FStaticMeshSourceModel& SM = M->AddSourceModel();
        SM.BuildSettings.bRecomputeNormals    = false;
        SM.BuildSettings.bRecomputeTangents   = false;
        SM.BuildSettings.bRemoveDegenerates   = true;
        SM.BuildSettings.bUseMikkTSpace       = true;
        SM.BuildSettings.bGenerateLightmapUVs = true;
        SM.BuildSettings.SrcLightmapIndex     = 1;
        SM.BuildSettings.DstLightmapIndex     = 1;
        SM.BuildSettings.MinLightmapResolution = 128;

        FStaticMeshOperations::ComputeTangentsAndNormals(
            In.MeshDesc,
            EComputeNTBsFlags::Normals
            | EComputeNTBsFlags::Tangents
            | EComputeNTBsFlags::WeightedNTBs
            | EComputeNTBsFlags::BlendOverlappingNormals
            | EComputeNTBsFlags::UseMikkTSpace);

        if (FMeshDescription* DestMD = M->CreateMeshDescription(0))
        {
            *DestMD = MoveTemp(In.MeshDesc);
            M->CommitMeshDescription(0);
        }
        else
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("UStaticMesh::CreateMeshDescription returned null."));
            return nullptr;
        }

        M->SetLightMapCoordinateIndex(1);
        M->SetLightMapResolution(128);

        // Material slot list.
        TArray<FStaticMaterial> StaticMats;
        StaticMats.Reserve(In.SlotNames.Num());
        UMaterialInterface* DefaultMat = UMaterial::GetDefaultMaterial(MD_Surface);
        int32 NumExplicit = 0, NumSynth = 0, NumDefault = 0;
        for (const FName& Slot : In.SlotNames)
        {
            UMaterialInterface* Mat = nullptr;
            const FString SlotKey = Slot.ToString();
            if (TObjectPtr<UMaterialInterface>* Found = MaterialMap.Find(SlotKey))
            {
                Mat = Found->Get();
                if (Mat) { ++NumExplicit; }
            }
            if (!Mat)
            {
                Mat = MatBuilder.GetOrCreateMaterial(SlotKey);
                if (Mat) { ++NumSynth; }
            }
            if (!Mat) { Mat = DefaultMat; ++NumDefault; }
            StaticMats.Add(FStaticMaterial(Mat, Slot, Slot));
        }
        M->SetStaticMaterials(StaticMats);
        UE_LOG(LogHL2BSPImporter, Log,
            TEXT("Mesh '%s' material assignment: explicit=%d synthesized=%d default=%d"),
            *Name.ToString(), NumExplicit, NumSynth, NumDefault);

        M->Build(false);
        M->PostEditChange();

        if (Sets->bImportCollision)
        {
            if (!M->GetBodySetup()) { M->CreateBodySetup(); }
            if (UBodySetup* BS = M->GetBodySetup()) { BS->CollisionTraceFlag = CTF_UseComplexAsSimple; }
        }

        FAssetRegistryModule::AssetCreated(M);
        M->MarkPackageDirty();
        return M;
    };

    UStaticMesh* Mesh = FinalizeMesh(Built, InParent, InName, Flags);
    if (!Mesh)
    {
        bOutOperationCanceled = true;
        return nullptr;
    }
    UE_LOG(LogHL2BSPImporter, Log, TEXT("Worldspawn StaticMesh built. LODs=%d Materials=%d"),
        Mesh->GetNumLODs(), Mesh->GetStaticMaterials().Num());

    // ---------- Brush sub-models (func_door, func_brush, water, etc.) ----------
    // Resolve model index → entity index (first match wins). Brush entities reference
    // their sub-model via the `model "*N"` keyvalue.
    TArray<FHL2Entity> Entities = Bsp.GetEntities();
    TMap<int32, int32> ModelToEntity;
    for (int32 EntIdx = 0; EntIdx < Entities.Num(); ++EntIdx)
    {
        const FString& ModelStr = Entities[EntIdx].Model;
        if (ModelStr.Len() < 2 || ModelStr[0] != TEXT('*')) { continue; }
        const int32 ModelNum = FCString::Atoi(*ModelStr + 1);
        if (ModelNum > 0) { ModelToEntity.FindOrAdd(ModelNum, EntIdx); }
    }

    const TArray<FBspBrushModel>& BrushModels = Bsp.GetBrushModels();
    int32 BrushMeshesBuilt = 0;
    if (BrushModels.Num() > 0 && InParent)
    {
        const FString ParentLongPath = FPackageName::GetLongPackagePath(InParent->GetPathName());
        const FString BaseName       = InParent->GetName();

        for (const FBspBrushModel& BM : BrushModels)
        {
            // Look up the owning entity (if any) so we can pivot the mesh around its origin.
            const int32* EntIdxPtr = ModelToEntity.Find(BM.ModelIndex);
            const FVector OriginOffsetSrc = (EntIdxPtr && Entities.IsValidIndex(*EntIdxPtr))
                ? Entities[*EntIdxPtr].Origin
                : FVector::ZeroVector;

            FBuildResult SubBuilt = BuildMeshDescriptionFromBSP(
                Bsp, Sets,
                BM.FirstFace, BM.NumFaces,
                /*bIncludeDisplacements=*/false,
                OriginOffsetSrc);

            if (SubBuilt.MeshDesc.Triangles().Num() == 0) { continue; }

            const FString SubShort = FString::Printf(TEXT("%s_BModel_%d"), *BaseName, BM.ModelIndex);
            const FString SubPkgName = ParentLongPath / SubShort;
            UPackage* SubPkg = CreatePackage(*SubPkgName);
            if (!SubPkg) { continue; }
            SubPkg->FullyLoad();

            UStaticMesh* SubMesh = FinalizeMesh(SubBuilt, SubPkg, FName(*SubShort), Flags);
            if (!SubMesh) { continue; }

            ++BrushMeshesBuilt;

            if (EntIdxPtr && Entities.IsValidIndex(*EntIdxPtr))
            {
                Entities[*EntIdxPtr].BrushMesh = FSoftObjectPath(SubMesh);
            }
            else
            {
                UE_LOG(LogHL2BSPImporter, Verbose,
                    TEXT("Brush model *%d has no owning entity; mesh '%s' is orphan."),
                    BM.ModelIndex, *SubMesh->GetPathName());
            }
        }
    }
    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("Brush sub-models: declared=%d meshes-built=%d"),
        BrushModels.Num(), BrushMeshesBuilt);

    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("Material builder totals: created=%d cached=%d failed=%d, tex created=%d cached=%d failed=%d"),
        MatBuilder.NumMaterialsCreated, MatBuilder.NumMaterialsCached, MatBuilder.NumMaterialsFailed,
        MatBuilder.NumTexturesCreated,  MatBuilder.NumTexturesCached,  MatBuilder.NumTexturesFailed);

    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("Material shaders: lit=%d masked=%d translucent=%d wvt=%d vlit=%d unlit=%d decal=%d other=%d"),
        MatBuilder.NumShader_Lit, MatBuilder.NumShader_LitMasked, MatBuilder.NumShader_LitTrans,
        MatBuilder.NumShader_Wvt, MatBuilder.NumShader_VertexLit, MatBuilder.NumShader_Unlit,
        MatBuilder.NumShader_Decal, MatBuilder.NumShader_Other);

    SlowTask.EnterProgressFrame(1.f, LOCTEXT("ImportBSP_Entities", "Creating entity table..."));
    // Entity-table creation is deferred to after the static-prop synthesis block below, so
    // any `prop_dynamic` / `prop_physics` rows can carry their resolved `PropMesh` asset paths.

    // ---------- Static props (sprp GameLump) ----------
    // Phase 9 / 9b emit instance metadata (model path + UE-space transform).
    // Phase 12 (this block, gated by bImportStaticPropMeshes) additionally
    // synthesises one UStaticMesh per unique referenced .mdl by parsing the
    // .mdl/.vvd/.dx90.vtx triple under SourceContentRoots + the per-import
    // pakfile extract dir, and stores the resulting asset path on each row's
    // StaticMeshAsset so a Blueprint / editor utility can spawn one
    // AStaticMeshActor per row at the recorded transform.
    const TArray<FBspStaticProp>& RawProps = Bsp.GetStaticProps();
    if (RawProps.Num() > 0 && InParent)
    {
        TArray<FHL2StaticProp> Props;
        Props.Reserve(RawProps.Num());
        for (const FBspStaticProp& Src : RawProps)
        {
            FHL2StaticProp P;
            P.ModelName    = Src.ModelName;
            P.Origin       = TransformPos(Src.Origin, Sets);
            // Source static-prop angles are (pitch, yaw, roll) in degrees, same convention as
            // entity `angles`. Pass through as-is; per-axis sign work for the Y-flip is handled
            // implicitly by callers spawning the actor (UE FRotator field order matches).
            P.Rotation     = FRotator(Src.Angles.X, Src.Angles.Y, Src.Angles.Z);
            P.UniformScale = Src.UniformScale;
            P.Skin         = Src.Skin;
            P.Solid        = Src.Solid;
            Props.Add(MoveTemp(P));
        }

        // ---- Phase 12: optional mesh synthesis ----
        if (Sets && Sets->bImportStaticPropMeshes && Sets->bSynthesizeMaterials)
        {
            // Roots in priority order: pakfile extract dir first (per-map shipped
            // assets), then user-configured roots.
            TArray<FString> StudioRoots;
            if (!PakExtractDir.IsEmpty()) { StudioRoots.Add(FPaths::ConvertRelativePathToFull(PakExtractDir)); }
            for (const FString& R : Sets->SourceContentRoots)
            {
                if (!R.IsEmpty()) { StudioRoots.Add(FPaths::ConvertRelativePathToFull(R)); }
            }

            // Sanitise a model-relative path into a /Game/-safe asset path. Source
            // paths are already a-z/0-9/_/-/.; replace anything else with '_'.
            auto SanitisePropSegment = [](const FString& In) -> FString
            {
                FString Out; Out.Reserve(In.Len());
                for (TCHAR Ch : In)
                {
                    if ((Ch >= TEXT('a') && Ch <= TEXT('z')) ||
                        (Ch >= TEXT('A') && Ch <= TEXT('Z')) ||
                        (Ch >= TEXT('0') && Ch <= TEXT('9')) ||
                        Ch == TEXT('_') || Ch == TEXT('-') || Ch == TEXT('/'))
                    { Out.AppendChar(Ch); }
                    else { Out.AppendChar(TEXT('_')); }
                }
                return Out;
            };

            auto NormaliseModelKey = [](const FString& In) -> FString
            {
                FString K = In.Replace(TEXT("\\"), TEXT("/"));
                K.ToLowerInline();
                while (K.StartsWith(TEXT("/"))) { K.RightChopInline(1, EAllowShrinking::No); }
                if (!K.EndsWith(TEXT(".mdl"))) { K += TEXT(".mdl"); }
                return K;
            };

            TMap<FString, FSoftObjectPath> ModelToAsset;
            for (const FHL2StaticProp& P : Props)
            {
                if (P.ModelName.IsEmpty()) { continue; }
                // Variant key is `<normalised model>#skin<N>`. Skin 0 keeps
                // the bare model key so legacy assets / cache hits round-trip
                // unchanged; non-zero skins produce sibling assets.
                FString Key = NormaliseModelKey(P.ModelName);
                if (P.Skin != 0) { Key += FString::Printf(TEXT("#skin%d"), P.Skin); }
                ModelToAsset.FindOrAdd(Key);
            }

            int32 NumBuilt = 0, NumCached = 0, NumFailed = 0;
            const FString AssetRoot = (Sets->SynthesizedAssetRoot.IsEmpty()
                ? FString(TEXT("/Game/HL2/Imported"))
                : Sets->SynthesizedAssetRoot);

            for (TPair<FString, FSoftObjectPath>& Pair : ModelToAsset)
            {
                const FString& VariantKey = Pair.Key;

                // Split off the optional `#skinN` suffix.
                FString ModelKey = VariantKey;
                int32 SkinIdx = 0;
                int32 SkinSepIdx = INDEX_NONE;
                if (VariantKey.FindLastChar(TEXT('#'), SkinSepIdx))
                {
                    const FString Tail = VariantKey.Mid(SkinSepIdx + 1);
                    if (Tail.StartsWith(TEXT("skin")))
                    {
                        SkinIdx = FCString::Atoi(*Tail.Mid(4));
                        ModelKey = VariantKey.Left(SkinSepIdx);
                    }
                }

                // Strip leading "models/" so the per-prop subtree lives at
                // <AssetRoot>/Props/<sub>/...; preserves Source's hierarchy.
                FString Stem = ModelKey.LeftChop(4); // drop ".mdl"
                if (Stem.StartsWith(TEXT("models/"))) { Stem.RightChopInline(7, EAllowShrinking::No); }
                Stem = SanitisePropSegment(Stem);

                FString Dir, AssetName;
                if (!Stem.Split(TEXT("/"), &Dir, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
                {
                    Dir.Reset();
                    AssetName = Stem;
                }
                if (AssetName.IsEmpty()) { ++NumFailed; continue; }
                if (SkinIdx != 0) { AssetName += FString::Printf(TEXT("_skin%d"), SkinIdx); }

                FString PkgPath = AssetRoot / TEXT("Props") / Dir;
                while (PkgPath.EndsWith(TEXT("/"))) { PkgPath.LeftChopInline(1, EAllowShrinking::No); }
                const FString FullObject = PkgPath / AssetName + TEXT(".") + AssetName;

                // Re-use any existing asset on disk (stable across re-imports).
                FSoftObjectPath SOP(FullObject);
                if (UStaticMesh* Existing = Cast<UStaticMesh>(SOP.TryLoad()))
                {
                    Pair.Value = FSoftObjectPath(Existing);
                    ++NumCached;
                    continue;
                }

                HL2Studio::FStudioFile Studio;
                FString Err;
                if (!HL2Studio::LoadModel(ModelKey, StudioRoots, Studio, Err))
                {
                    UE_LOG(LogHL2BSPImporter, Verbose,
                        TEXT("Static prop mesh skipped (%s): %s"), *ModelKey, *Err);
                    ++NumFailed;
                    continue;
                }

                UPackage* PropPkg = CreatePackage(*(PkgPath / AssetName));
                if (!PropPkg)
                {
                    UE_LOG(LogHL2BSPImporter, Warning,
                        TEXT("Static prop mesh: CreatePackage failed for %s"), *(PkgPath / AssetName));
                    ++NumFailed;
                    continue;
                }
                PropPkg->FullyLoad();

                UStaticMesh* M = HL2Studio::BuildStaticMesh(
                    Studio, Sets, MatBuilder, PropPkg, FName(*AssetName), Flags, SkinIdx);
                if (M)
                {
                    Pair.Value = FSoftObjectPath(M);
                    ++NumBuilt;
                }
                else
                {
                    ++NumFailed;
                }
            }

            // Back-assign to every row that referenced one of the resolved variants.
            for (FHL2StaticProp& P : Props)
            {
                if (P.ModelName.IsEmpty()) { continue; }
                FString Key = NormaliseModelKey(P.ModelName);
                if (P.Skin != 0) { Key += FString::Printf(TEXT("#skin%d"), P.Skin); }
                if (FSoftObjectPath* Asset = ModelToAsset.Find(Key))
                {
                    P.StaticMeshAsset = *Asset;
                }
            }

            UE_LOG(LogHL2BSPImporter, Log,
                TEXT("Static prop meshes: built=%d cached=%d failed=%d (unique=%d, instances=%d)"),
                NumBuilt, NumCached, NumFailed, ModelToAsset.Num(), Props.Num());

            // ---- Entity props (`prop_dynamic` / `prop_physics` / `prop_ragdoll` / etc.) ----
            // Source emits these as point entities in the entity lump rather than via the
            // `sprp` GameLump. We resolve the same `.mdl` triple, run the same builder, and
            // back-assign `FHL2Entity::PropMesh`. Only the static geometry is produced —
            // animation / physics simulation is left to the user's pipeline. Sharing the
            // on-disk cache via TryLoad means a model already produced by the prop_static
            // pass above is a cache hit here (no rebuild).
            int32 EntBuilt = 0, EntCached = 0, EntFailed = 0, EntCandidates = 0;
            TMap<FString, FSoftObjectPath> EntModelToAsset;
            for (FHL2Entity& E : Entities)
            {
                if (E.Class.Len() < 5 || !E.Class.StartsWith(TEXT("prop_"))) { continue; }
                if (E.Model.IsEmpty()) { continue; }
                // Brush-model refs (e.g. `*23` on `prop_door_rotating`) are not .mdl paths.
                if (E.Model.StartsWith(TEXT("*"))) { continue; }
                FString Lower = E.Model; Lower.ToLowerInline();
                if (!Lower.EndsWith(TEXT(".mdl"))) { continue; }
                ++EntCandidates;
                FString Key = NormaliseModelKey(E.Model);
                if (E.Skin != 0)      { Key += FString::Printf(TEXT("#skin%d"), E.Skin); }
                if (E.BodyGroup != 0) { Key += FString::Printf(TEXT("#bg%d"),   E.BodyGroup); }
                EntModelToAsset.FindOrAdd(Key);
            }

            for (TPair<FString, FSoftObjectPath>& Pair : EntModelToAsset)
            {
                const FString& VariantKey = Pair.Key;
                FString ModelKey = VariantKey;
                int32 SkinIdx = 0;
                int32 BodyMask = 0;
                // Variant key suffix grammar: `<model>(#skin<N>)?(#bg<M>)?`. Strip
                // suffixes from the right; either may be absent.
                auto StripSuffix = [&ModelKey](const TCHAR* Tag, int32& OutVal) -> bool
                {
                    int32 SepIdx = INDEX_NONE;
                    if (!ModelKey.FindLastChar(TEXT('#'), SepIdx)) { return false; }
                    const FString Tail = ModelKey.Mid(SepIdx + 1);
                    if (!Tail.StartsWith(Tag)) { return false; }
                    OutVal = FCString::Atoi(*Tail.Mid(FCString::Strlen(Tag)));
                    ModelKey = ModelKey.Left(SepIdx);
                    return true;
                };
                StripSuffix(TEXT("bg"),   BodyMask);
                StripSuffix(TEXT("skin"), SkinIdx);

                FString Stem = ModelKey.LeftChop(4);
                if (Stem.StartsWith(TEXT("models/"))) { Stem.RightChopInline(7, EAllowShrinking::No); }
                Stem = SanitisePropSegment(Stem);

                FString Dir, AssetName;
                if (!Stem.Split(TEXT("/"), &Dir, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
                {
                    Dir.Reset();
                    AssetName = Stem;
                }
                if (AssetName.IsEmpty()) { ++EntFailed; continue; }
                if (SkinIdx  != 0) { AssetName += FString::Printf(TEXT("_skin%d"), SkinIdx); }
                if (BodyMask != 0) { AssetName += FString::Printf(TEXT("_bg%d"),   BodyMask); }

                FString PkgPath = AssetRoot / TEXT("Props") / Dir;
                while (PkgPath.EndsWith(TEXT("/"))) { PkgPath.LeftChopInline(1, EAllowShrinking::No); }
                const FString FullObject = PkgPath / AssetName + TEXT(".") + AssetName;

                FSoftObjectPath SOP(FullObject);
                if (UStaticMesh* Existing = Cast<UStaticMesh>(SOP.TryLoad()))
                {
                    Pair.Value = FSoftObjectPath(Existing);
                    ++EntCached;
                    continue;
                }

                HL2Studio::FStudioFile Studio;
                FString Err;
                if (!HL2Studio::LoadModel(ModelKey, StudioRoots, Studio, Err))
                {
                    UE_LOG(LogHL2BSPImporter, Verbose,
                        TEXT("Entity prop mesh skipped (%s): %s"), *ModelKey, *Err);
                    ++EntFailed;
                    continue;
                }

                UPackage* PropPkg = CreatePackage(*(PkgPath / AssetName));
                if (!PropPkg)
                {
                    UE_LOG(LogHL2BSPImporter, Warning,
                        TEXT("Entity prop mesh: CreatePackage failed for %s"), *(PkgPath / AssetName));
                    ++EntFailed;
                    continue;
                }
                PropPkg->FullyLoad();

                UStaticMesh* M = HL2Studio::BuildStaticMesh(
                    Studio, Sets, MatBuilder, PropPkg, FName(*AssetName), Flags, SkinIdx, BodyMask);
                if (M)
                {
                    Pair.Value = FSoftObjectPath(M);
                    ++EntBuilt;
                }
                else
                {
                    ++EntFailed;
                }
            }

            for (FHL2Entity& E : Entities)
            {
                if (E.Class.Len() < 5 || !E.Class.StartsWith(TEXT("prop_"))) { continue; }
                if (E.Model.IsEmpty() || E.Model.StartsWith(TEXT("*"))) { continue; }
                FString Lower = E.Model; Lower.ToLowerInline();
                if (!Lower.EndsWith(TEXT(".mdl"))) { continue; }
                FString Key = NormaliseModelKey(E.Model);
                if (E.Skin != 0)      { Key += FString::Printf(TEXT("#skin%d"), E.Skin); }
                if (E.BodyGroup != 0) { Key += FString::Printf(TEXT("#bg%d"),   E.BodyGroup); }
                if (FSoftObjectPath* Asset = EntModelToAsset.Find(Key))
                {
                    E.PropMesh = *Asset;
                }
            }

            UE_LOG(LogHL2BSPImporter, Log,
                TEXT("Entity prop meshes: built=%d cached=%d failed=%d (unique=%d, candidates=%d)"),
                EntBuilt, EntCached, EntFailed, EntModelToAsset.Num(), EntCandidates);
        }

        const FString ParentLongPath = FPackageName::GetLongPackagePath(InParent->GetPathName());
        const FString ShortName      = InParent->GetName() + TEXT("_StaticProps");
        const FString PropPkgName    = ParentLongPath / ShortName;

        if (UPackage* PropPkg = CreatePackage(*PropPkgName))
        {
            PropPkg->FullyLoad();
            UHL2StaticPropTable* Table = UHL2StaticPropTable::CreateFromProps(PropPkg, FName(*ShortName), Flags, Props);
            if (Table)
            {
                FAssetRegistryModule::AssetCreated(Table);
                Table->MarkPackageDirty();
                UE_LOG(LogHL2BSPImporter, Log, TEXT("Created StaticProps DataTable: %s (%d rows)"),
                    *Table->GetPathName(), Props.Num());
            }
        }
    }

    // ---------- Entity table (deferred until after entity-prop synthesis) ----------
    if (Entities.Num() > 0 && InParent)
    {
        const FString ParentLongPath = FPackageName::GetLongPackagePath(InParent->GetPathName());
        const FString ShortName      = InParent->GetName() + TEXT("_Entities");
        const FString EntityPkgName  = ParentLongPath / ShortName;

        if (UPackage* EntPkg = CreatePackage(*EntityPkgName))
        {
            EntPkg->FullyLoad();
            UHL2EntityTable* Table = UHL2EntityTable::CreateFromEntities(EntPkg, FName(*ShortName), Flags, Entities);
            if (Table)
            {
                FAssetRegistryModule::AssetCreated(Table);
                Table->MarkPackageDirty();
                UE_LOG(LogHL2BSPImporter, Log, TEXT("Created Entities DataTable: %s (%d rows)"),
                    *Table->GetPathName(), Entities.Num());
            }
        }
    }

    return Mesh;
}

#undef LOCTEXT_NAMESPACE
