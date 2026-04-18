#pragma once
#include "CoreMinimal.h"

// Minimal VTF (Valve Texture File) reader. Targets v7.0–v7.5 — every shipping
// HL2 / Source 2007 / Source 2013 SDK map. Decodes ONLY the high-resolution
// image, mip 0, frame 0, face 0, slice 0. Cubemaps and volume textures are
// reported via the header but only the first face/slice is decoded.
//
// Output is always 8-bit BGRA in the layout UTexture2D::Source expects when
// initialised with TSF_BGRA8 (Width*Height*4 bytes, top-down rows).

namespace HL2VTF
{
    enum class EImageFormat : int32
    {
        NONE                  = -1,
        RGBA8888              = 0,
        ABGR8888              = 1,
        RGB888                = 2,
        BGR888                = 3,
        RGB565                = 4,
        I8                    = 5,
        IA88                  = 6,
        P8                    = 7,
        A8                    = 8,
        RGB888_BLUESCREEN     = 9,
        BGR888_BLUESCREEN     = 10,
        ARGB8888              = 11,
        BGRA8888              = 12,
        DXT1                  = 13,
        DXT3                  = 14,
        DXT5                  = 15,
        BGRX8888              = 16,
        BGR565                = 17,
        BGRX5551              = 18,
        BGRA4444              = 19,
        DXT1_ONEBITALPHA      = 20,
        BGRA5551              = 21,
        UV88                  = 22,
        UVWQ8888              = 23,
        RGBA16161616F         = 24,
        RGBA16161616          = 25,
        UVLX8888              = 26,
    };

    // Subset of the Source header flags we care about. Bit positions match
    // engine source.
    enum EFlags : uint32
    {
        TEXTUREFLAGS_POINTSAMPLE    = 1u <<  0,
        TEXTUREFLAGS_TRILINEAR      = 1u <<  1,
        TEXTUREFLAGS_CLAMPS         = 1u <<  2,
        TEXTUREFLAGS_CLAMPT         = 1u <<  3,
        TEXTUREFLAGS_NORMAL         = 1u <<  7,
        TEXTUREFLAGS_NOMIP          = 1u <<  8,
        TEXTUREFLAGS_NOLOD          = 1u <<  9,
        TEXTUREFLAGS_ENVMAP         = 1u << 14,
        TEXTUREFLAGS_SRGB           = 1u <<  6, // (Source 2013 reuses this slot — best-effort)
    };

    struct FInfo
    {
        int32        Width    = 0;
        int32        Height   = 0;
        int32        Depth    = 1;
        int32        NumFaces = 1;
        int32        NumMips  = 1;
        int32        Frames   = 1;
        EImageFormat Format   = EImageFormat::NONE;
        uint32       Flags    = 0;
        uint16       VersionMajor = 0;
        uint16       VersionMinor = 0;
    };

    // Inspect header only. Returns false on truncated / non-VTF input.
    HL2BSPIMPORTER_API bool ReadHeader(const TArray<uint8>& File, FInfo& OutInfo, FString& OutError);

    // Decode mip 0, frame 0, face 0, slice 0 into BGRA8 pixels (top-down).
    // OutPixels.Num() == Width*Height*4 on success. Supported formats:
    //   DXT1, DXT1_ONEBITALPHA, DXT3, DXT5, BGRA8888, BGRX8888, BGR888,
    //   RGB888, RGBA8888, ARGB8888, ABGR8888, A8, I8, IA88.
    // Other formats fail with a descriptive error.
    HL2BSPIMPORTER_API bool DecodeBGRA(
        const TArray<uint8>& File,
        const FInfo& Info,
        TArray<uint8>& OutPixels,
        FString& OutError);

    // Convenience: load file + header + decode in one shot.
    HL2BSPIMPORTER_API bool LoadAndDecode(
        const FString& AbsPath,
        FInfo& OutInfo,
        TArray<uint8>& OutBGRA,
        FString& OutError);
}
