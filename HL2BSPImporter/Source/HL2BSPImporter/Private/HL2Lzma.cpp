#include "HL2Lzma.h"
#include "HL2BSPImporter.h"

// Bundle the LZMA SDK decoder (public domain, Igor Pavlov) directly into this TU.
// LzmaDec.c.inl is excluded from UBT's automatic source list by its non-compiled extension.
THIRD_PARTY_INCLUDES_START
extern "C"
{
    #include "ThirdParty/Lzma/LzmaDec.h"
    #include "ThirdParty/Lzma/LzmaDec.c.inl"
}
THIRD_PARTY_INCLUDES_END

namespace
{
    constexpr uint32 SOURCE_LZMA_ID  = 0x414D5A4Cu; // 'LZMA' little-endian
    constexpr int32  SOURCE_LZMA_HDR = 17;          // 4 + 4 + 4 + 5
    constexpr int64  MAX_DECOMPRESSED = 256ll * 1024 * 1024; // sanity cap (matches BspFile.cpp)

    void* LzAlloc(ISzAllocPtr, size_t Size) { return FMemory::Malloc(Size); }
    void  LzFree (ISzAllocPtr, void* Addr)  { if (Addr) FMemory::Free(Addr); }
    const ISzAlloc GLzAlloc = { &LzAlloc, &LzFree };
}

bool HL2Lzma::IsSourceLzma(const uint8* SourceBytes, int64 SourceSize)
{
    if (!SourceBytes || SourceSize < SOURCE_LZMA_HDR) { return false; }
    uint32 Id = 0;
    FMemory::Memcpy(&Id, SourceBytes, sizeof(uint32));
    return Id == SOURCE_LZMA_ID;
}

bool HL2Lzma::DecompressSourceLump(
    const uint8* SourceBytes,
    int64 SourceSize,
    TArray<uint8>& OutDecompressed,
    const TCHAR* DebugName)
{
    OutDecompressed.Reset();

    if (!SourceBytes || SourceSize < SOURCE_LZMA_HDR)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("LZMA lump %s too small for header (%lld bytes)."), DebugName, (long long)SourceSize);
        return false;
    }

    uint32 Id = 0, ActualSize = 0, LzmaSize = 0;
    FMemory::Memcpy(&Id,         SourceBytes + 0, sizeof(uint32));
    FMemory::Memcpy(&ActualSize, SourceBytes + 4, sizeof(uint32));
    FMemory::Memcpy(&LzmaSize,   SourceBytes + 8, sizeof(uint32));

    if (Id != SOURCE_LZMA_ID)
    {
        UE_LOG(LogHL2BSPImporter, Error,
            TEXT("LZMA lump %s has wrong magic 0x%08x (expected 'LZMA' = 0x%08x)."),
            DebugName, Id, SOURCE_LZMA_ID);
        return false;
    }
    if (ActualSize == 0 || LzmaSize == 0)
    {
        UE_LOG(LogHL2BSPImporter, Error,
            TEXT("LZMA lump %s has zero size (actual=%u lzma=%u)."), DebugName, ActualSize, LzmaSize);
        return false;
    }
    if (static_cast<int64>(ActualSize) > MAX_DECOMPRESSED)
    {
        UE_LOG(LogHL2BSPImporter, Error,
            TEXT("LZMA lump %s decompressed size %u exceeds sanity cap %lld."),
            DebugName, ActualSize, (long long)MAX_DECOMPRESSED);
        return false;
    }
    const int64 PayloadEnd = static_cast<int64>(SOURCE_LZMA_HDR) + static_cast<int64>(LzmaSize);
    if (PayloadEnd > SourceSize)
    {
        UE_LOG(LogHL2BSPImporter, Error,
            TEXT("LZMA lump %s payload %u extends past lump (have %lld bytes after header)."),
            DebugName, LzmaSize, (long long)(SourceSize - SOURCE_LZMA_HDR));
        return false;
    }

    const uint8* Props   = SourceBytes + 12;       // 5 bytes
    const uint8* Payload = SourceBytes + SOURCE_LZMA_HDR;

    OutDecompressed.SetNumUninitialized(static_cast<int32>(ActualSize));

    SizeT       DestLen = ActualSize;
    SizeT       SrcLen  = LzmaSize;
    ELzmaStatus Status  = LZMA_STATUS_NOT_SPECIFIED;

    const SRes Res = LzmaDecode(
        OutDecompressed.GetData(), &DestLen,
        Payload,                   &SrcLen,
        Props, LZMA_PROPS_SIZE,
        LZMA_FINISH_ANY, &Status, &GLzAlloc);

    if (Res != SZ_OK)
    {
        UE_LOG(LogHL2BSPImporter, Error,
            TEXT("LZMA decode failed for lump %s (SRes=%d, status=%d)."), DebugName, Res, (int32)Status);
        OutDecompressed.Reset();
        return false;
    }
    if (static_cast<int64>(DestLen) != static_cast<int64>(ActualSize))
    {
        UE_LOG(LogHL2BSPImporter, Error,
            TEXT("LZMA decode for lump %s produced %lld bytes, expected %u."),
            DebugName, (long long)DestLen, ActualSize);
        OutDecompressed.Reset();
        return false;
    }

    UE_LOG(LogHL2BSPImporter, Verbose,
        TEXT("LZMA lump %s decompressed: %u -> %u bytes."), DebugName, LzmaSize, ActualSize);
    return true;
}
