#include "HL2LevelSpawnLibrary.h"

#include "HL2BSPImporter.h"
#include "HL2EntityTable.h"

#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Editor.h"
#include "Engine/DataTable.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"

namespace
{
    UWorld* ResolveSpawnWorld(UObject* WorldContextObject)
    {
        if (WorldContextObject)
        {
            if (UWorld* World = WorldContextObject->GetWorld())
            {
                return World;
            }
        }

        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    FString CleanLabelToken(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("/"), TEXT("_"));
        Out.ReplaceInline(TEXT("\\"), TEXT("_"));
        Out.ReplaceInline(TEXT("."), TEXT("_"));
        Out.ReplaceInline(TEXT(" "), TEXT("_"));
        return Out.IsEmpty() ? FString(TEXT("Actor")) : Out;
    }

    FName MakeFolder(const FHL2LevelSpawnOptions& Options, const TCHAR* Leaf)
    {
        const FString Root = Options.FolderRoot.IsNone()
            ? FString(TEXT("HL2Imported"))
            : Options.FolderRoot.ToString();
        return FName(*(Root / Leaf));
    }

    UStaticMesh* LoadStaticMesh(const FSoftObjectPath& Path)
    {
        return Path.IsValid() ? Cast<UStaticMesh>(Path.TryLoad()) : nullptr;
    }

    AStaticMeshActor* SpawnStaticMeshActor(
        UWorld* World,
        UStaticMesh* Mesh,
        const FTransform& Transform,
        const FString& Label,
        FName Folder)
    {
        if (!World || !Mesh) { return nullptr; }

        FActorSpawnParameters Params;
        Params.ObjectFlags |= RF_Transactional;

        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, Params);
        if (!Actor) { return nullptr; }

        Actor->SetMobility(EComponentMobility::Static);
        if (UStaticMeshComponent* MeshComp = Actor->GetStaticMeshComponent())
        {
            MeshComp->SetStaticMesh(Mesh);
            MeshComp->SetMobility(EComponentMobility::Static);
        }

#if WITH_EDITOR
        Actor->SetActorLabel(Label, /*bMarkDirty=*/false);
        Actor->SetFolderPath(Folder);
#endif
        return Actor;
    }

    AActor* SpawnBasicLight(UWorld* World, const FHL2Entity& Entity, FName Folder)
    {
        if (!World) { return nullptr; }

        const FString ClassLower = Entity.Class.ToLower();
        UClass* LightClass = nullptr;
        if (ClassLower == TEXT("light"))
        {
            LightClass = APointLight::StaticClass();
        }
        else if (ClassLower == TEXT("light_spot"))
        {
            LightClass = ASpotLight::StaticClass();
        }
        else if (ClassLower == TEXT("light_environment"))
        {
            LightClass = ADirectionalLight::StaticClass();
        }
        if (!LightClass) { return nullptr; }

        FActorSpawnParameters Params;
        Params.ObjectFlags |= RF_Transactional;
        const FTransform Transform(Entity.Rotation, Entity.Origin);
        AActor* Actor = World->SpawnActor<AActor>(LightClass, Transform, Params);
        if (!Actor) { return nullptr; }

        if (ULightComponent* LightComponent = Actor->FindComponentByClass<ULightComponent>())
        {
            const FString* LightValue = Entity.KeyValues.Find(TEXT("_light"));
            if (!LightValue) { LightValue = Entity.KeyValues.Find(TEXT("_lightHDR")); }
            if (LightValue)
            {
                TArray<FString> Parts;
                LightValue->ParseIntoArrayWS(Parts);
                if (Parts.Num() >= 3)
                {
                    const float R = FMath::Clamp(FCString::Atof(*Parts[0]) / 255.f, 0.f, 1.f);
                    const float G = FMath::Clamp(FCString::Atof(*Parts[1]) / 255.f, 0.f, 1.f);
                    const float B = FMath::Clamp(FCString::Atof(*Parts[2]) / 255.f, 0.f, 1.f);
                    LightComponent->SetLightColor(FLinearColor(R, G, B));
                }
                if (Parts.Num() >= 4)
                {
                    LightComponent->SetIntensity(FMath::Max(0.f, FCString::Atof(*Parts[3])));
                }
            }
        }

#if WITH_EDITOR
        const FString BaseName = Entity.Name.IsEmpty() ? Entity.Class : Entity.Name;
        Actor->SetActorLabel(TEXT("HL2_") + CleanLabelToken(BaseName), /*bMarkDirty=*/false);
        Actor->SetFolderPath(Folder);
#endif
        return Actor;
    }

    void SelectSpawnedActors(const TArray<TObjectPtr<AActor>>& Actors)
    {
        if (!GEditor) { return; }
        GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
        for (AActor* Actor : Actors)
        {
            if (Actor)
            {
                GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/false);
            }
        }
        GEditor->NoteSelectionChange();
    }
}

FHL2LevelSpawnResult UHL2LevelSpawnLibrary::SpawnImportedActors(
    UObject* WorldContextObject,
    UDataTable* EntityTable,
    UDataTable* StaticPropTable,
    FHL2LevelSpawnOptions Options)
{
    FHL2LevelSpawnResult Result;
    UWorld* World = ResolveSpawnWorld(WorldContextObject);
    if (!World)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("SpawnImportedActors: no editor world available."));
        return Result;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("HL2BSPImporter", "SpawnImportedActors", "Spawn HL2 Imported Actors"));
    World->Modify();

    if (Options.bSpawnStaticProps && StaticPropTable)
    {
        if (StaticPropTable->GetRowStruct() != FHL2StaticPropTableRow::StaticStruct())
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("SpawnImportedActors: StaticPropTable has an unexpected row type."));
        }
        else
        {
            TArray<FHL2StaticPropTableRow*> Rows;
            StaticPropTable->GetAllRows(TEXT("HL2 static prop spawn"), Rows);
            for (const FHL2StaticPropTableRow* Row : Rows)
            {
                if (!Row) { ++Result.NumRowsSkipped; continue; }
                const FHL2StaticProp& Prop = Row->Prop;
                UStaticMesh* Mesh = LoadStaticMesh(Prop.StaticMeshAsset);
                if (!Mesh) { ++Result.NumRowsSkipped; continue; }

                const FTransform Transform(Prop.Rotation, Prop.Origin, FVector(Prop.UniformScale));
                const FString Label = TEXT("HL2_Static_") + CleanLabelToken(Prop.ModelName);
                if (AStaticMeshActor* Actor = SpawnStaticMeshActor(World, Mesh, Transform, Label, MakeFolder(Options, TEXT("StaticProps"))))
                {
                    Result.SpawnedActors.Add(Actor);
                    ++Result.NumStaticPropsSpawned;
                }
                else
                {
                    ++Result.NumRowsSkipped;
                }
            }
        }
    }

    if (EntityTable)
    {
        if (EntityTable->GetRowStruct() != FHL2EntityTableRow::StaticStruct())
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("SpawnImportedActors: EntityTable has an unexpected row type."));
        }
        else
        {
            TArray<FHL2EntityTableRow*> Rows;
            EntityTable->GetAllRows(TEXT("HL2 entity spawn"), Rows);
            for (const FHL2EntityTableRow* Row : Rows)
            {
                if (!Row) { ++Result.NumRowsSkipped; continue; }
                const FHL2Entity& Entity = Row->Entity;
                const FTransform Transform(Entity.Rotation, Entity.Origin);

                if (Options.bSpawnBrushEntities)
                {
                    if (UStaticMesh* BrushMesh = LoadStaticMesh(Entity.BrushMesh))
                    {
                        const FString LabelBase = Entity.Name.IsEmpty() ? Entity.Class : Entity.Name;
                        if (AStaticMeshActor* Actor = SpawnStaticMeshActor(World, BrushMesh, Transform, TEXT("HL2_Brush_") + CleanLabelToken(LabelBase), MakeFolder(Options, TEXT("BrushEntities"))))
                        {
                            Result.SpawnedActors.Add(Actor);
                            ++Result.NumBrushEntitiesSpawned;
                        }
                        else
                        {
                            ++Result.NumRowsSkipped;
                        }
                        continue;
                    }
                }

                if (Options.bSpawnEntityProps)
                {
                    if (UStaticMesh* PropMesh = LoadStaticMesh(Entity.PropMesh))
                    {
                        const FString LabelBase = Entity.Name.IsEmpty() ? Entity.Model : Entity.Name;
                        if (AStaticMeshActor* Actor = SpawnStaticMeshActor(World, PropMesh, Transform, TEXT("HL2_Prop_") + CleanLabelToken(LabelBase), MakeFolder(Options, TEXT("EntityProps"))))
                        {
                            Result.SpawnedActors.Add(Actor);
                            ++Result.NumEntityPropsSpawned;
                        }
                        else
                        {
                            ++Result.NumRowsSkipped;
                        }
                        continue;
                    }
                }

                if (Options.bSpawnBasicLights)
                {
                    if (AActor* LightActor = SpawnBasicLight(World, Entity, MakeFolder(Options, TEXT("Lights"))))
                    {
                        Result.SpawnedActors.Add(LightActor);
                        ++Result.NumLightsSpawned;
                    }
                }
            }
        }
    }

    if (Options.bSelectSpawnedActors && Result.SpawnedActors.Num() > 0)
    {
        SelectSpawnedActors(Result.SpawnedActors);
    }

    UE_LOG(LogHL2BSPImporter, Log,
        TEXT("SpawnImportedActors: staticProps=%d brushEntities=%d entityProps=%d lights=%d skipped=%d"),
        Result.NumStaticPropsSpawned, Result.NumBrushEntitiesSpawned,
        Result.NumEntityPropsSpawned, Result.NumLightsSpawned, Result.NumRowsSkipped);

    return Result;
}
