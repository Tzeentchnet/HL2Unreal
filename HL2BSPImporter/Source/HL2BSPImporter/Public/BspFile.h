#pragma once
#include "CoreMinimal.h"
#include "HL2BSPImporterTypes.h"

// Source-engine surface flags (subset). Faces with any of these are skipped from visible geometry.
namespace EHL2SurfFlag
{
    enum Type : uint32
    {
        Light       = 0x0001,
        Sky2D       = 0x0002,
        Sky         = 0x0004,
        Warp        = 0x0008,
        Trans       = 0x0010,
        NoPortal    = 0x0020,
        Trigger     = 0x0040,
        NoDraw      = 0x0080,
        Hint        = 0x0100,
        Skip        = 0x0200,
        NoLight     = 0x0400,
        BumpLight   = 0x0800,
        NoShadows   = 0x1000,
        NoDecals    = 0x2000,
        NoChop      = 0x4000,
        Hitbox      = 0x8000,

        // Bitmask of flags that mean "do not render this face as solid geometry"
        SkipRenderMask =
            NoDraw | Sky | Sky2D | Trigger | Hint | Skip,

        // Subset of SkipRenderMask that identifies sky faces specifically.
        // Phase A1 captures these face names before the SkipRenderMask filter
        // drops them so the skybox cubemap converter can emit a UTextureCube
        // for each unique skybox referenced by the map.
        SkyMask = Sky | Sky2D,
    };
}

struct FBspVertex
{
    FVector Position = FVector::ZeroVector;
    FVector2D UV = FVector2D::ZeroVector;
    FVector2D LightmapUV = FVector2D::ZeroVector;
};

struct FBspFace
{
    uint32 FirstVertex = 0;
    uint32 NumVertices = 0;
    FString TextureName;        // Source texture path for material lookup
    int32 DispInfoIndex = -1;   // >=0 if this face is a displacement base; not emitted as a brush face.
    uint8 Side = 0;             // dface_t.side
    uint32 SurfFlags = 0;       // dtexinfo_t.flags
};

struct FDispInfo
{
    FVector  StartPosition = FVector::ZeroVector;
    int32    Power      = 0;
    int32    VertStart  = 0;
    int32    MapFace    = -1;

    // Base-face quad corners (in Source/raw coordinates). Always 4 entries when valid.
    FVector   Corners[4]    = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
    FVector2D CornerUVs[4]  = { FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector };
    FString   TextureName;
};

struct FDispVert
{
    float Vector[3] = { 0.f, 0.f, 0.f };
    float Dist = 0.f;
    float Alpha = 0.f;
};

// Sub-model emitted from the BSP brush list. Model 0 is worldspawn (handled separately via
// GetWorldFirstFace/NumFaces); models 1..N-1 are brush entities (`func_door`, `func_brush`,
// water volumes, triggers, etc.) and are referenced by entities via `model "*N"`.
struct FBspBrushModel
{
    int32   ModelIndex = 0;     // raw index in DMODEL lump (>=1)
    int32   FirstFace  = 0;     // index into FBspFile::GetFaces()
    int32   NumFaces   = 0;     // contiguous face count in GetFaces()
    FVector Origin     = FVector::ZeroVector; // model.Origin (Source coords; usually 0)
    FVector Mins       = FVector::ZeroVector;
    FVector Maxs       = FVector::ZeroVector;
};

// One `prop_static` instance in raw Source coordinates. The factory transforms Origin/Angles
// into Unreal space before persisting them on the FHL2StaticProp DataTable row.
struct FBspStaticProp
{
    FString ModelName;                            // e.g. "models/props_c17/oildrum001.mdl"
    FVector Origin       = FVector::ZeroVector;   // Source coords, inches
    FVector Angles       = FVector::ZeroVector;   // pitch, yaw, roll (degrees)
    float   UniformScale = 1.f;                   // v11+; 1.0 otherwise
    int32   Skin         = 0;
    uint8   Solid        = 0;
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
    const TArray<FBspBrushModel>& GetBrushModels() const { return BrushModels; }
    const TArray<FBspStaticProp>& GetStaticProps() const { return StaticProps; }

    // Phase A1: deduplicated set of materials/skybox/<base> stems referenced by
    // SURF_SKY / SURF_SKY2D faces. The skybox converter looks up the six face
    // VMTs (<base>{bk,ft,lf,rt,up,dn}.vmt) under SourceContentRoots and emits
    // one UTextureCube per stem.
    const TSet<FString>& GetSkyTextureNames() const { return SkyTextureNames; }

    // Phase A5: raw bytes of LUMP_VISIBILITY (4). Empty when the BSP has no
    // visibility data (rare). The runtime UHL2VBSPInfo asset decompresses
    // per-leaf PVS bitmaps from this blob on demand.
    const TArray<uint8>& GetVisibilityBytes() const { return VisibilityBytes; }

    // Phase A5: per-leaf bounds extracted from LUMP_LEAFS (10), in raw Source
    // coordinates (BSP units, no Y-flip). The factory converts to Unreal cm
    // when emitting the UHL2VBSPInfo asset.
    const TArray<FBox>& GetLeafBounds() const { return LeafBounds; }

    // Raw bytes of LUMP_PAKFILE (40), already decompressed if the lump used LZMA.
    // Empty when the BSP carries no embedded pakfile. Format is a standard PKZIP archive.
    const TArray<uint8>& GetPakfileBytes() const { return PakfileBytes; }

    // Worldspawn (model 0) face index range within Faces. Other ranges belong to brush entities.
    int32 GetWorldFirstFace() const { return WorldFirstFace; }
    int32 GetWorldNumFaces()  const { return WorldNumFaces;  }

    int32 GetVersion() const { return Version; }

private:
    TArray<FBspVertex>     Vertices;
    TArray<FBspFace>       Faces;
    TArray<FDispInfo>      DispInfos;
    TArray<FDispVert>      DispVerts;
    TArray<FHL2Entity>     Entities;
    TArray<FBspBrushModel> BrushModels;
    TArray<FBspStaticProp> StaticProps;
    TArray<uint8>          PakfileBytes;
    TSet<FString>          SkyTextureNames;     // Phase A1
    TArray<uint8>          VisibilityBytes;     // Phase A5
    TArray<FBox>           LeafBounds;          // Phase A5

    int32 Version        = 0;
    int32 WorldFirstFace = 0;
    int32 WorldNumFaces  = 0;

    void ParseEntities(const FString& EntText, TArray<FHL2Entity>& Out) const;
};
