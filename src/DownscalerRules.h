#pragma once

// The size Texture Downscaler (RoseEden30, Nexus SSE 187049) gives a texture, worked out from its ini as its page and ini
// describe it. Beside it, a full texture fetched after a loading screen arrives at that size rather than whole, and a texture
// whose small version is already that small needs no fetch at all.
//
// Its rules: a limit per type, the type read from the end of the file name; folder rules, a part of the path with an optional
// type in front, read top to bottom, the first match winning over the type; 0 keeps full size. It halves a texture until both
// sides fit, keeping at least one level, sides of at least 4, and multiples of 4 for block-compressed formats.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DownscalerRules {
    enum class Kind : std::uint8_t { kDiffuse, kNormal, kParallax, kMaterial, kGlow, kMask };
    inline constexpr std::size_t kKinds = 6;
    inline constexpr std::array<std::string_view, kKinds> kKindNames{"Diffuse", "Normal", "Parallax", "Material", "Glow", "Mask"};
    inline constexpr std::uint32_t kSmallest = 4;  // no limit below this, and no level kept smaller
    inline constexpr std::uint32_t kDefaultLimit = 1024;

    struct Folder {
        std::string part;  // folded
        std::uint32_t maxSize = 0;
        std::optional<Kind> kind;  // unset: every type
    };

    struct Rules {
        std::array<std::uint32_t, kKinds> maxSize{kDefaultLimit, kDefaultLimit, kDefaultLimit, kDefaultLimit, kDefaultLimit, kDefaultLimit};
        std::vector<Folder> folders;  // in file order: the first match wins
    };

    // Lower case, and forward slashes as backslashes: the form names and rules are compared in.
    constexpr char Fold(char a_c) { return a_c >= 'A' && a_c <= 'Z' ? static_cast<char>(a_c + ('a' - 'A')) : a_c == '/' ? '\\' : a_c; }

    inline std::string Fold(std::string_view a_text) {
        std::string folded(a_text.size(), '\0');
        for (std::size_t i = 0; i < a_text.size(); ++i) folded[i] = Fold(a_text[i]);
        return folded;
    }

    // 0 keeps full size; any other limit is at least kSmallest.
    constexpr std::uint32_t Clamp(long a_value) {
        return a_value <= 0 ? 0 : a_value < static_cast<long>(kSmallest) ? kSmallest : static_cast<std::uint32_t>(a_value);
    }

    inline std::optional<Kind> KindNamed(std::string_view a_name) {
        for (std::size_t i = 0; i < kKinds; ++i) {
            const auto name = kKindNames[i];
            if (name.size() != a_name.size()) continue;
            bool same = true;
            for (std::size_t c = 0; c < name.size() && same; ++c) same = Fold(name[c]) == Fold(a_name[c]);
            if (same) return static_cast<Kind>(i);
        }
        return std::nullopt;
    }

    // One [Folders] entry, in file order: "part=size" or "Type:part=size". False for a type it does not know, a line it skips.
    inline bool AddFolder(Rules& a_rules, std::string_view a_key, long a_value) {
        auto key = Fold(a_key);
        std::optional<Kind> kind;
        if (const auto colon = key.find(':'); colon != std::string::npos) {
            kind = KindNamed(std::string_view(key).substr(0, colon));
            if (!kind) return false;
            key.erase(0, colon + 1);
        }
        if (!key.empty()) a_rules.folders.push_back({std::move(key), Clamp(a_value), kind});
        return true;
    }

    // The type, from the end of the file name, given folded. Longer endings come first, so "_msn" is not read as "_n".
    inline Kind KindOf(std::string_view a_folded) {
        auto stem = a_folded;
        if (const auto slash = stem.find_last_of('\\'); slash != std::string_view::npos) stem.remove_prefix(slash + 1);
        if (const auto dot = stem.rfind('.'); dot != std::string_view::npos) stem = stem.substr(0, dot);
        static constexpr std::pair<std::string_view, Kind> kEndings[]{
            {"_rmaos", Kind::kMaterial}, {"_msn", Kind::kNormal}, {"_em", Kind::kMask}, {"_sk", Kind::kMask}, {"_n", Kind::kNormal},
            {"_p", Kind::kParallax}, {"_g", Kind::kGlow}, {"_m", Kind::kMask}, {"_s", Kind::kMask}, {"_b", Kind::kMask}};
        for (const auto& [ending, kind] : kEndings)
            if (stem.size() > ending.size() && stem.ends_with(ending)) return kind;
        return Kind::kDiffuse;
    }

    // The largest side a texture may keep; 0 for full size. The name given folded.
    inline std::uint32_t LimitFor(const Rules& a_rules, std::string_view a_folded, Kind a_kind) {
        for (const auto& folder : a_rules.folders) {
            if (folder.kind && *folder.kind != a_kind) continue;
            if (!folder.part.empty() && a_folded.find(folder.part) != std::string_view::npos) return folder.maxSize;
        }
        return a_rules.maxSize[static_cast<std::size_t>(a_kind)];
    }

    inline std::uint32_t LimitFor(const Rules& a_rules, std::string_view a_folded) { return LimitFor(a_rules, a_folded, KindOf(a_folded)); }

    // How many of the largest levels it leaves out of a texture of this size.
    constexpr std::uint32_t Skip(std::uint32_t a_limit, std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, bool a_blockCompressed) {
        if (a_limit == 0 || (a_width <= a_limit && a_height <= a_limit)) return 0;
        std::uint32_t skip = 0, width = a_width, height = a_height;
        while ((width > a_limit || height > a_limit) && skip + 1 < a_mips && (width >> 1) >= kSmallest && (height >> 1) >= kSmallest) {
            width >>= 1;
            height >>= 1;
            ++skip;
        }
        if (a_blockCompressed)
            while (skip > 0 && (((a_width >> skip) & 3u) || ((a_height >> skip) & 3u))) --skip;
        return skip;
    }

    // DXGI's block-compressed formats, BC1 to BC5 and BC6H to BC7.
    constexpr bool BlockCompressed(std::uint32_t a_dxgiFormat) { return (a_dxgiFormat >= 70 && a_dxgiFormat <= 84) || (a_dxgiFormat >= 94 && a_dxgiFormat <= 99); }
}
