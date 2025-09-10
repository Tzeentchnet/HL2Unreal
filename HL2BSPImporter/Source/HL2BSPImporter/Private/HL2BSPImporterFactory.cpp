#include "HL2BSPImporterFactory.h"
#include "HL2BSPImporter.h"
#include "BspFile.h"
#include "HL2EntityTable.h"
#include "HL2BSPImporterSettings.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "UObject/Package.h"
#include "Materials/Material.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/SoftObjectPath.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "Misc/FeedbackContext.h"

static TMap<FString, UMaterialInterface*> GMaterialMap;

static TMap<FString, UMaterialInterface*> LoadMaterialMap()
{
    TMap<FString, UMaterialInterface*> Map;

    const UHL2BSPImporterSettings* Sets = GetDefault<UHL2BSPImporterSettings>();
    TArray<FString> Candidates;

    auto AddIfFile = [&](const FString& Path)
    {
        if (!Path.IsEmpty() && FPaths::FileExists(Path))
        {
            Candidates.Add(Path);
        }
    };

    // Resolve settings path
    if (!Sets->MaterialJsonPath.IsEmpty())
    {
        FString P = Sets->MaterialJsonPath;
        if (P.StartsWith(TEXT("/Game/")))
        {
            FString Rel = P.RightChop(6); // strip /Game/
            if (!Rel.EndsWith(TEXT(".json")))
            {
                Rel += TEXT(".json");
            }
            FString Abs = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Rel);
            if (FPaths::FileExists(Abs))
            {
                AddIfFile(Abs);
            }
            else
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("MaterialJsonPath points to '/Game/...', but file was not found: %s"), *Abs);
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
            FString Fallback = Plugin->GetBaseDir() / TEXT("Resources/Materials.json");
            AddIfFile(Fallback);
        }
    }

    FString Chosen;
    if (Candidates.Num() > 0)
    {
        Chosen = Candidates[0];
    }
    else
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("No candidate material JSON found. Using empty material map."));
        return Map; // nothing to load
    }

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

    const TArray<TSharedPtr<FJsonValue>>& Arr = RootValue->AsArray();
    for (const TSharedPtr<FJsonValue>& V : Arr)
    {
        if (!V.IsValid() || V->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> Obj = V->AsObject();
        FString TextureName;
        FString MaterialPath;
        if (!Obj->TryGetStringField(TEXT("TextureName"), TextureName)) continue;
        if (!Obj->TryGetStringField(TEXT("MaterialPath"), MaterialPath)) continue;

        FSoftObjectPath Path(MaterialPath);
        if (UObject* Loaded = Path.TryLoad())
        {
            if (auto* MI = Cast<UMaterialInterface>(Loaded))
            {
                Map.Add(TextureName, MI);
            }
        }
    }
    UE_LOG(LogHL2BSPImporter, Log, TEXT("Loaded %d material mappings from: %s"), Map.Num(), *Chosen);
    return Map;
}

static FVector TransformPos(const FVector& In, const UHL2BSPImporterSettings* Sets)
{
    FVector P = In;
    if (Sets && Sets->bFlipYZ)
    {
        P = FVector(In.X, In.Z, In.Y);
    }
    // Match legacy behavior: flip Y sign for Source->UE forward axis
    P.Y *= -1.f;
    const float Scale = Sets ? Sets->WorldScale : 1.f;
    P *= Scale;
    return P;
}

static FVector TransformDir(const FVector& In, const UHL2BSPImporterSettings* Sets)
{
    // Same as position for linear transforms (swap/flip/scale)
    return TransformPos(In, Sets);
}

static FMeshDescription BuildMeshDescriptionFromBSP(const FBspFile& Bsp, const UHL2BSPImporterSettings* Sets, TArray<FName>& OutMaterialSlotNames)
{
    FMeshDescription MD;
    FStaticMeshAttributes Attrs(MD);
    Attrs.Register();

    TVertexAttributesRef<FVector3f> VertexPositions = MD.GetVertexPositions();
    TVertexInstanceAttributesRef<FVector3f> InstanceNormals = Attrs.GetVertexInstanceNormals();
    TVertexInstanceAttributesRef<FVector3f> InstanceTangents = Attrs.GetVertexInstanceTangents();
    TVertexInstanceAttributesRef<float> InstanceBinormalSigns = Attrs.GetVertexInstanceBinormalSigns();
    TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attrs.GetVertexInstanceColors();
    TVertexInstanceAttributesRef<FVector2f> InstanceUVs = Attrs.GetVertexInstanceUVs();
    InstanceUVs.SetNumChannels(1);

    TPolygonGroupAttributesRef<FName> PolyGroupMaterialNames = Attrs.GetPolygonGroupMaterialSlotNames();

    // Map texture name -> polygon group
    TMap<FName, FPolygonGroupID> PolyGroups;
    auto GetOrCreatePG = [&](const FString& TextureName) -> FPolygonGroupID
    {
        const FName SlotName = TextureName.IsEmpty() ? FName(TEXT("Default")) : FName(*TextureName);
        if (FPolygonGroupID* Found = PolyGroups.Find(SlotName))
        {
            return *Found;
        }
        FPolygonGroupID NewId = MD.CreatePolygonGroup();
        PolyGroups.Add(SlotName, NewId);
        PolyGroupMaterialNames[NewId] = SlotName;
        OutMaterialSlotNames.AddUnique(SlotName);
        return NewId;
    };

    const auto& Verts = Bsp.GetVertices();
    const auto& Faces = Bsp.GetFaces();

    // NOTE: Tri creation helper removed; triangles are built inline per polygon group.

    // Brushes: fan-triangulate faces; assign polygon groups by texture name
    int32 FacesProcessed = 0;
    for (const auto& F : Faces)
    {
        if (F.NumVertices < 3) continue;
        const FPolygonGroupID PGID = GetOrCreatePG(F.TextureName);
        for (uint32 t = 0; t < F.NumVertices - 2; ++t)
        {
            const uint32 I0 = F.FirstVertex;
            const uint32 I1 = F.FirstVertex + t + 1;
            const uint32 I2 = F.FirstVertex + t + 2;
            // Rebind AddTri to use this PGID
            // Create vertices with transform
            const FVertexID V0 = MD.CreateVertex(); VertexPositions[V0] = (FVector3f)TransformPos(Verts[I0].Position, Sets);
            const FVertexID V1 = MD.CreateVertex(); VertexPositions[V1] = (FVector3f)TransformPos(Verts[I1].Position, Sets);
            const FVertexID V2 = MD.CreateVertex(); VertexPositions[V2] = (FVector3f)TransformPos(Verts[I2].Position, Sets);

            const FVertexInstanceID J0 = MD.CreateVertexInstance(V0);
            const FVertexInstanceID J1 = MD.CreateVertexInstance(V1);
            const FVertexInstanceID J2 = MD.CreateVertexInstance(V2);

            InstanceUVs.Set(J0, 0, (FVector2f)Verts[I0].UV);
            InstanceUVs.Set(J1, 0, (FVector2f)Verts[I1].UV);
            InstanceUVs.Set(J2, 0, (FVector2f)Verts[I2].UV);

            const FVector3f Up = (FVector3f)FVector::UpVector;
            InstanceNormals[J0] = Up; InstanceNormals[J1] = Up; InstanceNormals[J2] = Up;
            InstanceTangents[J0] = FVector3f::ZeroVector; InstanceTangents[J1] = FVector3f::ZeroVector; InstanceTangents[J2] = FVector3f::ZeroVector;
            InstanceBinormalSigns[J0] = 1.0f; InstanceBinormalSigns[J1] = 1.0f; InstanceBinormalSigns[J2] = 1.0f;
            InstanceColors[J0] = FVector4f(1,1,1,1); InstanceColors[J1] = FVector4f(1,1,1,1); InstanceColors[J2] = FVector4f(1,1,1,1);

            TArray<FVertexInstanceID, TFixedAllocator<3>> InstIDs{ J0, J1, J2 };
            MD.CreateTriangle(PGID, InstIDs);
        }
        ++FacesProcessed;
    }

    // Displacements: build via bilinear from base quad; use dispvert vectors as offsets
    const auto& Disps = Bsp.GetDispInfos();
    const auto& DV = Bsp.GetDispVerts();
    int32 DispsProcessed = 0;
    int32 DispsSkipped = 0;
    for (const auto& DI : Disps)
    {
        if (DI.MapFace < 0 || DI.MapFace >= Faces.Num()) { ++DispsSkipped; continue; }
        const auto& BaseFace = Faces[DI.MapFace];
        if (BaseFace.NumVertices < 4) { ++DispsSkipped; continue; } // only handle quads for now

        const int32 Side = (1 << DI.Power) + 1;
        const int32 Total = Side * Side;
        if (DI.VertStart < 0 || DI.VertStart + Total > DV.Num()) { ++DispsSkipped; continue; }

        // Base quad corners in 0..1 grid order (00,10,11,01)
        const uint32 I0 = BaseFace.FirstVertex + 0;
        const uint32 I1 = BaseFace.FirstVertex + 1;
        const uint32 I2 = BaseFace.FirstVertex + 2;
        const uint32 I3 = BaseFace.FirstVertex + 3;
        if (I3 >= (uint32)Verts.Num()) continue;

        const FVector C0 = TransformPos(Verts[I0].Position, Sets);
        const FVector C1 = TransformPos(Verts[I1].Position, Sets);
        const FVector C2 = TransformPos(Verts[I2].Position, Sets);
        const FVector C3 = TransformPos(Verts[I3].Position, Sets);

        auto Bilinear = [&](float u, float v) -> FVector
        {
            const FVector A = FMath::Lerp(C0, C1, u);
            const FVector B = FMath::Lerp(C3, C2, u);
            return FMath::Lerp(A, B, v);
        };

        // Build grid vertices and store bilinear UVs from base face
        TArray<FVertexID> Grid; Grid.SetNum(Total);
        const FVector2D T0 = Verts[I0].UV;
        const FVector2D T1 = Verts[I1].UV;
        const FVector2D T2 = Verts[I2].UV;
        const FVector2D T3 = Verts[I3].UV;
        auto BilinearUV = [&](float u, float v) -> FVector2D
        {
            const FVector2D A = FMath::Lerp(T0, T1, u);
            const FVector2D B = FMath::Lerp(T3, T2, u);
            return FMath::Lerp(A, B, v);
        };
        TArray<FVector2f> GridUV; GridUV.SetNum(Total);
        for (int32 y = 0; y < Side; ++y)
        {
            for (int32 x = 0; x < Side; ++x)
            {
                const float u = (float)x / (Side - 1);
                const float v = (float)y / (Side - 1);
                const FVector Base = Bilinear(u, v);
                const auto& SrcDV = DV[DI.VertStart + y * Side + x];
                const FVector Offset(SrcDV.Vector[0], SrcDV.Vector[1], SrcDV.Vector[2]);
                const FVector P = Base + TransformDir(Offset, Sets);

                const FVertexID VId = MD.CreateVertex(); VertexPositions[VId] = (FVector3f)P;
                Grid[y * Side + x] = VId;
                GridUV[y * Side + x] = (FVector2f)BilinearUV(u, v);
            }
        }

        const FPolygonGroupID PGID = GetOrCreatePG(BaseFace.TextureName);
        for (int32 y = 0; y < Side - 1; ++y)
        {
            for (int32 x = 0; x < Side - 1; ++x)
            {
                const int A = y * Side + x;
                const int B = A + 1;
                const int C = (y + 1) * Side + x + 1;
                const int D = (y + 1) * Side + x;

                const FVertexInstanceID IA = MD.CreateVertexInstance(Grid[A]);
                const FVertexInstanceID IB = MD.CreateVertexInstance(Grid[B]);
                const FVertexInstanceID IC = MD.CreateVertexInstance(Grid[C]);
                const FVertexInstanceID ID = MD.CreateVertexInstance(Grid[D]);

                // Assign UVs and default attributes for the new instances
                InstanceUVs.Set(IA, 0, GridUV[A]);
                InstanceUVs.Set(IB, 0, GridUV[B]);
                InstanceUVs.Set(IC, 0, GridUV[C]);
                InstanceUVs.Set(ID, 0, GridUV[D]);
                const FVector3f Up = (FVector3f)FVector::UpVector;
                InstanceNormals[IA] = Up; InstanceNormals[IB] = Up; InstanceNormals[IC] = Up; InstanceNormals[ID] = Up;
                InstanceTangents[IA] = FVector3f::ZeroVector; InstanceTangents[IB] = FVector3f::ZeroVector; InstanceTangents[IC] = FVector3f::ZeroVector; InstanceTangents[ID] = FVector3f::ZeroVector;
                InstanceBinormalSigns[IA] = 1.0f; InstanceBinormalSigns[IB] = 1.0f; InstanceBinormalSigns[IC] = 1.0f; InstanceBinormalSigns[ID] = 1.0f;
                InstanceColors[IA] = FVector4f(1,1,1,1); InstanceColors[IB] = FVector4f(1,1,1,1); InstanceColors[IC] = FVector4f(1,1,1,1); InstanceColors[ID] = FVector4f(1,1,1,1);

                TArray<FVertexInstanceID, TFixedAllocator<3>> Tri1{ IA, IB, IC };
                TArray<FVertexInstanceID, TFixedAllocator<3>> Tri2{ IA, IC, ID };
                MD.CreateTriangle(PGID, Tri1);
                MD.CreateTriangle(PGID, Tri2);
            }
        }
        ++DispsProcessed;
    }
    UE_LOG(LogHL2BSPImporter, Log, TEXT("BSP build: Faces=%d Disps=%d SkippedDisps=%d V=%d VI=%d T=%d PG=%d Slots=%d"),
        FacesProcessed, DispsProcessed, DispsSkipped, MD.Vertices().Num(), MD.VertexInstances().Num(), MD.Triangles().Num(), MD.PolygonGroups().Num(), OutMaterialSlotNames.Num());
    return MD;
}

UHL2BSPImporterFactory::UHL2BSPImporterFactory()
{
    bEditorImport = true;
    SupportedClass = UStaticMesh::StaticClass();
    Formats.Add(TEXT("bsp;HL2 Map"));
    UE_LOG(LogHL2BSPImporter, Log, TEXT("UHL2BSPImporterFactory constructed. SupportedClass=UStaticMesh Formats=%s"), TEXT("bsp"));
}

bool UHL2BSPImporterFactory::FactoryCanImport(const FString& Filename)
{
    const bool bCan = Filename.EndsWith(TEXT(".bsp"), ESearchCase::IgnoreCase);
    UE_LOG(LogHL2BSPImporter, Log, TEXT("FactoryCanImport(%s) -> %s"), *Filename, bCan ? TEXT("true") : TEXT("false"));
    UE_LOG(LogTemp, Log, TEXT("[HL2BSPImporter] FactoryCanImport(%s) -> %s"), *Filename, bCan ? TEXT("true") : TEXT("false"));
    return bCan;
}

UObject* UHL2BSPImporterFactory::FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName,
                                                   EObjectFlags Flags, const FString& Filename, const TCHAR* Parms,
                                                   FFeedbackContext* Warn, bool& bOutOperationCanceled)
{
    UE_LOG(LogHL2BSPImporter, Log, TEXT("FactoryCreateFile: '%s' InParent=%s InName=%s"), *Filename, *GetNameSafe(InParent), *InName.ToString());
    UE_LOG(LogTemp, Log, TEXT("[HL2BSPImporter] FactoryCreateFile: '%s' InParent=%s InName=%s"), *Filename, *GetNameSafe(InParent), *InName.ToString());
    if (Warn)
    {
        Warn->Logf(ELogVerbosity::Display, TEXT("HL2BSPImporter: Importing %s"), *Filename);
    }
    // Basic file diagnostics before parsing
    const bool bExists = FPaths::FileExists(Filename);
    const int64 FileSize = IFileManager::Get().FileSize(*Filename);
    UE_LOG(LogHL2BSPImporter, Log, TEXT("File check: Exists=%s Size=%lld"), bExists ? TEXT("true") : TEXT("false"), FileSize);
    if (Warn)
    {
        Warn->Logf(ELogVerbosity::Display, TEXT("HL2BSPImporter: File exists=%s size=%lld"), bExists ? TEXT("true") : TEXT("false"), FileSize);
    }
    if (!bExists)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Input file does not exist: %s"), *Filename);
        if (Warn)
        {
            Warn->Logf(ELogVerbosity::Error, TEXT("HL2BSPImporter: File does not exist: %s"), *Filename);
        }
    }
    if (Filename.EndsWith(TEXT(".bz2"), ESearchCase::IgnoreCase))
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("Input appears compressed (.bz2). Decompress before importing: %s"), *Filename);
        if (Warn)
        {
            Warn->Logf(ELogVerbosity::Warning, TEXT("HL2BSPImporter: File looks compressed (.bz2). Decompress before importing."));
        }
    }

    // Quick read probe to catch permissions/locking issues
    TArray<uint8> Probe;
    const bool bProbeOk = FFileHelper::LoadFileToArray(Probe, *Filename);
    UE_LOG(LogHL2BSPImporter, Log, TEXT("Probe read: %s (bytes=%d)"), bProbeOk ? TEXT("OK") : TEXT("FAILED"), Probe.Num());
    if (Warn)
    {
        Warn->Logf(bProbeOk ? ELogVerbosity::Display : ELogVerbosity::Warning, TEXT("HL2BSPImporter: Probe read %s (bytes=%d)"), bProbeOk ? TEXT("OK") : TEXT("FAILED"), Probe.Num());
    }

    FBspFile Bsp;
    if (!Bsp.LoadFromFile(Filename))
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Failed to load BSP from file: %s"), *Filename);
        UE_LOG(LogTemp, Error, TEXT("[HL2BSPImporter] Failed to load BSP: %s"), *Filename);
        // Dump basic header info from the probe buffer to aid diagnosis
        if (Probe.Num() >= 8)
        {
            const int32 Ident = *(const int32*)Probe.GetData();
            const int32 Version = *(const int32*)(Probe.GetData() + 4);
            ANSICHAR Magic[5]; Magic[0] = (Ident & 0xFF); Magic[1] = (Ident >> 8) & 0xFF; Magic[2] = (Ident >> 16) & 0xFF; Magic[3] = (Ident >> 24) & 0xFF; Magic[4] = 0;
            UE_LOG(LogHL2BSPImporter, Error, TEXT("Probe header: Ident='%hs' (0x%08x) Version=%d"), Magic, Ident, Version);
            if (Warn)
            {
                Warn->Logf(ELogVerbosity::Error, TEXT("HL2BSPImporter: Probe header Ident='%hs' (0x%08x) Version=%d"), Magic, Ident, Version);
            }
        }
        else
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("Probe buffer too small to read header (bytes=%d)"), Probe.Num());
        }
        if (Warn)
        {
            Warn->Logf(ELogVerbosity::Error, TEXT("HL2BSPImporter: Failed to parse BSP. See Output Log for details."));
        }
        return nullptr;
    }

    GMaterialMap = LoadMaterialMap();

    const UHL2BSPImporterSettings* Sets = GetDefault<UHL2BSPImporterSettings>();
    TArray<FName> SlotNames;
    FMeshDescription MD = BuildMeshDescriptionFromBSP(Bsp, Sets, SlotNames);

    // Create the asset in the provided parent package with provided flags
    UStaticMesh* Mesh = NewObject<UStaticMesh>(InParent, InClass ? InClass : UStaticMesh::StaticClass(), InName, Flags);
    if (!Mesh)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("NewObject<UStaticMesh> returned null (parent=%s, name=%s)."), *GetNameSafe(InParent), *InName.ToString());
        UE_LOG(LogTemp, Error, TEXT("[HL2BSPImporter] NewObject<UStaticMesh> failed."));
        bOutOperationCanceled = true;
        return nullptr;
    }

    // Create material slots matching polygon groups; use map when available
    Mesh->GetStaticMaterials().Reset();
    for (const FName& Slot : SlotNames)
    {
        UMaterialInterface* Mat = nullptr;
        if (UMaterialInterface** Found = GMaterialMap.Find(Slot.ToString()))
        {
            Mat = *Found;
        }
        // Avoid needing the full EMaterialDomain definition here
        if (!Mat)
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("No material mapped for slot '%s'; using default."), *Slot.ToString());
            Mat = UMaterial::GetDefaultMaterial(static_cast<EMaterialDomain>(0));
        }
        Mesh->GetStaticMaterials().Add(FStaticMaterial(Mat, Slot));
    }

    // Ensure MeshDescription arrays are compact before NTB compute
    {
        const int32 TriNum = MD.Triangles().Num();
        const int32 TriSize = MD.Triangles().GetArraySize();
        const int32 VertNum = MD.Vertices().Num();
        const int32 VertSize = MD.Vertices().GetArraySize();
        const int32 VINum = MD.VertexInstances().Num();
        const int32 VISize = MD.VertexInstances().GetArraySize();
        UE_LOG(LogHL2BSPImporter, Log, TEXT("MeshDesc sizes before compact: Tri=%d/%d Vert=%d/%d VI=%d/%d"), TriNum, TriSize, VertNum, VertSize, VINum, VISize);
        if (TriNum != TriSize || VertNum != VertSize || VINum != VISize)
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("MeshDesc arrays not compact; compacting before NTB compute."));
            FStaticMeshOperations::CompactMeshDescription(MD);
            const int32 TriNum2 = MD.Triangles().Num();
            const int32 TriSize2 = MD.Triangles().GetArraySize();
            const int32 VertNum2 = MD.Vertices().Num();
            const int32 VertSize2 = MD.Vertices().GetArraySize();
            const int32 VINum2 = MD.VertexInstances().Num();
            const int32 VISize2 = MD.VertexInstances().GetArraySize();
            UE_LOG(LogHL2BSPImporter, Log, TEXT("MeshDesc sizes after compact: Tri=%d/%d Vert=%d/%d VI=%d/%d"), TriNum2, TriSize2, VertNum2, VertSize2, VINum2, VISize2);
        }
    }

    // Compute normals/tangents from geometry (UE5.6 flags-based API)
    const int32 NumTris = MD.Triangles().Num();
    if (NumTris == 0)
    {
        UE_LOG(LogHL2BSPImporter, Warning, TEXT("MeshDescription has 0 triangles. Skipping tangent/normal computation."));
        UE_LOG(LogTemp, Warning, TEXT("[HL2BSPImporter] 0 triangles produced from BSP. Skipping NTB compute."));
    }
    else
    {
        FStaticMeshOperations::ComputeTangentsAndNormals(MD, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);
        UE_LOG(LogHL2BSPImporter, Log, TEXT("Computed normals/tangents for %d triangles."), NumTris);
    }
    if (Warn)
    {
        Warn->Logf(ELogVerbosity::Display, TEXT("HL2BSPImporter: Geometry ready. Building mesh (materials=%d, tris=%d)"), Mesh->GetStaticMaterials().Num(), MD.Triangles().Num());
    }

    // Configure Nanite before build
    Mesh->NaniteSettings.bEnabled = Sets->bBuildNanite;

    // Build from MeshDescription (UE5 path)
    TArray<const FMeshDescription*> Descs; Descs.Add(&MD);
    Mesh->BuildFromMeshDescriptions(Descs);
    UE_LOG(LogHL2BSPImporter, Log, TEXT("StaticMesh built from MeshDescription. LODs=%d Materials=%d"), Mesh->GetNumLODs(), Mesh->GetStaticMaterials().Num());
    if (Warn)
    {
        Warn->Logf(ELogVerbosity::Display, TEXT("HL2BSPImporter: Mesh built. LODs=%d Materials=%d"), Mesh->GetNumLODs(), Mesh->GetStaticMaterials().Num());
    }

    // Collision settings
    if (Sets->bImportCollision)
    {
        Mesh->CreateBodySetup();
        if (Mesh->GetBodySetup())
        {
            Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
            UE_LOG(LogHL2BSPImporter, Log, TEXT("Collision: Set to UseComplexAsSimple."));
        }
    }

    FAssetRegistryModule::AssetCreated(Mesh);
    Mesh->MarkPackageDirty();

    // Create Entities DataTable asset from BSP entities if available
    const TArray<FHL2Entity>& Entities = Bsp.GetEntities();
    if (Entities.Num() > 0)
    {
        FString EntityPkgName = InParent->GetName() + TEXT("_Entities");
        UPackage* EntPkg = CreatePackage(*EntityPkgName);
        UHL2EntityTable* Table = UHL2EntityTable::CreateFromEntities(EntPkg, Entities);
        if (Table)
        {
            FAssetRegistryModule::AssetCreated(Table);
            Table->MarkPackageDirty();
            UE_LOG(LogHL2BSPImporter, Log, TEXT("Created Entities DataTable: %s"), *Table->GetName());
        }
        else
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("Failed to create Entities DataTable for %d entities."), Entities.Num());
        }
    }
    else
    {
        UE_LOG(LogHL2BSPImporter, Log, TEXT("No entities found in BSP."));
    }

    bOutOperationCanceled = false;
    return Mesh;
}
