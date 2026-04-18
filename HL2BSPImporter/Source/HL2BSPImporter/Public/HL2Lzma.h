#pragma once
#include "CoreMinimal.h"

namespace HL2Lzma
{
    /**
     * Decompress a Source-engine LZMA-compressed BSP/pak lump payload.
     *
     * Source uses a custom 17-byte header prepended to a raw LZMA1 stream:
     *   uint32 id            // 'LZMA' = 0x414D5A4C ('L','Z','M','A' little-endian)
     *   uint32 actualSize    // uncompressed size in bytes
     *   uint32 lzmaSize      // compressed payload size (excludes this 17-byte header)
     *   uint8  properties[5] // LZMA properties (lc/lp/pb + dictSize)
     *
     * @param SourceBytes  Pointer to the start of the lump (i.e. the 17-byte header).
     * @param SourceSize   Total size of the compressed lump including the 17-byte header.
     * @param OutDecompressed Filled with `actualSize` bytes on success.
     * @param DebugName    Lump name for log messages.
     * @return true on success.
     */
    bool DecompressSourceLump(
        const uint8* SourceBytes,
        int64 SourceSize,
        TArray<uint8>& OutDecompressed,
        const TCHAR* DebugName);

    /** Returns true if the buffer begins with a Source-LZMA header. */
    bool IsSourceLzma(const uint8* SourceBytes, int64 SourceSize);
}
