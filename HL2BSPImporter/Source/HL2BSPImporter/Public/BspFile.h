#pragma once
#include "CoreMinimal.h"
#include "HL2BSPImporterTypes.h"

// Minimal placeholder BSP structures to allow compilation.

struct FBspVertex
{
    FVector Position = FVector::ZeroVector;
    FVector2D UV = FVector2D::ZeroVector;
};

struct FBspFace
{
    uint32 FirstVertex = 0;
    uint32 NumVertices = 0;
    FString TextureName; // Source texture name for material lookup
};

struct FDispInfo
{
    int32 Power = 0;
    int32 VertStart = 0;
    int32 MapFace = -1;
};

struct FDispVert
{
    float Vector[3] = {0.f, 0.f, 0.f};
};

class FBspFile
{
public:
    bool LoadFromFile(const FString& Filename);

    const TArray<FBspVertex>& GetVertices() const { return Vertices; }
    const TArray<FBspFace>& GetFaces() const { return Faces; }
    const TArray<FDispInfo>& GetDispInfos() const { return DispInfos; }
    const TArray<FDispVert>& GetDispVerts() const { return DispVerts; }
    const TArray<FHL2Entity>& GetEntities() const { return Entities; }

private:
    TArray<FBspVertex> Vertices;
    TArray<FBspFace> Faces;
    TArray<FDispInfo> DispInfos;
    TArray<FDispVert> DispVerts;
    TArray<FHL2Entity> Entities;
};
