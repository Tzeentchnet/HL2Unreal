#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "HL2VBSPInfo.generated.h"

// Phase A5 — runtime BSP visibility / leaf bounds asset.
//
// Holds the raw bytes of LUMP_VISIBILITY (Source PVS, RLE-compressed bitmap
// per leaf cluster) and the per-leaf AABBs from LUMP_LEAFS so runtime systems
// can answer "is leaf B visible from leaf A?" without parsing the BSP again.
//
// The current consumer set is intentionally empty — Phase 13 item 5 (World
// Partition import-as-level) and any future per-PVS streaming work would be
// the natural readers. Emission is gated by the bExportVBSPInfoAsset setting
// (default-off) since there's no mandatory runtime cost today.
UCLASS(BlueprintType)
class HL2BSPIMPORTER_API UHL2VBSPInfo : public UDataAsset
{
    GENERATED_BODY()
public:
    // Raw LUMP_VISIBILITY bytes. The first 4 bytes are NumClusters, then
    // (NumClusters * 8) bytes of (PVSOffset, PASOffset) pairs, then the
    // RLE-encoded bitmaps at the referenced offsets. We don't decompress at
    // import time; queries decompress on demand.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VBSP")
    TArray<uint8> VisibilityBytes;

    // Per-leaf bounds in Unreal cm (already coord-transformed by the factory
    // emitting this asset). Index matches the BSP leaf index; index 0 is the
    // outside-the-world solid leaf and is intentionally degenerate.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VBSP")
    TArray<FBox> LeafBounds;

    // Test whether a leaf is in another leaf's PVS. Decompresses the observer
    // leaf's bitmap on demand; safe to call with out-of-range indices (returns
    // false). Returns true for self-visibility (ObserverLeaf == TargetLeaf is
    // always visible).
    UFUNCTION(BlueprintCallable, Category = "VBSP")
    bool IsLeafVisibleFrom(int32 ObserverLeaf, int32 TargetLeaf) const;
};
