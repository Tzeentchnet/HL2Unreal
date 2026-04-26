#include "HL2PakFile.h"
#include "HL2BSPImporter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

// PKZIP reader for Source's embedded pakfile lump.
//
// Reference: APPNOTE.TXT (PKWARE ZIP File Format Specification).
// We only need read-only support for the subset Source actually emits with bspzip:
//   - 32-bit (no ZIP64) archives
//   - STORE (method 0) and (eventually) DEFLATE (method 8)
//   - No encryption, no spanning, no central-directory encryption
//
// Parsing strategy:
//   1. Locate End-Of-Central-Directory record by scanning back from EOF for sig 0x06054b50.
//   2. Walk the central directory at EOCD.cdOffset (one CDH per entry; sig 0x02014b50).
//   3. For each entry, seek to LFH at cdh.localHeaderOffset (sig 0x04034b50), skip past its
//      filename + extra fields, and the file payload follows. (Per APPNOTE 4.4.4, the LFH may
//      have 0-length filename/extra unique to itself; we trust LFH's own lengths.)

namespace
{
    constexpr uint32 SIG_LFH  = 0x04034b50u;
    constexpr uint32 SIG_CDH  = 0x02014b50u;
    constexpr uint32 SIG_EOCD = 0x06054b50u;

    constexpr int32 EOCD_FIXED_SIZE = 22;     // EOCD record without comment
    constexpr int32 EOCD_MAX_SCAN   = 65535 + EOCD_FIXED_SIZE; // max comment is uint16

    constexpr int64 MAX_ENTRY_BYTES = 256ll * 1024 * 1024; // sanity cap per entry
    constexpr int64 MAX_TOTAL_EXTRACTED_BYTES = 2ll * 1024 * 1024 * 1024; // per pak extraction cap
    static_assert(sizeof(uInt) >= 4, "zlib uInt must hold capped 256 MiB entry sizes");

    template<typename T>
    bool ReadAt(TArrayView<const uint8> Buf, int64 Offset, T& Out)
    {
        if (Offset < 0 || Offset + (int64)sizeof(T) > Buf.Num()) { return false; }
        FMemory::Memcpy(&Out, Buf.GetData() + Offset, sizeof(T));
        return true;
    }

    // Reject absolute / drive-letter / parent-traversal paths. Normalise separators.
    bool SanitiseEntryPath(const FString& InPath, FString& OutRel)
    {
        if (InPath.IsEmpty()) { return false; }
        FString P = InPath.Replace(TEXT("\\"), TEXT("/"));
        // Drive letter (e.g. "C:") or absolute path.
        if (P.StartsWith(TEXT("/"))) { return false; }
        if (P.Len() >= 2 && P[1] == TEXT(':')) { return false; }
        // Reject any ".." segment.
        TArray<FString> Parts;
        P.ParseIntoArray(Parts, TEXT("/"), /*InCullEmpty=*/true);
        for (const FString& S : Parts)
        {
            if (S == TEXT("..")) { return false; }
        }
        if (Parts.Num() == 0) { return false; }
        OutRel = FString::Join(Parts, TEXT("/"));
        return true;
    }

    bool WouldExceedAggregateCap(int64 CurrentTotal, uint32 EntryBytes)
    {
        const int64 Entry64 = static_cast<int64>(EntryBytes);
        return Entry64 < 0 || CurrentTotal < 0 || Entry64 > MAX_TOTAL_EXTRACTED_BYTES ||
            CurrentTotal > MAX_TOTAL_EXTRACTED_BYTES - Entry64;
    }
}

bool HL2Pak::ExtractToDirectory(
    TArrayView<const uint8> PakBytes,
    const FString& DestDir,
    HL2Pak::FExtractStats& OutStats)
{
    OutStats = FExtractStats{};

    if (PakBytes.Num() < EOCD_FIXED_SIZE)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Pakfile too small for EOCD (%d bytes)."), PakBytes.Num());
        return false;
    }

    // 1. Locate EOCD by scanning backwards.
    int64 EocdOfs = -1;
    {
        const int64 ScanStart = FMath::Max<int64>(0, PakBytes.Num() - EOCD_MAX_SCAN);
        for (int64 i = PakBytes.Num() - EOCD_FIXED_SIZE; i >= ScanStart; --i)
        {
            uint32 Sig = 0;
            if (ReadAt(PakBytes, i, Sig) && Sig == SIG_EOCD)
            {
                EocdOfs = i;
                break;
            }
        }
    }
    if (EocdOfs < 0)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Pakfile EOCD signature not found."));
        return false;
    }

#pragma pack(push, 1)
    struct FEocd
    {
        uint32 Sig;
        uint16 DiskNum;
        uint16 CdStartDisk;
        uint16 NumEntriesThisDisk;
        uint16 NumEntriesTotal;
        uint32 CdSize;
        uint32 CdOffset;
        uint16 CommentLen;
    };
    static_assert(sizeof(FEocd) == 22, "FEocd must be 22 bytes");

    struct FCdh
    {
        uint32 Sig;
        uint16 VersionMadeBy;
        uint16 VersionNeeded;
        uint16 BitFlag;
        uint16 Method;
        uint16 ModTime;
        uint16 ModDate;
        uint32 Crc32;
        uint32 CompressedSize;
        uint32 UncompressedSize;
        uint16 NameLen;
        uint16 ExtraLen;
        uint16 CommentLen;
        uint16 DiskStart;
        uint16 InternalAttr;
        uint32 ExternalAttr;
        uint32 LocalHeaderOffset;
    };
    static_assert(sizeof(FCdh) == 46, "FCdh must be 46 bytes");

    struct FLfh
    {
        uint32 Sig;
        uint16 VersionNeeded;
        uint16 BitFlag;
        uint16 Method;
        uint16 ModTime;
        uint16 ModDate;
        uint32 Crc32;
        uint32 CompressedSize;
        uint32 UncompressedSize;
        uint16 NameLen;
        uint16 ExtraLen;
    };
    static_assert(sizeof(FLfh) == 30, "FLfh must be 30 bytes");
#pragma pack(pop)

    FEocd Eocd{};
    if (!ReadAt(PakBytes, EocdOfs, Eocd))
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Pakfile EOCD read failed at %lld."), (long long)EocdOfs);
        return false;
    }
    if (Eocd.DiskNum != 0 || Eocd.CdStartDisk != 0 || Eocd.NumEntriesThisDisk != Eocd.NumEntriesTotal)
    {
        UE_LOG(LogHL2BSPImporter, Error, TEXT("Pakfile is multi-disk; not supported."));
        return false;
    }
    if ((int64)Eocd.CdOffset + (int64)Eocd.CdSize > PakBytes.Num())
    {
        UE_LOG(LogHL2BSPImporter, Error,
            TEXT("Pakfile central directory overruns archive (cdOfs=%u cdSize=%u total=%d)."),
            Eocd.CdOffset, Eocd.CdSize, PakBytes.Num());
        return false;
    }

    if (!IFileManager::Get().DirectoryExists(*DestDir))
    {
        if (!IFileManager::Get().MakeDirectory(*DestDir, /*Tree=*/true))
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("Pakfile: failed to create extract dir '%s'."), *DestDir);
            return false;
        }
    }

    // 2. Walk central directory.
    int64 Cursor = (int64)Eocd.CdOffset;
    const int64 CdEnd = Cursor + (int64)Eocd.CdSize;

    for (uint16 i = 0; i < Eocd.NumEntriesTotal; ++i)
    {
        FCdh Cdh{};
        if (!ReadAt(PakBytes, Cursor, Cdh) || Cdh.Sig != SIG_CDH)
        {
            UE_LOG(LogHL2BSPImporter, Error,
                TEXT("Pakfile central-directory entry %u has bad signature at %lld."), i, (long long)Cursor);
            return false;
        }
        const int64 CdhEnd = Cursor + (int64)sizeof(FCdh) + Cdh.NameLen + Cdh.ExtraLen + Cdh.CommentLen;
        if (CdhEnd > CdEnd)
        {
            UE_LOG(LogHL2BSPImporter, Error, TEXT("Pakfile CDH %u overruns central directory."), i);
            return false;
        }

        FString EntryName;
        if (Cdh.NameLen > 0)
        {
            const ANSICHAR* NameBytes = reinterpret_cast<const ANSICHAR*>(PakBytes.GetData() + Cursor + sizeof(FCdh));
            EntryName = FString(static_cast<int32>(Cdh.NameLen), NameBytes);
        }
        Cursor = CdhEnd;

        // Skip directory entries (zero-byte, name ends in '/').
        if (Cdh.UncompressedSize == 0 && (EntryName.IsEmpty() || EntryName.EndsWith(TEXT("/")) || EntryName.EndsWith(TEXT("\\"))))
        {
            ++OutStats.NumSkippedOther;
            continue;
        }

        FString RelPath;
        if (!SanitiseEntryPath(EntryName, RelPath))
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("Pakfile: rejected unsafe entry path '%s'."), *EntryName);
            ++OutStats.NumSkippedUnsafe;
            continue;
        }
        if ((int64)Cdh.UncompressedSize > MAX_ENTRY_BYTES || (int64)Cdh.CompressedSize > MAX_ENTRY_BYTES)
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("Pakfile: entry '%s' too large (comp=%u, uncomp=%u); skipping."),
                *RelPath, Cdh.CompressedSize, Cdh.UncompressedSize);
            ++OutStats.NumSkippedUnsafe;
            continue;
        }
        if (WouldExceedAggregateCap(OutStats.TotalBytesExtracted, Cdh.UncompressedSize))
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("Pakfile: extracting '%s' would exceed aggregate cap %lld bytes (current=%lld, entry=%u); skipping."),
                *RelPath, (long long)MAX_TOTAL_EXTRACTED_BYTES, (long long)OutStats.TotalBytesExtracted, Cdh.UncompressedSize);
            ++OutStats.NumSkippedUnsafe;
            continue;
        }
        if (Cdh.BitFlag & 0x1)
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("Pakfile: encrypted entry '%s' is not supported; skipping."), *RelPath);
            ++OutStats.NumSkippedOther;
            continue;
        }

        if (Cdh.Method == 8) // DEFLATE
        {
            // Read local file header to find payload start.
            FLfh Lfh{};
            if (!ReadAt(PakBytes, (int64)Cdh.LocalHeaderOffset, Lfh) || Lfh.Sig != SIG_LFH)
            {
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Pakfile: DEFLATE entry '%s' has bad/missing local header at %u; skipping."),
                    *RelPath, Cdh.LocalHeaderOffset);
                ++OutStats.NumFailed;
                continue;
            }
            const int64 PayloadOfs = (int64)Cdh.LocalHeaderOffset + (int64)sizeof(FLfh) + Lfh.NameLen + Lfh.ExtraLen;
            const int64 PayloadEnd = PayloadOfs + (int64)Cdh.CompressedSize;
            if (PayloadOfs < 0 || PayloadEnd > PakBytes.Num())
            {
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Pakfile: DEFLATE entry '%s' payload OOB (ofs=%lld end=%lld total=%d); skipping."),
                    *RelPath, (long long)PayloadOfs, (long long)PayloadEnd, PakBytes.Num());
                ++OutStats.NumFailed;
                continue;
            }

            // Raw DEFLATE (no zlib header) — windowBits = -15.
            TArray<uint8> Inflated;
            // Safe: both sizes were checked against MAX_ENTRY_BYTES, which is below MAX_int32 and uInt max.
            Inflated.SetNumUninitialized(static_cast<int32>(Cdh.UncompressedSize));

            z_stream Strm{};
            Strm.next_in   = const_cast<Bytef*>(PakBytes.GetData() + PayloadOfs);
            Strm.avail_in  = static_cast<uInt>(Cdh.CompressedSize);
            Strm.next_out  = Inflated.GetData();
            Strm.avail_out = static_cast<uInt>(Cdh.UncompressedSize);

            if (inflateInit2(&Strm, -MAX_WBITS) != Z_OK)
            {
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Pakfile: DEFLATE entry '%s' inflateInit2 failed; skipping."), *RelPath);
                ++OutStats.NumFailed;
                continue;
            }

            const int InflateRes = inflate(&Strm, Z_FINISH);
            const uLong Produced = Strm.total_out;
            inflateEnd(&Strm);

            if (InflateRes != Z_STREAM_END || Produced != Cdh.UncompressedSize)
            {
                UE_LOG(LogHL2BSPImporter, Warning,
                    TEXT("Pakfile: DEFLATE entry '%s' inflate failed (res=%d, produced=%lu, expected=%u); skipping."),
                    *RelPath, InflateRes, (unsigned long)Produced, Cdh.UncompressedSize);
                ++OutStats.NumFailed;
                continue;
            }

            const FString OutPath = FPaths::Combine(DestDir, RelPath);
            const FString OutDir  = FPaths::GetPath(OutPath);
            if (!OutDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutDir))
            {
                IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);
            }
            if (!FFileHelper::SaveArrayToFile(Inflated, *OutPath))
            {
                UE_LOG(LogHL2BSPImporter, Warning, TEXT("Pakfile: failed to write '%s'."), *OutPath);
                ++OutStats.NumFailed;
                continue;
            }
            ++OutStats.NumExtracted;
            OutStats.TotalBytesExtracted += Cdh.UncompressedSize;
            continue;
        }
        if (Cdh.Method != 0) // not STORE
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("Pakfile: entry '%s' uses unsupported compression method %u; skipping."),
                *RelPath, Cdh.Method);
            ++OutStats.NumSkippedOther;
            continue;
        }
        if (Cdh.CompressedSize != Cdh.UncompressedSize)
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("Pakfile: STORE entry '%s' has mismatched sizes (comp=%u uncomp=%u); skipping."),
                *RelPath, Cdh.CompressedSize, Cdh.UncompressedSize);
            ++OutStats.NumFailed;
            continue;
        }

        // 3. Read local file header to find payload start.
        FLfh Lfh{};
        if (!ReadAt(PakBytes, (int64)Cdh.LocalHeaderOffset, Lfh) || Lfh.Sig != SIG_LFH)
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("Pakfile: entry '%s' has bad/missing local header at %u; skipping."),
                *RelPath, Cdh.LocalHeaderOffset);
            ++OutStats.NumFailed;
            continue;
        }
        const int64 PayloadOfs = (int64)Cdh.LocalHeaderOffset + (int64)sizeof(FLfh) + Lfh.NameLen + Lfh.ExtraLen;
        const int64 PayloadEnd = PayloadOfs + (int64)Cdh.UncompressedSize; // STORE: comp == uncomp, verified above
        if (PayloadOfs < 0 || PayloadEnd > PakBytes.Num())
        {
            UE_LOG(LogHL2BSPImporter, Warning,
                TEXT("Pakfile: entry '%s' payload out of bounds (ofs=%lld end=%lld total=%d); skipping."),
                *RelPath, (long long)PayloadOfs, (long long)PayloadEnd, PakBytes.Num());
            ++OutStats.NumFailed;
            continue;
        }

        const FString OutPath = FPaths::Combine(DestDir, RelPath);
        const FString OutDir  = FPaths::GetPath(OutPath);
        if (!OutDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutDir))
        {
            IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);
        }
        TArrayView<const uint8> Payload(PakBytes.GetData() + PayloadOfs, static_cast<int32>(Cdh.UncompressedSize));
        if (!FFileHelper::SaveArrayToFile(TArray<uint8>(Payload.GetData(), Payload.Num()), *OutPath))
        {
            UE_LOG(LogHL2BSPImporter, Warning, TEXT("Pakfile: failed to write '%s'."), *OutPath);
            ++OutStats.NumFailed;
            continue;
        }
        ++OutStats.NumExtracted;
        OutStats.TotalBytesExtracted += Cdh.UncompressedSize;
    }

    return true;
}
