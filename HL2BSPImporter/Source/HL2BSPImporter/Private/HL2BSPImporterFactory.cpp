#include "HL2BSPImporterFactory.h"
#include "BspFile.h"
#include "HL2EntityTable.h"
#include "HL2BSPImporterSettings.h"
#include "Engine/StaticMesh.h"
#include "RawMesh.h"
#include "StaticMeshAttributes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

static TMap<FString, UMaterialInterface*> GMaterialMap;

static FRawMesh BuildRawMeshFromBSP(const FBspFile& Bsp)
{
    FRawMesh RM;
    const auto& Verts = Bsp.GetVertices();
    const auto& Faces = Bsp.GetFaces();

    /* brushes */
    for (const auto& F : Faces)
    {
        uint32 NumTris = F.NumVertices - 2;
        for (uint32 t = 0; t < NumTris; ++t)
        {
            uint32 I0 = F.FirstVertex;
            uint32 I1 = F.FirstVertex + t + 1;
            uint32 I2 = F.FirstVertex + t + 2;
            RM.VertexPositions.Append({ Verts[I0].Position, Verts[I1].Position, Verts[I2].Position });
            RM.WedgeIndices.Append({ RM.WedgeIndices.Num(), RM.WedgeIndices.Num(), RM.WedgeIndices.Num() });
            RM.WedgeTexCoords[0].Append({ Verts[I0].UV, Verts[I1].UV, Verts[I2].UV });
            RM.WedgeTangentZ.Append({ FVector::UpVector, FVector::UpVector, FVector::UpVector });
            RM.FaceMaterialIndices.Add(0);
            RM.FaceSmoothingMasks.Add(0xFFFFFFFF);
        }
    }

    /* displacements */
    const auto& Disps = Bsp.GetDispInfos();
    for (const auto& DI : Disps)
    {
        int32 Side = (1 << DI.Power) + 1;
        int32 Total = Side * Side;
        const auto& DV = Bsp.GetDispVerts();
        TArray<FVector> DispPos;
        DispPos.SetNum(Total);
        for (int32 i = 0; i < Total; ++i)
        {
            FVector P(DV[DI.VertStart + i].Vector[0], -DV[DI.VertStart + i].Vector[1], DV[DI.VertStart + i].Vector[2]);
            P *= 2.54f;
            DispPos[i] = P;
        }
        int32 Base = RM.VertexPositions.Num();
        for (int32 y = 0; y < Side; ++y)
            for (int32 x = 0; x < Side; ++x)
            {
                RM.VertexPositions.Add(DispPos[y * Side + x]);
                RM.WedgeTexCoords[0].Add(FVector2D(x / float(Side - 1), y / float(Side - 1)));
                RM.WedgeTangentZ.Add(FVector::UpVector);
            }
        for (int32 y = 0; y < Side - 1; ++y)
            for (int32 x = 0; x < Side - 1; ++x)
            {
                int A = Base + y * Side + x;
                int B = A + 1;
                int C = Base + (y + 1) * Side + x + 1;
                int D = Base + (y + 1) * Side + x;
                RM.WedgeIndices.Append({ A, B, C, A, C, D });
                RM.FaceMaterialIndices.Append({ 0, 0 });
                RM.FaceSmoothingMasks.Append({ 0xFFFFFFFF, 0xFFFFFFFF });
            }
    }
    return RM;
}

UHL2BSPImporterFactory::UHL2BSPImporterFactory()
{
    bEditorImport = true;
    SupportedClass = UStaticMesh::StaticClass();
    Formats.Add(TEXT("bsp;HL2 Map"));
}

bool UHL2BSPImporterFactory::FactoryCanImport(const FString& Filename)
{
    return Filename.EndsWith(TEXT(".bsp"));
}

UObject* UHL2BSPImporterFactory::FactoryCreateFile(UClass*, UObject* InParent, FName InName,
                                                   const FString& Filename, const TCHAR*,
                                                   FFeedbackContext* Warn, bool& bCancel)
{
    FBspFile Bsp;
    if (!Bsp.LoadFromFile(Filename)) return nullptr;

    GMaterialMap = LoadMaterialMap();

    const UHL2BSPImporterSettings* Sets = GetDefault<UHL2BSPImporterSettings>();
    FRawMesh RM = BuildRawMeshFromBSP(Bsp);

    FString PkgName = FPackageName::GetLongPackagePath(InParent->GetName()) / InName.ToString();
    UPackage* Pkg = CreatePackage(*PkgName);
    UStaticMesh* Mesh = NewObject<UStaticMesh>(Pkg, InName, RF_Public | RF_Standalone);

    Mesh->AddSourceModel();
    FStaticMeshSourceModel& Src = Mesh->GetSourceModel(0);
    Src.BuildSettings.bRecomputeNormals = true;
    Src.BuildSettings.bRecomputeTangents = true;
    Src.BuildSettings.bGenerateLightmapUVs = true;
    Src.SaveRawMesh(RM);

    Mesh->StaticMaterials.Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface)));
    if (Sets->bBuildNanite) Mesh->NaniteSettings.bEnabled = true;
    Mesh->Build();

    FAssetRegistryModule::AssetCreated(Mesh);
    return Mesh;
}