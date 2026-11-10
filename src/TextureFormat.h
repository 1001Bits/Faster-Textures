#pragma once

#include <cstdint>

// Mip sizes exactly as the engine works them out (BSGraphics::GetMipSizes and
// BSGraphics::BitsPerPixel, which follow DirectXTex). The plugin moves the
// engine's data pointer past the mips it drops, so these have to agree with
// the engine byte for byte. Checked against every chunk of the vanilla BA2s:
// all non-cubemap textures match.
namespace TextureFormat
{
    // Bytes per 4x4 block for BCn, zero for anything else.
    constexpr std::uint32_t BlockBytes(std::uint8_t a_format)
    {
        switch (a_format) {
        case 70:  // BC1_TYPELESS
        case 71:  // BC1_UNORM
        case 72:  // BC1_UNORM_SRGB
        case 79:  // BC4_TYPELESS
        case 80:  // BC4_UNORM
        case 81:  // BC4_SNORM
            return 8;
        case 73:  // BC2
        case 74:
        case 75:
        case 76:  // BC3
        case 77:
        case 78:
        case 82:  // BC5
        case 83:
        case 84:
        case 94:  // BC6H
        case 95:
        case 96:
        case 97:  // BC7
        case 98:
        case 99:
            return 16;
        default:
            return 0;
        }
    }

    constexpr bool IsBlockCompressed(std::uint8_t a_format) { return BlockBytes(a_format) != 0; }

    // The engine's BitsPerPixel, minus R1_UNORM (66) and the two packed 4:2:2
    // formats (68, 69), which the engine lays out differently from this
    // formula. Zero means the format is left alone.
    constexpr std::uint32_t BitsPerPixel(std::uint8_t a_format)
    {
        if (a_format >= 1 && a_format <= 4) return 128;
        if (a_format >= 5 && a_format <= 8) return 96;
        if (a_format >= 9 && a_format <= 22) return 64;
        if ((a_format >= 23 && a_format <= 47) || a_format == 67 || (a_format >= 87 && a_format <= 93)) return 32;
        if ((a_format >= 48 && a_format <= 59) || a_format == 85 || a_format == 86) return 16;
        if (a_format >= 60 && a_format <= 65) return 8;
        return 0;
    }

    constexpr bool IsSupported(std::uint8_t a_format) { return IsBlockCompressed(a_format) || BitsPerPixel(a_format) != 0; }

    // One mip level, as BSGraphics::GetMipSizes counts it.
    constexpr std::uint64_t MipBytes(std::uint8_t a_format, std::uint32_t a_width, std::uint32_t a_height)
    {
        if (const auto block = BlockBytes(a_format); block != 0) {
            const std::uint64_t blocksX = a_width ? ((a_width + 3u) >> 2) : 0;
            const std::uint64_t blocksY = a_height ? ((a_height + 3u) >> 2) : 0;
            return blocksX * blocksY * block;
        }
        const std::uint64_t rowBytes = (static_cast<std::uint64_t>(BitsPerPixel(a_format)) * a_width + 7u) >> 3;
        return rowBytes * a_height;
    }

    // Levels [a_first, a_first + a_count) of a chain whose top level is
    // a_width x a_height. Each level halves both sides and never goes below 1.
    constexpr std::uint64_t ChainBytes(std::uint8_t a_format, std::uint32_t a_width, std::uint32_t a_height,
        std::uint32_t a_first, std::uint32_t a_count)
    {
        std::uint64_t total = 0;
        for (std::uint32_t level = 0; level < a_first + a_count; ++level) {
            if (level >= a_first) total += MipBytes(a_format, a_width, a_height);
            a_width = a_width > 1 ? a_width >> 1 : 1;
            a_height = a_height > 1 ? a_height >> 1 : 1;
        }
        return total;
    }

    static_assert(MipBytes(71, 1024, 1024) == 1024 * 1024 / 2);
    static_assert(MipBytes(77, 4, 4) == 16);
    static_assert(MipBytes(87, 3, 1) == 12);
    static_assert(ChainBytes(71, 8, 8, 0, 4) == 32 + 8 + 8 + 8);
}
