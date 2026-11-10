#pragma once

// The small mips of a texture on their own: which top mips a load can leave
// out, the DDS header that describes what is left, and how to reach those mips
// inside an LZ4 frame. Game-independent, so the tests run what the game runs.
//
// A frame has no block table: each block's size sits in front of it, so walking
// to the last blocks from the start touches every block on disk. The frame's end
// is known, though (an archive entry stores its size), and every block but the
// last decodes to exactly the block size. So the plugin reads only the end of the
// frame and looks for a position from which block sizes chain exactly to the end
// mark; how many blocks that chain holds says where they belong in the file. A
// false start needs a run of bytes that both chains to the end and decodes to
// exact block sizes, so a candidate is only accepted once it has decoded.

#include "SourceIdentity.h"

#include "FasterArchives/Lz4Blocks.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace MipTail {
    // Bytes of a file that always cover its DDS headers, extended header included.
    inline constexpr std::size_t kHeaderMax = 148;

    struct Plan {
        std::uint32_t skip{};         // top mips left out
        std::uint32_t width{}, height{}, mips{};  // the texture without them
        std::uint64_t tailOffset{};   // where the kept mips start in the file
        std::uint64_t tailBytes{};
        std::uint64_t skippedBytes{};
    };

    // Leaves out up to a_maxSkip top mips while the longer side stays at least
    // a_minEdge. A block-compressed top mip must have sides divisible by 4 or the
    // game cannot create the texture, and a side that stops being divisible never
    // becomes so again. Nothing when that leaves out under a_minSaved bytes:
    // loading twice would then cost more than it saves.
    inline std::optional<Plan> PlanFor(const SkyrimSource::Info& file, std::uint32_t a_maxSkip, std::uint32_t a_minEdge,
                                       std::uint64_t a_minSaved) {
        const auto format = static_cast<std::uint8_t>(file.format);
        const bool blocks = TextureFormat::IsBlockCompressed(format);
        std::uint32_t skip = 0;
        for (std::uint32_t s = 1; s <= a_maxSkip && s < file.mips; ++s) {
            const auto w = std::max(file.width >> s, 1u);
            const auto h = std::max(file.height >> s, 1u);
            if (std::max(w, h) < a_minEdge || (blocks && (w % 4 != 0 || h % 4 != 0))) break;
            skip = s;
        }
        if (skip == 0) return std::nullopt;
        Plan plan;
        plan.skip = skip;
        plan.width = std::max(file.width >> skip, 1u);
        plan.height = std::max(file.height >> skip, 1u);
        plan.mips = file.mips - skip;
        plan.skippedBytes = TextureFormat::ChainBytes(format, file.width, file.height, 0, skip);
        plan.tailOffset = file.offset + plan.skippedBytes;
        plan.tailBytes = TextureFormat::ChainBytes(format, file.width, file.height, skip, plan.mips);
        if (plan.skippedBytes < a_minSaved) return std::nullopt;
        return plan;
    }

    // Turns the file's headers into those of a DDS that starts at the plan's
    // first kept mip. a_header holds at least the file's a_file.offset bytes.
    inline void RewriteHeader(std::span<std::byte> a_header, const SkyrimSource::Info& a_file, const Plan& a_plan) {
        const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(a_header.data() + at, &value, 4); };
        std::uint32_t flags{};
        std::memcpy(&flags, a_header.data() + 8, 4);
        const auto format = static_cast<std::uint8_t>(a_file.format);
        put(12, a_plan.height);
        put(16, a_plan.width);
        put(28, a_plan.mips);
        if (flags & 0x80000u) {  // DDSD_LINEARSIZE: the top mip's size
            put(20, static_cast<std::uint32_t>(TextureFormat::MipBytes(format, a_plan.width, a_plan.height)));
        } else if (flags & 0x8u) {  // DDSD_PITCH: bytes per row; Parse accepts packed rows only
            const auto block = TextureFormat::BlockBytes(format);
            put(20, block ? ((a_plan.width + 3u) / 4u) * block : (a_plan.width * TextureFormat::BitsPerPixel(format) + 7u) / 8u);
        }
    }

    // Where each mip of a whole file lies, for creating the texture from it.
    struct Mip {
        std::uint64_t offset{};
        std::uint32_t rowPitch{}, slicePitch{};
    };

    inline std::vector<Mip> MipsOf(const SkyrimSource::Info& a_file) {
        const auto format = static_cast<std::uint8_t>(a_file.format);
        const auto block = TextureFormat::BlockBytes(format);
        std::vector<Mip> mips(a_file.mips);
        std::uint64_t offset = a_file.offset;
        auto width = a_file.width, height = a_file.height;
        for (auto& mip : mips) {
            const auto bytes = TextureFormat::MipBytes(format, width, height);
            mip.offset = offset;
            mip.slicePitch = static_cast<std::uint32_t>(bytes);
            mip.rowPitch = block ? ((width + 3u) >> 2) * block : (TextureFormat::BitsPerPixel(format) * width + 7u) >> 3;
            offset += bytes;
            width = std::max(width >> 1, 1u);
            height = std::max(height >> 1, 1u);
        }
        return mips;
    }

    // ── Reaching the kept mips inside a frame ───────────────────────────────

    // Whether decoding can start past the beginning: independent blocks, and at
    // least one whole block before a_from. Bethesda's frames mostly fail this:
    // linked blocks, or one block as large as the entry.
    [[nodiscard]] constexpr bool CanSkip(const Lz4Blocks::Header& a_h, std::uint64_t a_from) noexcept {
        return a_h.independent && !a_h.dictionary && a_h.blockMax != 0 && a_from >= a_h.blockMax;
    }

    [[nodiscard]] constexpr std::uint64_t BlocksFrom(const Lz4Blocks::Header& a_h, std::uint64_t a_total, std::uint64_t a_from) noexcept {
        return (a_total + a_h.blockMax - 1) / a_h.blockMax - a_from / a_h.blockMax;
    }

    [[nodiscard]] constexpr std::uint64_t TrailerBytes(const Lz4Blocks::Header& a_h) noexcept {
        return 4 + (a_h.contentChecksum ? 4 : 0);
    }

    // The most bytes at a frame's end those blocks can take: certain to hold them.
    [[nodiscard]] constexpr std::uint64_t WindowCertain(const Lz4Blocks::Header& a_h, std::uint64_t a_total, std::uint64_t a_from) noexcept {
        return BlocksFrom(a_h, a_total, a_from) * (std::uint64_t{a_h.blockMax} + 4 + (a_h.blockChecksum ? 4 : 0)) + TrailerBytes(a_h);
    }

    // What they likely take, from how well the whole frame compressed. Reading
    // this first avoids reading far more than needed for compressible data.
    [[nodiscard]] constexpr std::uint64_t WindowLikely(const Lz4Blocks::Header& a_h, std::uint64_t a_frameBytes, std::uint64_t a_total,
                                                       std::uint64_t a_from) noexcept {
        const auto decoded = BlocksFrom(a_h, a_total, a_from) * a_h.blockMax;
        const auto scaled = a_total ? static_cast<std::uint64_t>(static_cast<double>(decoded) * a_frameBytes / static_cast<double>(a_total)) : decoded;
        return std::min(scaled + scaled / 4 + std::uint64_t{a_h.blockMax} + 16, WindowCertain(a_h, a_total, a_from));
    }

    namespace detail {
        // Block positions from a_at up to a_end, where the end mark sits: false
        // unless block sizes chain exactly onto a_end.
        inline bool WalkToEnd(const std::uint8_t* a_window, std::size_t a_at, std::size_t a_end, const Lz4Blocks::Header& a_h,
                              std::vector<Lz4Blocks::Block>& a_out) {
            a_out.clear();
            const std::size_t checksum = a_h.blockChecksum ? 4 : 0;
            while (a_at != a_end) {
                if (a_end - a_at < 5) return false;
                std::uint32_t word{};
                std::memcpy(&word, a_window + a_at, 4);
                const std::uint32_t size = word & 0x7FFFFFFFu;
                if (size == 0 || size > a_h.blockMax) return false;
                a_at += 4;
                if (a_end - a_at < std::size_t{size} + checksum) return false;
                a_out.push_back({static_cast<std::uint32_t>(a_at), size, 0, 0, (word & 0x80000000u) == 0});
                a_at += size + checksum;
            }
            return true;
        }

        inline bool DecodeInto(const std::uint8_t* a_window, Lz4Blocks::Block a_block, std::uint8_t* a_dst) {
            a_block.dstOffset = 0;
            return Lz4Blocks::DecodeBlock(a_window, a_block, a_dst);
        }
    }

    // Decodes bytes [a_from, a_to) of the a_total decoded bytes of a frame into
    // a_dst. a_window holds the frame's last bytes and ends exactly at its end,
    // a_h is the frame's header. False when the window does not hold them or the
    // frame ends otherwise: the caller then reads more, or leaves it to the game.
    inline bool DecodeTail(const std::uint8_t* a_window, std::size_t a_size, const Lz4Blocks::Header& a_h, std::uint64_t a_total,
                           std::uint64_t a_from, std::uint64_t a_to, std::uint8_t* a_dst) {
        const auto trailer = TrailerBytes(a_h);
        if (!a_h.blockMax || a_from >= a_to || a_to > a_total || a_size < trailer + 5) return false;
        const std::size_t end = a_size - static_cast<std::size_t>(trailer);
        std::uint32_t mark{};
        std::memcpy(&mark, a_window + end, 4);
        if (mark != 0) return false;

        const std::uint64_t count = (a_total + a_h.blockMax - 1) / a_h.blockMax;
        const std::uint64_t need = count - a_from / a_h.blockMax;
        const std::uint64_t firstWanted = a_from / a_h.blockMax;
        const auto lastBlock = static_cast<std::uint32_t>(a_total - (count - 1) * a_h.blockMax);

        std::vector<Lz4Blocks::Block> chain;
        std::vector<std::uint8_t> temp;
        for (std::size_t start = 0; start + need * 5 <= end; ++start) {
            std::uint32_t word{};
            std::memcpy(&word, a_window + start, 4);
            const std::uint32_t size = word & 0x7FFFFFFFu;
            if (size == 0 || size > a_h.blockMax) continue;
            if (!detail::WalkToEnd(a_window, start, end, a_h, chain)) continue;
            if (chain.size() < need || chain.size() > count) continue;

            bool ok = true;
            const std::size_t skipped = chain.size() - static_cast<std::size_t>(need);
            for (std::size_t i = 0; ok && i < need; ++i) {
                auto block = chain[skipped + i];
                const std::uint64_t index = firstWanted + i;
                const std::uint64_t blockStart = index * a_h.blockMax;
                block.dstSize = index + 1 == count ? lastBlock : a_h.blockMax;
                if (!block.compressed && block.srcSize != block.dstSize) {
                    ok = false;
                    break;
                }
                const std::uint64_t blockEnd = blockStart + block.dstSize;
                if (blockStart >= a_to) break;  // past what was asked for
                const auto copyFrom = std::max(blockStart, a_from);
                const auto copyTo = std::min(blockEnd, a_to);
                if (copyFrom == blockStart && copyTo == blockEnd) {
                    ok = detail::DecodeInto(a_window, block, a_dst + (blockStart - a_from));
                } else {
                    temp.resize(block.dstSize);
                    ok = detail::DecodeInto(a_window, block, temp.data());
                    if (ok) std::memcpy(a_dst + (copyFrom - a_from), temp.data() + (copyFrom - blockStart), static_cast<std::size_t>(copyTo - copyFrom));
                }
            }
            if (ok) return true;
        }
        return false;
    }

    // The first a_want decoded bytes of a frame, from its start: what the DDS
    // headers need, without decoding the rest of the block. Any frame layout: the
    // first block never depends on anything before it. a_frame holds the frame's
    // start, a_size bytes of it, enough to produce a_want bytes.
    inline bool DecodeStart(const std::uint8_t* a_frame, std::size_t a_size, const Lz4Blocks::Header& a_h, std::uint8_t* a_dst, std::size_t a_want) {
        if (a_size < a_h.size + 4) return false;
        std::uint32_t word{};
        std::memcpy(&word, a_frame + a_h.size, 4);
        const std::uint32_t blockBytes = word & 0x7FFFFFFFu;
        if (blockBytes == 0 || blockBytes > a_h.blockMax) return false;
        const auto* src = a_frame + a_h.size + 4;
        const std::size_t available = std::min<std::size_t>(a_size - a_h.size - 4, blockBytes);
        if (word & 0x80000000u) {
            if (available < a_want) return false;
            std::memcpy(a_dst, src, a_want);
            return true;
        }
        const int decoded = LZ4_decompress_safe_partial(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(a_dst),
                                                        static_cast<int>(available), static_cast<int>(a_want), static_cast<int>(a_want));
        return decoded == static_cast<int>(a_want);
    }
}
