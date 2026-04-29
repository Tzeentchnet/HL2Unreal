#include "HL2VBSPInfo.h"

bool UHL2VBSPInfo::IsLeafVisibleFrom(int32 ObserverLeaf, int32 TargetLeaf) const
{
    if (ObserverLeaf == TargetLeaf) { return true; }
    if (ObserverLeaf < 0 || TargetLeaf < 0) { return false; }
    if (VisibilityBytes.Num() < 4)         { return false; }

    // Source visibility lump layout:
    //   int32  NumClusters
    //   int32  ClusterOffsets[NumClusters][2]    // [PVS offset, PAS offset]
    //   bytes  RLE-compressed bitmaps at the offsets above
    int32 NumClusters = 0;
    FMemory::Memcpy(&NumClusters, VisibilityBytes.GetData(), sizeof(int32));
    if (NumClusters <= 0) { return false; }

    // dleaf_t carries `Cluster` (int16) per leaf — without that mapping at
    // hand we can't translate leaf index → cluster. The current asset only
    // stores VisibilityBytes + LeafBounds; we treat the leaf index AS the
    // cluster index, which is correct only when the map's leaf-to-cluster
    // mapping is identity (the common case for HL2 maps). Future revisions
    // should store the leaf→cluster table when the consumer needs strict
    // correctness.
    if (ObserverLeaf >= NumClusters || TargetLeaf >= NumClusters) { return false; }

    const int64 OffsetTableEnd = 4 + static_cast<int64>(NumClusters) * 8;
    if (OffsetTableEnd > VisibilityBytes.Num()) { return false; }

    int32 PvsOffset = 0;
    FMemory::Memcpy(&PvsOffset, VisibilityBytes.GetData() + 4 + ObserverLeaf * 8, sizeof(int32));
    if (PvsOffset < 0 || PvsOffset >= VisibilityBytes.Num()) { return false; }

    // RLE decode just enough to reach TargetLeaf's bit. Source's RLE: bytes
    // are read sequentially; a 0 byte means "the next byte is a count of
    // zero bytes to skip" (i.e. that many clusters with no PVS bit set).
    const uint8* P  = VisibilityBytes.GetData() + PvsOffset;
    const uint8* End = VisibilityBytes.GetData() + VisibilityBytes.Num();

    int32 ClusterIdx = 0;
    while (ClusterIdx < NumClusters && P < End)
    {
        const uint8 Byte = *P++;
        if (Byte == 0)
        {
            if (P >= End) { return false; }
            const uint8 Run = *P++;
            ClusterIdx += static_cast<int32>(Run) * 8;
            continue;
        }
        // Byte covers ClusterIdx .. ClusterIdx+7.
        if (TargetLeaf >= ClusterIdx && TargetLeaf < ClusterIdx + 8)
        {
            const int32 BitInByte = TargetLeaf - ClusterIdx;
            return (Byte & (1u << BitInByte)) != 0;
        }
        ClusterIdx += 8;
    }
    return false;
}
