#pragma once
#include "CoreMinimal.h"

namespace HL2Pak
{
    struct FExtractStats
    {
        int32 NumExtracted        = 0;  // entries written to disk (STORE or DEFLATE)
        int32 NumSkippedDeflate   = 0;  // legacy counter; always 0 now that DEFLATE is supported
        int32 NumSkippedOther     = 0;  // unknown compression method or directory entry
        int32 NumSkippedUnsafe    = 0;  // path traversal / absolute / oversized
        int32 NumFailed           = 0;  // disk write failures or DEFLATE inflate errors
        int64 TotalBytesExtracted = 0;
    };

    /**
     * Extract a Source-engine embedded pakfile (a standard PKZIP archive carried in BSP lump 40)
     * into `DestDir`. STORE (method 0) and DEFLATE (method 8) entries are written; other methods
     * are counted in stats and skipped with a warning.
     *
     * Paths are sanitised: backslashes → forward slashes, drive letters / leading slashes /
     * `..` segments are rejected. Files are written with their original (case-preserved) names
     * underneath `DestDir`, but a lower-cased mirror table is also produced for case-insensitive
     * lookups by callers that expect Source's case-insensitive content roots.
    *
    * Extraction is capped at 256 MiB per entry and 2 GiB total per pakfile. Entries that would
    * exceed those caps are skipped and counted in OutStats rather than written.
     *
     * @param PakBytes  Raw bytes of the pakfile lump (already decompressed if the lump was LZMA).
     * @param DestDir   Filesystem directory to receive extracted files. Created if missing.
     * @param OutStats  Filled with per-import counts.
     * @return true if the archive was structurally valid (even if some entries were skipped).
     */
    bool ExtractToDirectory(
        TArrayView<const uint8> PakBytes,
        const FString& DestDir,
        FExtractStats& OutStats);
}
