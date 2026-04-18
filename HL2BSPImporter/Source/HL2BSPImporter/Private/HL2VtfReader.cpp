#include "HL2VtfReader.h"
#include "HL2BSPImporter.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryReader.h"

namespace HL2VTF
{
    namespace
    {
        // VTF "VTF\0" little-endian.
        constexpr uint32 kVTFMagic = 0x00465456;

        FORCEINLINE int32 BlockBytesFor(EImageFormat F)
        {
            switch (F)
            {
            case EImageFormat::DXT1:
            case EImageFormat::DXT1_ONEBITALPHA: return 8;
            case EImageFormat::DXT3:
            case EImageFormat::DXT5:             return 16;
            default: return 0;
            }
        }

        FORCEINLINE int32 BytesPerPixelFor(EImageFormat F)
        {
            switch (F)
            {
            case EImageFormat::RGBA8888:
            case EImageFormat::ABGR8888:
            case EImageFormat::ARGB8888:
            case EImageFormat::BGRA8888:
            case EImageFormat::BGRX8888:    return 4;
            case EImageFormat::RGB888:
            case EImageFormat::BGR888:      return 3;
            case EImageFormat::RGB565:
            case EImageFormat::BGR565:
            case EImageFormat::IA88:
            case EImageFormat::BGRA4444:
            case EImageFormat::BGRA5551:
            case EImageFormat::BGRX5551:
            case EImageFormat::UV88:        return 2;
            case EImageFormat::I8:
            case EImageFormat::A8:
            case EImageFormat::P8:          return 1;
            default: return 0;
            }
        }

        // Bytes occupied by ONE 2D image of (W,H) for the given format.
        int32 ImageSize(int32 W, int32 H, EImageFormat F)
        {
            if (W <= 0 || H <= 0) { return 0; }
            if (const int32 BB = BlockBytesFor(F))
            {
                const int32 BX = FMath::Max(1, (W + 3) / 4);
                const int32 BY = FMath::Max(1, (H + 3) / 4);
                return BX * BY * BB;
            }
            const int32 BPP = BytesPerPixelFor(F);
            return BPP > 0 ? (W * H * BPP) : 0;
        }

        FORCEINLINE int32 NumFacesFromFlags(const FInfo& I)
        {
            if (!(I.Flags & TEXTUREFLAGS_ENVMAP)) { return 1; }
            // Source 7.0–7.4 wrote 7 faces (extra spheremap) when major==7 minor<5.
            // We never read past face 0, but the byte count matters.
            const bool bHasSpheremap = (I.VersionMajor == 7 && I.VersionMinor < 5);
            return bHasSpheremap ? 7 : 6;
        }
    } // namespace

    bool ReadHeader(const TArray<uint8>& File, FInfo& Out, FString& OutError)
    {
        if (File.Num() < 64) { OutError = TEXT("VTF too small"); return false; }
        FMemoryReader R(File, true);
        uint32 Magic = 0; R << Magic;
        if (Magic != kVTFMagic) { OutError = TEXT("Not a VTF (bad magic)"); return false; }

        uint32 VerMajor = 0, VerMinor = 0, HeaderSize = 0;
        R << VerMajor; R << VerMinor; R << HeaderSize;

        if (VerMajor != 7 || VerMinor > 5)
        {
            OutError = FString::Printf(TEXT("Unsupported VTF version %u.%u (only 7.0–7.5)"), VerMajor, VerMinor);
            return false;
        }
        if (HeaderSize < 64 || (int64)HeaderSize > File.Num())
        {
            OutError = TEXT("VTF header size out of range");
            return false;
        }

        uint16 Width = 0, Height = 0;
        uint32 Flags = 0;
        uint16 Frames = 0, FirstFrame = 0;
        R << Width; R << Height; R << Flags; R << Frames; R << FirstFrame;
        R.Seek(R.Tell() + 4); // padding0
        float Refl[3] = {}; R.Serialize(Refl, sizeof(Refl));
        R.Seek(R.Tell() + 4); // padding1
        float BumpScale = 0; R << BumpScale;

        int32 ImageFormatI32 = 0; R << ImageFormatI32;
        uint8 NumMips = 0; R << NumMips;
        int32 LowResFormatI32 = 0; R << LowResFormatI32;
        uint8 LowResW = 0, LowResH = 0; R << LowResW; R << LowResH;

        // Depth (v7.2+).
        uint16 Depth = 1;
        if (VerMinor >= 2)
        {
            if (R.Tell() + 2 <= File.Num()) { R << Depth; }
            if (Depth == 0) { Depth = 1; }
        }

        Out.Width        = Width;
        Out.Height       = Height;
        Out.Depth        = Depth;
        Out.NumMips      = FMath::Max<int32>(1, NumMips);
        Out.Frames       = FMath::Max<int32>(1, Frames);
        Out.Format       = (EImageFormat)ImageFormatI32;
        Out.Flags        = Flags;
        Out.VersionMajor = (uint16)VerMajor;
        Out.VersionMinor = (uint16)VerMinor;
        Out.NumFaces     = NumFacesFromFlags(Out);

        if (Out.Width <= 0 || Out.Height <= 0)
        {
            OutError = TEXT("VTF has zero dimensions");
            return false;
        }
        if (Out.Format == EImageFormat::NONE)
        {
            OutError = TEXT("VTF image format is NONE");
            return false;
        }
        return true;
    }

    namespace
    {
        // Find absolute offset of the high-res image data block (start of the
        // smallest mip — VTF stores from smallest to largest).
        // Returns -1 on error.
        int64 LocateHighResData(const TArray<uint8>& File, const FInfo& I)
        {
            FMemoryReader R(File, true);
            R.Seek(4 + 4 + 4); // skip magic+version
            uint32 HeaderSize = 0; R << HeaderSize;

            if (I.VersionMinor < 3)
            {
                // Layout: header | low-res image | high-res image
                int32 LowResBytes = 0;
                // Low-res format / dims read from header at fixed offsets.
                if (File.Num() >= 64)
                {
                    // Re-read low-res descriptors at known offsets within the v7.0 header.
                    FMemoryReader Rh(File, true);
                    Rh.Seek(52); // ImageFormat at 48..51, NumMips at 52? Actually layout is:
                    // 0..3 magic, 4..11 version (8B), 12..15 headerSize, 16..19 W/H (W u16,H u16),
                    // 20..23 flags, 24..27 frames+firstFrame, 28..31 pad, 32..43 reflectivity,
                    // 44..47 pad, 48..51 bumpScale, 52..55 imageFormat, 56 numMips,
                    // 57..60 lowResImageFormat, 61 lowResW, 62 lowResH, 63 (depth_v72)
                    Rh.Seek(57);
                    int32 LowFmt = 0; Rh << LowFmt;
                    uint8 LW = 0, LH = 0; Rh << LW; Rh << LH;
                    if (LowFmt != (int32)EImageFormat::NONE && LW > 0 && LH > 0)
                    {
                        LowResBytes = ImageSize(LW, LH, (EImageFormat)LowFmt);
                    }
                }
                return (int64)HeaderSize + (int64)LowResBytes;
            }

            // v7.3+: walk the resource directory.
            R.Seek(R.Tell()); // we are after HeaderSize; skip rest of common header to NumResources.
            // After HeaderSize (offset 16), the v7.3 layout is identical to v7.2 up to byte 64,
            // then 3 bytes pad, 4 bytes NumResources, 8 bytes pad. NumResources lives at offset 75.
            FMemoryReader Rr(File, true);
            Rr.Seek(75);
            uint32 NumResources = 0;
            if (Rr.Tell() + 4 <= File.Num()) { Rr << NumResources; } else { NumResources = 0; }

            // Resource entries start at offset 80 (after 8 bytes pad).
            constexpr int64 kResourceTableOffset = 80;
            if (NumResources == 0)
            {
                return (int64)HeaderSize; // no resources: image data follows header
            }
            if (kResourceTableOffset + (int64)NumResources * 8 > File.Num())
            {
                return -1;
            }

            int64 HighResOff = -1;
            int64 LowResOff  = -1;
            for (uint32 i = 0; i < NumResources; ++i)
            {
                const uint8* Entry = File.GetData() + kResourceTableOffset + i * 8;
                const uint32 Tag = Entry[0] | ((uint32)Entry[1] << 8) | ((uint32)Entry[2] << 16);
                const uint8  Flg = Entry[3];
                uint32 Off = 0;
                FMemory::Memcpy(&Off, Entry + 4, 4);
                if (Flg & 0x2) { continue; } // inline data, not a file offset
                if (Tag == 0x30) { HighResOff = Off; }
                if (Tag == 0x01) { LowResOff  = Off; }
            }
            if (HighResOff >= 0) { return HighResOff; }
            // Fallback: header + lowres area.
            const int64 LowResEnd = LowResOff < 0 ? (int64)HeaderSize : (int64)HeaderSize; // unknown size; best-effort
            return LowResEnd;
        }

        // ---------------- Pixel-format converters (face 0, slice 0 of mip 0) ----------------
        // Output pixel order is BGRA8 (UTexture2D::Source TSF_BGRA8).

        void Convert_BGRA8888(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            FMemory::Memcpy(Dst, Src, W * H * 4);
        }
        void Convert_BGRX8888(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            {
                Dst[i*4+0] = Src[i*4+0]; Dst[i*4+1] = Src[i*4+1]; Dst[i*4+2] = Src[i*4+2]; Dst[i*4+3] = 0xFF;
            }
        }
        void Convert_RGBA8888(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            { Dst[i*4+0]=Src[i*4+2]; Dst[i*4+1]=Src[i*4+1]; Dst[i*4+2]=Src[i*4+0]; Dst[i*4+3]=Src[i*4+3]; }
        }
        void Convert_ABGR8888(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            { Dst[i*4+0]=Src[i*4+1]; Dst[i*4+1]=Src[i*4+2]; Dst[i*4+2]=Src[i*4+3]; Dst[i*4+3]=Src[i*4+0]; }
        }
        void Convert_ARGB8888(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            { Dst[i*4+0]=Src[i*4+3]; Dst[i*4+1]=Src[i*4+2]; Dst[i*4+2]=Src[i*4+1]; Dst[i*4+3]=Src[i*4+0]; }
        }
        void Convert_BGR888(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            { Dst[i*4+0]=Src[i*3+0]; Dst[i*4+1]=Src[i*3+1]; Dst[i*4+2]=Src[i*3+2]; Dst[i*4+3]=0xFF; }
        }
        void Convert_RGB888(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            { Dst[i*4+0]=Src[i*3+2]; Dst[i*4+1]=Src[i*3+1]; Dst[i*4+2]=Src[i*3+0]; Dst[i*4+3]=0xFF; }
        }
        void Convert_I8(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            { const uint8 v = Src[i]; Dst[i*4+0]=v; Dst[i*4+1]=v; Dst[i*4+2]=v; Dst[i*4+3]=0xFF; }
        }
        void Convert_A8(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            { Dst[i*4+0]=0xFF; Dst[i*4+1]=0xFF; Dst[i*4+2]=0xFF; Dst[i*4+3]=Src[i]; }
        }
        void Convert_IA88(const uint8* Src, int32 W, int32 H, uint8* Dst)
        {
            for (int32 i = 0; i < W * H; ++i)
            { const uint8 v = Src[i*2+0]; Dst[i*4+0]=v; Dst[i*4+1]=v; Dst[i*4+2]=v; Dst[i*4+3]=Src[i*2+1]; }
        }

        // ---------------- DXT decoders ----------------
        // Output: writes 4x4 BGRA pixels into Dst at offset (BX*4, BY*4).

        FORCEINLINE void Unpack565(uint16 c, uint8& R, uint8& G, uint8& B)
        {
            R = (uint8)(((c >> 11) & 0x1F) * 255 / 31);
            G = (uint8)(((c >>  5) & 0x3F) * 255 / 63);
            B = (uint8)(((c      ) & 0x1F) * 255 / 31);
        }

        void DecodeDXT1Block(const uint8* Block, int32 PixX, int32 PixY, int32 W, int32 H, uint8* Dst)
        {
            const uint16 c0 = (uint16)Block[0] | ((uint16)Block[1] << 8);
            const uint16 c1 = (uint16)Block[2] | ((uint16)Block[3] << 8);
            uint8 R0,G0,B0,R1,G1,B1;
            Unpack565(c0, R0, G0, B0);
            Unpack565(c1, R1, G1, B1);
            uint8 Pal[4][4]; // BGRA
            Pal[0][0]=B0; Pal[0][1]=G0; Pal[0][2]=R0; Pal[0][3]=0xFF;
            Pal[1][0]=B1; Pal[1][1]=G1; Pal[1][2]=R1; Pal[1][3]=0xFF;
            if (c0 > c1)
            {
                Pal[2][0]=(uint8)((2*B0+B1)/3); Pal[2][1]=(uint8)((2*G0+G1)/3); Pal[2][2]=(uint8)((2*R0+R1)/3); Pal[2][3]=0xFF;
                Pal[3][0]=(uint8)((B0+2*B1)/3); Pal[3][1]=(uint8)((G0+2*G1)/3); Pal[3][2]=(uint8)((R0+2*R1)/3); Pal[3][3]=0xFF;
            }
            else
            {
                Pal[2][0]=(uint8)((B0+B1)/2); Pal[2][1]=(uint8)((G0+G1)/2); Pal[2][2]=(uint8)((R0+R1)/2); Pal[2][3]=0xFF;
                Pal[3][0]=0; Pal[3][1]=0; Pal[3][2]=0; Pal[3][3]=0; // 1-bit alpha hole
            }
            const uint32 Indices = (uint32)Block[4] | ((uint32)Block[5] << 8) | ((uint32)Block[6] << 16) | ((uint32)Block[7] << 24);
            for (int32 py = 0; py < 4; ++py)
            {
                for (int32 px = 0; px < 4; ++px)
                {
                    const int32 X = PixX + px;
                    const int32 Y = PixY + py;
                    if (X >= W || Y >= H) { continue; }
                    const uint32 Idx = (Indices >> (2 * (py * 4 + px))) & 0x3;
                    uint8* Out = Dst + (Y * W + X) * 4;
                    Out[0] = Pal[Idx][0]; Out[1] = Pal[Idx][1]; Out[2] = Pal[Idx][2]; Out[3] = Pal[Idx][3];
                }
            }
        }

        void DecodeDXT5AlphaBlock(const uint8* AlphaBlock, uint8 OutAlpha[16])
        {
            const uint8 a0 = AlphaBlock[0];
            const uint8 a1 = AlphaBlock[1];
            uint8 Pal[8];
            Pal[0] = a0; Pal[1] = a1;
            if (a0 > a1)
            {
                for (int32 i = 1; i <= 6; ++i) { Pal[1+i] = (uint8)(((7-i)*a0 + i*a1) / 7); }
            }
            else
            {
                for (int32 i = 1; i <= 4; ++i) { Pal[1+i] = (uint8)(((5-i)*a0 + i*a1) / 5); }
                Pal[6] = 0; Pal[7] = 0xFF;
            }
            uint64 Bits = 0;
            for (int32 i = 0; i < 6; ++i) { Bits |= (uint64)AlphaBlock[2+i] << (8*i); }
            for (int32 i = 0; i < 16; ++i)
            {
                OutAlpha[i] = Pal[(Bits >> (3*i)) & 0x7];
            }
        }

        void DecodeDXT5Block(const uint8* Block, int32 PixX, int32 PixY, int32 W, int32 H, uint8* Dst)
        {
            uint8 Alpha[16];
            DecodeDXT5AlphaBlock(Block, Alpha);
            // Colour block = Block + 8, decoded like DXT1 but always 4-colour mode (no 1-bit alpha hole).
            const uint8* CB = Block + 8;
            const uint16 c0 = (uint16)CB[0] | ((uint16)CB[1] << 8);
            const uint16 c1 = (uint16)CB[2] | ((uint16)CB[3] << 8);
            uint8 R0,G0,B0,R1,G1,B1;
            Unpack565(c0, R0, G0, B0); Unpack565(c1, R1, G1, B1);
            uint8 Pal[4][3]; // BGR
            Pal[0][0]=B0; Pal[0][1]=G0; Pal[0][2]=R0;
            Pal[1][0]=B1; Pal[1][1]=G1; Pal[1][2]=R1;
            Pal[2][0]=(uint8)((2*B0+B1)/3); Pal[2][1]=(uint8)((2*G0+G1)/3); Pal[2][2]=(uint8)((2*R0+R1)/3);
            Pal[3][0]=(uint8)((B0+2*B1)/3); Pal[3][1]=(uint8)((G0+2*G1)/3); Pal[3][2]=(uint8)((R0+2*R1)/3);
            const uint32 Indices = (uint32)CB[4] | ((uint32)CB[5] << 8) | ((uint32)CB[6] << 16) | ((uint32)CB[7] << 24);
            for (int32 py = 0; py < 4; ++py)
            {
                for (int32 px = 0; px < 4; ++px)
                {
                    const int32 X = PixX + px;
                    const int32 Y = PixY + py;
                    if (X >= W || Y >= H) { continue; }
                    const uint32 Idx = (Indices >> (2 * (py * 4 + px))) & 0x3;
                    uint8* Out = Dst + (Y * W + X) * 4;
                    Out[0] = Pal[Idx][0]; Out[1] = Pal[Idx][1]; Out[2] = Pal[Idx][2];
                    Out[3] = Alpha[py * 4 + px];
                }
            }
        }

        void DecodeDXT3Block(const uint8* Block, int32 PixX, int32 PixY, int32 W, int32 H, uint8* Dst)
        {
            // Colour block follows the 8-byte explicit-alpha block.
            const uint8* CB = Block + 8;
            const uint16 c0 = (uint16)CB[0] | ((uint16)CB[1] << 8);
            const uint16 c1 = (uint16)CB[2] | ((uint16)CB[3] << 8);
            uint8 R0,G0,B0,R1,G1,B1;
            Unpack565(c0, R0, G0, B0); Unpack565(c1, R1, G1, B1);
            uint8 Pal[4][3];
            Pal[0][0]=B0; Pal[0][1]=G0; Pal[0][2]=R0;
            Pal[1][0]=B1; Pal[1][1]=G1; Pal[1][2]=R1;
            Pal[2][0]=(uint8)((2*B0+B1)/3); Pal[2][1]=(uint8)((2*G0+G1)/3); Pal[2][2]=(uint8)((2*R0+R1)/3);
            Pal[3][0]=(uint8)((B0+2*B1)/3); Pal[3][1]=(uint8)((G0+2*G1)/3); Pal[3][2]=(uint8)((R0+2*R1)/3);
            const uint32 Indices = (uint32)CB[4] | ((uint32)CB[5] << 8) | ((uint32)CB[6] << 16) | ((uint32)CB[7] << 24);
            for (int32 py = 0; py < 4; ++py)
            {
                const uint16 ARow = (uint16)Block[py*2] | ((uint16)Block[py*2+1] << 8);
                for (int32 px = 0; px < 4; ++px)
                {
                    const int32 X = PixX + px;
                    const int32 Y = PixY + py;
                    if (X >= W || Y >= H) { continue; }
                    const uint32 Idx = (Indices >> (2 * (py * 4 + px))) & 0x3;
                    const uint8 A4 = (uint8)((ARow >> (4 * px)) & 0xF);
                    uint8* Out = Dst + (Y * W + X) * 4;
                    Out[0] = Pal[Idx][0]; Out[1] = Pal[Idx][1]; Out[2] = Pal[Idx][2];
                    Out[3] = (uint8)((A4 << 4) | A4);
                }
            }
        }
    } // namespace

    bool DecodeBGRA(const TArray<uint8>& File, const FInfo& I, TArray<uint8>& Out, FString& OutError)
    {
        const int64 DataStart = LocateHighResData(File, I);
        if (DataStart < 0)
        {
            OutError = TEXT("Failed to locate high-res image data");
            return false;
        }

        // Walk mip chain from smallest -> largest, stopping at mip 0.
        int64 Cursor = DataStart;
        const int32 NumFaces  = FMath::Max(1, I.NumFaces);
        const int32 NumFrames = FMath::Max(1, I.Frames);
        const int32 NumSlices = FMath::Max(1, I.Depth);

        for (int32 Mip = I.NumMips - 1; Mip >= 0; --Mip)
        {
            const int32 MW = FMath::Max(1, I.Width  >> Mip);
            const int32 MH = FMath::Max(1, I.Height >> Mip);
            const int32 FaceBytes = ImageSize(MW, MH, I.Format);
            if (FaceBytes <= 0)
            {
                OutError = FString::Printf(TEXT("Unsupported VTF format %d"), (int32)I.Format);
                return false;
            }
            const int64 MipBytes = (int64)FaceBytes * NumFaces * NumFrames * NumSlices;
            if (Cursor + MipBytes > File.Num())
            {
                OutError = FString::Printf(TEXT("VTF truncated reading mip %d (need %lld bytes from offset %lld, file %d)"),
                    Mip, MipBytes, Cursor, File.Num());
                return false;
            }
            if (Mip == 0)
            {
                // We want frame 0, face 0, slice 0 — that's the first FaceBytes of this region.
                const uint8* Src = File.GetData() + Cursor;
                Out.SetNumUninitialized(I.Width * I.Height * 4);
                uint8* Dst = Out.GetData();

                switch (I.Format)
                {
                case EImageFormat::BGRA8888: Convert_BGRA8888(Src, I.Width, I.Height, Dst); break;
                case EImageFormat::BGRX8888: Convert_BGRX8888(Src, I.Width, I.Height, Dst); break;
                case EImageFormat::RGBA8888: Convert_RGBA8888(Src, I.Width, I.Height, Dst); break;
                case EImageFormat::ABGR8888: Convert_ABGR8888(Src, I.Width, I.Height, Dst); break;
                case EImageFormat::ARGB8888: Convert_ARGB8888(Src, I.Width, I.Height, Dst); break;
                case EImageFormat::BGR888:   Convert_BGR888  (Src, I.Width, I.Height, Dst); break;
                case EImageFormat::RGB888:   Convert_RGB888  (Src, I.Width, I.Height, Dst); break;
                case EImageFormat::I8:       Convert_I8      (Src, I.Width, I.Height, Dst); break;
                case EImageFormat::A8:       Convert_A8      (Src, I.Width, I.Height, Dst); break;
                case EImageFormat::IA88:     Convert_IA88    (Src, I.Width, I.Height, Dst); break;

                case EImageFormat::DXT1:
                case EImageFormat::DXT1_ONEBITALPHA:
                {
                    const int32 BX = FMath::Max(1, (I.Width  + 3) / 4);
                    const int32 BY = FMath::Max(1, (I.Height + 3) / 4);
                    for (int32 by = 0; by < BY; ++by)
                    {
                        for (int32 bx = 0; bx < BX; ++bx)
                        {
                            DecodeDXT1Block(Src + (by * BX + bx) * 8, bx * 4, by * 4, I.Width, I.Height, Dst);
                        }
                    }
                    break;
                }
                case EImageFormat::DXT3:
                {
                    const int32 BX = FMath::Max(1, (I.Width  + 3) / 4);
                    const int32 BY = FMath::Max(1, (I.Height + 3) / 4);
                    for (int32 by = 0; by < BY; ++by)
                    {
                        for (int32 bx = 0; bx < BX; ++bx)
                        {
                            DecodeDXT3Block(Src + (by * BX + bx) * 16, bx * 4, by * 4, I.Width, I.Height, Dst);
                        }
                    }
                    break;
                }
                case EImageFormat::DXT5:
                {
                    const int32 BX = FMath::Max(1, (I.Width  + 3) / 4);
                    const int32 BY = FMath::Max(1, (I.Height + 3) / 4);
                    for (int32 by = 0; by < BY; ++by)
                    {
                        for (int32 bx = 0; bx < BX; ++bx)
                        {
                            DecodeDXT5Block(Src + (by * BX + bx) * 16, bx * 4, by * 4, I.Width, I.Height, Dst);
                        }
                    }
                    break;
                }
                default:
                    OutError = FString::Printf(TEXT("Unsupported VTF format %d"), (int32)I.Format);
                    return false;
                }
                return true;
            }
            Cursor += MipBytes;
        }
        OutError = TEXT("VTF has no mip 0");
        return false;
    }

    bool LoadAndDecode(const FString& AbsPath, FInfo& Info, TArray<uint8>& OutBGRA, FString& OutError)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *AbsPath))
        {
            OutError = FString::Printf(TEXT("Failed to read VTF '%s'"), *AbsPath);
            return false;
        }
        if (!ReadHeader(Bytes, Info, OutError)) { return false; }
        return DecodeBGRA(Bytes, Info, OutBGRA, OutError);
    }
}
