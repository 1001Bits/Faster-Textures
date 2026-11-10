#pragma once

#include "TextureFormat.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

// Strict, byte-preserving DDS inputs only. Reject layouts that would require
// the engine's decompression/conversion path; matching dimensions alone is not
// evidence that bytes can safely be uploaded with the resident texture format.
namespace SkyrimSource {
    struct Info {
        std::uint32_t width{}, height{}, mips{}, format{}, offset{};
        std::uint64_t payloadBytes{};
    };

    constexpr std::uint32_t FourCC(char a, char b, char c, char d) {
        return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) |
               (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
    }

    // a_data must hold at least the file's headers (148 bytes covers every
    // layout); a_fileBytes is the whole file's size, for reading only its start.
    inline std::optional<Info> Parse(std::span<const std::byte> data, std::uint64_t fileBytes) {
        if (data.size() < 128) return std::nullopt;
        const auto u32 = [&](std::size_t at) {
            std::uint32_t n{};
            std::memcpy(&n, data.data() + at, sizeof(n));
            return n;
        };
        if (u32(0) != FourCC('D','D','S',' ') || u32(4) != 124 || u32(76) != 32 ||
            (u32(8) & 0x1007u) != 0x1007u || (u32(108) & 0x1000u) == 0 ||
            (u32(112) & (0xFE00u | 0x200000u)) != 0 || u32(24) > 1)
            return std::nullopt;
        Info info{u32(16), u32(12), std::max(u32(28), 1u), 0, 128, 0};
        if (!info.width || !info.height || info.width > 16384 || info.height > 16384) return std::nullopt;
        std::uint32_t maximumMips = 1;
        for (auto side = std::max(info.width, info.height); side > 1; side >>= 1) ++maximumMips;
        if (info.mips > maximumMips) return std::nullopt;

        const auto flags = u32(80);
        const auto code = u32(84);
        if ((flags & 4u) && code == FourCC('D','X','1','0')) {
            if (data.size() < 148 || u32(132) != 3 || u32(140) != 1 || (u32(136) & 4u))
                return std::nullopt;  // only a single non-cube Texture2D
            info.offset = 148;
            info.format = u32(128);
        } else if (flags & 4u) {
            switch (code) {
            case FourCC('D','X','T','1'): info.format = 71; break;
            case FourCC('D','X','T','3'): info.format = 74; break;
            case FourCC('D','X','T','5'): info.format = 77; break;
            case FourCC('A','T','I','1'):
            case FourCC('B','C','4','U'): info.format = 80; break;
            case FourCC('B','C','4','S'): info.format = 81; break;
            case FourCC('A','T','I','2'):
            case FourCC('B','C','5','U'): info.format = 83; break;
            case FourCC('B','C','5','S'): info.format = 84; break;
            case 36: info.format = 11; break;
            case 110: info.format = 13; break;
            case 111: info.format = 54; break;
            case 112: info.format = 34; break;
            case 113: info.format = 10; break;
            case 114: info.format = 41; break;
            case 115: info.format = 16; break;
            case 116: info.format = 2; break;
            default: return std::nullopt;
            }
        } else {
            const auto bits = u32(88), r = u32(92), g = u32(96), b = u32(100), a = u32(104);
            if (flags & 0x40u) {
                if (bits == 32 && r == 0xFF && g == 0xFF00 && b == 0xFF0000 && a == 0xFF000000) info.format = 28;
                else if (bits == 32 && r == 0xFF0000 && g == 0xFF00 && b == 0xFF && a == 0xFF000000) info.format = 87;
                else if (bits == 32 && r == 0xFF0000 && g == 0xFF00 && b == 0xFF && a == 0) info.format = 88;
                else if (bits == 32 && r == 0x3FF && g == 0xFFC00 && b == 0x3FF00000 && a == 0xC0000000) info.format = 24;
                else if (bits == 32 && r == 0xFFFF && g == 0xFFFF0000 && b == 0 && a == 0) info.format = 35;
                else if (bits == 16 && r == 0xF800 && g == 0x7E0 && b == 0x1F && a == 0) info.format = 85;
                else if (bits == 16 && r == 0x7C00 && g == 0x3E0 && b == 0x1F && a == 0x8000) info.format = 86;
            } else if (flags & 0x20000u) {
                if (bits == 8 && r == 0xFF && g == 0 && b == 0 && a == 0) info.format = 61;
                else if (bits == 16 && r == 0xFFFF && g == 0 && b == 0 && a == 0) info.format = 56;
                else if (bits == 16 && r == 0xFF && g == 0 && b == 0 && a == 0xFF00) info.format = 49;
            } else if ((flags & 2u) && bits == 8 && a == 0xFF) info.format = 65;
        }
        if (info.format > 255 || !TextureFormat::IsSupported(static_cast<std::uint8_t>(info.format)))
            return std::nullopt;
        const auto format = static_cast<std::uint8_t>(info.format);
        // A declared padded pitch would need per-file row decoding. The copy
        // path below accepts tightly packed DDS data only.
        const auto row = TextureFormat::BlockBytes(format) ?
            ((info.width + 3u) / 4u) * TextureFormat::BlockBytes(format) :
            (info.width * TextureFormat::BitsPerPixel(format) + 7u) / 8u;
        if ((u32(8) & 8u) && u32(20) != row) return std::nullopt;
        info.payloadBytes = TextureFormat::ChainBytes(format, info.width, info.height, 0, info.mips);
        if (fileBytes < info.offset || info.payloadBytes > fileBytes - info.offset) return std::nullopt;
        return info;
    }

    inline std::optional<Info> Parse(std::span<const std::byte> data) { return Parse(data, data.size()); }

    // These pairs differ in sampling transfer function, not stored bytes.
    constexpr std::uint32_t Linear(std::uint32_t format) {
        switch (format) {
        case 29: return 28;
        case 72: return 71;
        case 75: return 74;
        case 78: return 77;
        case 91: return 87;
        case 93: return 88;
        case 99: return 98;
        default: return format;
        }
    }
    constexpr bool TypelessMatches(std::uint32_t resource, std::uint32_t typed) {
        switch (resource) {
        case 1: return typed >= 2 && typed <= 4;
        case 5: return typed >= 6 && typed <= 8;
        case 9: return typed >= 10 && typed <= 14;
        case 15: return typed >= 16 && typed <= 18;
        case 23: return typed >= 24 && typed <= 25;
        case 27: return typed >= 28 && typed <= 32;
        case 33: return typed >= 34 && typed <= 38;
        case 39: return typed >= 41 && typed <= 43;
        case 48: return typed >= 49 && typed <= 52;
        case 53: return typed >= 54 && typed <= 59;
        case 60: return typed >= 61 && typed <= 64;
        case 70: return typed >= 71 && typed <= 72;
        case 73: return typed >= 74 && typed <= 75;
        case 76: return typed >= 77 && typed <= 78;
        case 79: return typed >= 80 && typed <= 81;
        case 82: return typed >= 83 && typed <= 84;
        case 90: return typed == 87 || typed == 91;
        case 92: return typed == 88 || typed == 93;
        case 94: return typed == 95 || typed == 96;
        case 97: return typed == 98 || typed == 99;
        default: return false;
        }
    }
    constexpr bool Compatible(std::uint32_t file, std::uint32_t resource, std::uint32_t view) {
        const bool viewMatches = Linear(file) == Linear(view) || TypelessMatches(file, view);
        const bool resourceMatches = Linear(resource) == Linear(view) || TypelessMatches(resource, view);
        return viewMatches && resourceMatches;
    }
}
