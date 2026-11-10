#include "ArchiveTail.h"

#include <lz4frame.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {
    void Require(bool value, const std::string& name) {
        if (!value) {
            std::cerr << "FAILED: " << name << '\n';
            std::exit(1);
        }
    }

    using Bytes = std::vector<std::uint8_t>;

    // Compressible like block-compressed texture data: runs mixed with noise.
    Bytes Content(std::size_t a_size, std::uint32_t a_seed, int a_noisePercent) {
        std::mt19937 random(a_seed);
        Bytes out(a_size);
        for (std::size_t i = 0; i < a_size;) {
            const auto run = std::min<std::size_t>(a_size - i, 1 + random() % 64);
            if (static_cast<int>(random() % 100) < a_noisePercent) {
                for (std::size_t j = 0; j < run; ++j) out[i + j] = static_cast<std::uint8_t>(random());
            } else {
                std::memset(out.data() + i, static_cast<int>(random() % 256), run);
            }
            i += run;
        }
        return out;
    }

    struct FrameOptions {
        LZ4F_blockSizeID_t blockSize = LZ4F_max64KB;
        LZ4F_blockMode_t mode = LZ4F_blockIndependent;
        bool blockChecksum = false, contentChecksum = false, contentSize = false;
    };

    Bytes Frame(const Bytes& a_content, const FrameOptions& a_options) {
        LZ4F_preferences_t preferences{};
        preferences.frameInfo.blockSizeID = a_options.blockSize;
        preferences.frameInfo.blockMode = a_options.mode;
        preferences.frameInfo.blockChecksumFlag = a_options.blockChecksum ? LZ4F_blockChecksumEnabled : LZ4F_noBlockChecksum;
        preferences.frameInfo.contentChecksumFlag = a_options.contentChecksum ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
        preferences.frameInfo.contentSize = a_options.contentSize ? a_content.size() : 0;
        Bytes out(LZ4F_compressFrameBound(a_content.size(), &preferences));
        const auto written = LZ4F_compressFrame(out.data(), out.size(), a_content.data(), a_content.size(), &preferences);
        Require(!LZ4F_isError(written), "frame compressed");
        out.resize(written);
        return out;
    }

    // Decodes [a_from, a_to) through the same steps the archive reader takes.
    bool Tail(const Bytes& a_frame, const Bytes& a_content, std::uint64_t a_from, std::uint64_t a_to, bool a_certain, Bytes& a_out) {
        Lz4Blocks::Header header;
        Require(Lz4Blocks::ParseHeader(a_frame.data(), a_frame.size(), header), "frame header parsed");
        const auto wanted = a_certain ? MipTail::WindowCertain(header, a_content.size(), a_from)
                                      : MipTail::WindowLikely(header, a_frame.size(), a_content.size(), a_from);
        const auto bytes = std::min<std::uint64_t>(wanted, a_frame.size() - header.size);
        a_out.assign(static_cast<std::size_t>(a_to - a_from), 0);
        return MipTail::DecodeTail(a_frame.data() + a_frame.size() - bytes, static_cast<std::size_t>(bytes), header, a_content.size(), a_from, a_to, a_out.data());
    }

    void Matches(const Bytes& a_frame, const Bytes& a_content, std::uint64_t a_from, std::uint64_t a_to, const std::string& a_name) {
        Bytes out;
        Require(Tail(a_frame, a_content, a_from, a_to, true, out), a_name + ": decoded from the certain window");
        Require(std::memcmp(out.data(), a_content.data() + a_from, out.size()) == 0, a_name + ": bytes match");
        if (Tail(a_frame, a_content, a_from, a_to, false, out))
            Require(std::memcmp(out.data(), a_content.data() + a_from, out.size()) == 0, a_name + ": bytes match from the likely window");
    }

    void TailAcrossFrames() {
        const std::vector<std::pair<LZ4F_blockSizeID_t, std::uint32_t>> sizes{{LZ4F_max64KB, 64u << 10}, {LZ4F_max256KB, 256u << 10}, {LZ4F_max1MB, 1u << 20}, {LZ4F_max4MB, 4u << 20}};
        for (const auto& [blockSize, blockBytes] : sizes) {
            for (const int noise : {0, 40, 100}) {
                for (const int flags : {0, 1, 2, 3}) {
                    FrameOptions options;
                    options.blockSize = blockSize;
                    options.blockChecksum = flags & 1;
                    options.contentChecksum = flags & 2;
                    options.contentSize = flags == 3;
                    const std::size_t total = std::size_t{blockBytes} * 5 + 12345;
                    const auto content = Content(total, blockBytes + noise + flags, noise);
                    const auto frame = Frame(content, options);
                    const auto name = "block " + std::to_string(blockBytes) + " noise " + std::to_string(noise) + " flags " + std::to_string(flags);
                    Matches(frame, content, total - 1000, total, name + " inside the last block");
                    Matches(frame, content, total - std::size_t{blockBytes} - 5, total, name + " across the last two blocks");
                    Matches(frame, content, blockBytes * 2 + 17, total, name + " from an inner offset");
                    Matches(frame, content, blockBytes, total, name + " from a block boundary");
                    Matches(frame, content, blockBytes * 2 + 17, blockBytes * 3 + 99, name + " a range inside the frame");
                }
            }
        }
    }

    void TotalOnBlockBoundary() {
        const std::size_t total = (64u << 10) * 7;
        const auto content = Content(total, 7, 30);
        const auto frame = Frame(content, {});
        Matches(frame, content, (64u << 10) * 6, total, "whole last block");
        Matches(frame, content, (64u << 10) * 6 - 1, total, "one byte of the block before it");
    }

    // Data full of words that read as valid block sizes must not mislead the
    // search: a false start has to chain to the end and also decode.
    void FalseBlockHeaders() {
        Bytes content((64u << 10) * 8);
        for (std::size_t i = 0; i + 4 <= content.size(); i += 4) {
            const std::uint32_t word = 0x00001000u + static_cast<std::uint32_t>(i % 0x8000);
            std::memcpy(content.data() + i, &word, 4);
        }
        const auto frame = Frame(content, {});
        Matches(frame, content, (64u << 10) * 5 + 3, content.size(), "words like block sizes");

        std::mt19937 random(99);
        Bytes noisy((64u << 10) * 8);  // incompressible: every block is stored raw, with bytes copied verbatim
        for (auto& byte : noisy) byte = static_cast<std::uint8_t>(random());
        for (std::size_t i = 0; i + 8 <= noisy.size(); i += 4097) {
            const std::uint32_t word = 0x80010000u;  // a stored block's exact header, as content
            std::memcpy(noisy.data() + i, &word, 4);
        }
        const auto stored = Frame(noisy, {});
        Matches(stored, noisy, (64u << 10) * 5 + 3, noisy.size(), "stored block headers as content");
    }

    void TooShortWindowsFail() {
        const std::size_t total = (64u << 10) * 8;
        const auto content = Content(total, 11, 100);
        const auto frame = Frame(content, {});
        Lz4Blocks::Header header;
        Require(Lz4Blocks::ParseHeader(frame.data(), frame.size(), header), "header parsed");
        Bytes out(total - (64u << 10) * 3);
        Require(!MipTail::DecodeTail(frame.data() + frame.size() - 1000, 1000, header, total, (64u << 10) * 3, total, out.data()), "too little of the frame");
        Bytes cut(frame.begin(), frame.end() - 1);
        Require(!MipTail::DecodeTail(cut.data(), cut.size(), header, total, (64u << 10) * 3, total, out.data()), "a frame that does not end in its end mark");
    }

    void LinkedFramesAreNotSkipped() {
        FrameOptions linked;
        linked.mode = LZ4F_blockLinked;
        const auto content = Content((64u << 10) * 6, 21, 30);
        const auto frame = Frame(content, linked);
        Lz4Blocks::Header header;
        Require(Lz4Blocks::ParseHeader(frame.data(), frame.size(), header), "linked header parsed");
        Require(!MipTail::CanSkip(header, 1u << 20), "linked blocks cannot be skipped");

        Lz4Blocks::Header independent = header;
        independent.independent = true;
        Require(MipTail::CanSkip(independent, 64u << 10), "a whole block before the mips");
        Require(!MipTail::CanSkip(independent, (64u << 10) - 1), "the mips start in the first block");
    }

    void StartOfFrame() {
        const auto content = Content((64u << 10) * 3, 31, 30);
        for (const auto mode : {LZ4F_blockIndependent, LZ4F_blockLinked}) {
            FrameOptions options;
            options.mode = mode;
            const auto frame = Frame(content, options);
            Lz4Blocks::Header header;
            Require(Lz4Blocks::ParseHeader(frame.data(), frame.size(), header), "header parsed");
            std::array<std::uint8_t, MipTail::kHeaderMax> out{};
            const auto bytes = std::min<std::size_t>(frame.size(), 4096);
            Require(MipTail::DecodeStart(frame.data(), bytes, header, out.data(), out.size()), "start decoded from 4 KiB");
            Require(std::memcmp(out.data(), content.data(), out.size()) == 0, "start matches");
        }
        const auto noise = Content((64u << 10) * 3, 32, 100);
        const auto stored = Frame(noise, {});
        Lz4Blocks::Header header;
        Require(Lz4Blocks::ParseHeader(stored.data(), stored.size(), header), "stored header parsed");
        std::array<std::uint8_t, MipTail::kHeaderMax> out{};
        Require(MipTail::DecodeStart(stored.data(), 4096, header, out.data(), out.size()), "start of a stored block");
        Require(std::memcmp(out.data(), noise.data(), out.size()) == 0, "stored start matches");
    }

    // ── Plans and headers ───────────────────────────────────────────────────

    Bytes Dds(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, std::uint32_t a_format) {
        Bytes out(148);
        const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(out.data() + at, &value, 4); };
        put(0, SkyrimSource::FourCC('D', 'D', 'S', ' '));
        put(4, 124);
        put(8, 0x1007u | 0x80000u);
        put(12, a_height);
        put(16, a_width);
        put(20, static_cast<std::uint32_t>(TextureFormat::MipBytes(static_cast<std::uint8_t>(a_format), a_width, a_height)));
        put(28, a_mips);
        put(76, 32);
        put(80, 4);
        put(84, SkyrimSource::FourCC('D', 'X', '1', '0'));
        put(108, 0x1000);
        put(128, a_format);
        put(132, 3);
        put(140, 1);
        return out;
    }

    std::optional<SkyrimSource::Info> Parsed(const Bytes& a_dds, std::uint64_t a_fileBytes) {
        return SkyrimSource::Parse(std::span(reinterpret_cast<const std::byte*>(a_dds.data()), a_dds.size()), a_fileBytes);
    }

    void Plans() {
        constexpr std::uint32_t bc7 = 98, bc1 = 71, rgba = 28;
        const auto chain = [](std::uint32_t format, std::uint32_t w, std::uint32_t h, std::uint32_t mips) {
            return 148 + TextureFormat::ChainBytes(static_cast<std::uint8_t>(format), w, h, 0, mips);
        };
        auto info = Parsed(Dds(4096, 4096, 13, bc7), chain(bc7, 4096, 4096, 13));
        Require(info.has_value(), "4K header parsed");
        auto plan = MipTail::PlanFor(*info, 2, 512, 1);
        Require(plan && plan->skip == 2 && plan->width == 1024 && plan->height == 1024 && plan->mips == 11, "two mips off a 4K texture");
        Require(plan->tailOffset == 148 + (16ull << 20) + (4ull << 20), "the kept mips start after the two left out");
        Require(plan->tailOffset + plan->tailBytes == chain(bc7, 4096, 4096, 13), "the kept mips reach the end of the file");

        auto small = MipTail::PlanFor(*info, 8, 1024, 1);
        Require(small && small->skip == 2, "the longer side stays at the minimum");

        info = Parsed(Dds(1024, 512, 11, bc1), chain(bc1, 1024, 512, 11));
        plan = MipTail::PlanFor(*info, 2, 512, 1);
        Require(plan && plan->skip == 1 && plan->width == 512 && plan->height == 256, "by the longer side");

        info = Parsed(Dds(512, 512, 10, bc1), chain(bc1, 512, 512, 10));
        Require(!MipTail::PlanFor(*info, 2, 512, 1), "nothing off a texture at the minimum");

        info = Parsed(Dds(2048, 1022, 11, bc1), chain(bc1, 2048, 1022, 11));
        Require(info.has_value(), "uneven texture parsed");
        Require(!MipTail::PlanFor(*info, 2, 64, 1), "an uneven side stops a compressed format");

        info = Parsed(Dds(2048, 1022, 11, rgba), chain(rgba, 2048, 1022, 11));
        plan = MipTail::PlanFor(*info, 2, 64, 1);
        Require(plan && plan->skip == 2, "an uneven side does not stop an uncompressed format");

        info = Parsed(Dds(2048, 2048, 1, bc1), chain(bc1, 2048, 2048, 1));
        Require(!MipTail::PlanFor(*info, 2, 64, 1), "no mips to drop");

        info = Parsed(Dds(1024, 1024, 11, bc1), chain(bc1, 1024, 1024, 11));
        Require(!MipTail::PlanFor(*info, 1, 64, 1u << 30), "too little saved");

        Require(!Parsed(Dds(4096, 4096, 13, bc7), 1000), "a file shorter than its mips");
    }

    void Headers() {
        constexpr std::uint32_t bc7 = 98;
        auto dds = Dds(4096, 2048, 13, bc7);
        const auto full = 148 + TextureFormat::ChainBytes(static_cast<std::uint8_t>(bc7), 4096, 2048, 0, 13);
        const auto info = Parsed(dds, full);
        Require(info.has_value(), "header parsed");
        const auto plan = MipTail::PlanFor(*info, 2, 512, 1);
        Require(plan.has_value(), "planned");
        MipTail::RewriteHeader(std::span(reinterpret_cast<std::byte*>(dds.data()), dds.size()), *info, *plan);
        const auto rewritten = Parsed(dds, 148 + plan->tailBytes);
        Require(rewritten.has_value(), "the rewritten header describes the kept bytes");
        Require(rewritten->width == 1024 && rewritten->height == 512 && rewritten->mips == 11 && rewritten->format == bc7, "rewritten fields");
        Require(rewritten->payloadBytes == plan->tailBytes, "rewritten payload equals the kept mips");

        const auto mips = MipTail::MipsOf(*info);
        Require(mips.size() == 13 && mips[0].offset == 148 && mips[0].slicePitch == (4096u * 2048u), "first mip");
        Require(mips[1].offset == 148 + (4096ull * 2048ull) && mips[1].rowPitch == 1024u * 2u * 4u, "second mip");
        Require(mips[2].offset == plan->tailOffset, "the plan starts where the third mip starts");
    }

    // ── From an archive file ────────────────────────────────────────────────

    void Archive() {
        constexpr std::uint32_t bc1 = 71;
        const auto format = static_cast<std::uint8_t>(bc1);
        const auto mipData = TextureFormat::ChainBytes(format, 2048, 2048, 0, 12);
        auto content = Content(static_cast<std::size_t>(148 + mipData), 41, 40);
        const auto dds = Dds(2048, 2048, 12, bc1);
        std::memcpy(content.data(), dds.data(), dds.size());

        const auto repacked = Frame(content, {});
        FrameOptions linkedOptions;
        linkedOptions.mode = LZ4F_blockLinked;
        linkedOptions.blockSize = LZ4F_max4MB;
        const auto linked = Frame(content, linkedOptions);

        const auto path = std::filesystem::temp_directory_path() / ("miptail-test-" + std::to_string(GetCurrentProcessId()) + ".bin");
        const auto original = static_cast<std::uint32_t>(content.size());
        const std::string name = "textures\\example.dds";
        Bytes file(4096, 0xAB);
        const auto append = [&](bool a_name, const Bytes& a_frame) {
            const auto at = static_cast<std::uint32_t>(file.size());
            if (a_name) {
                file.push_back(static_cast<std::uint8_t>(name.size()));
                file.insert(file.end(), name.begin(), name.end());
            }
            file.resize(file.size() + 4);
            std::memcpy(file.data() + file.size() - 4, &original, 4);
            file.insert(file.end(), a_frame.begin(), a_frame.end());
            return ArchiveIndex::Entry{at, static_cast<std::uint32_t>(file.size() - at), 0};
        };
        const auto plain = append(false, repacked);
        const auto chained = append(false, linked);
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        }

        const auto open = [&] { return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr); };
        {
            ArchiveIndex::Index index;
            index.archives.push_back({open(), "test", false, true});
            index.entries = {plain, chained};
            Require(index.archives[0].handle != INVALID_HANDLE_VALUE, "archive opened");

            const auto located = ArchiveTail::Locate(index, plain.offset, plain.size);
            Require(located && located->offset == plain.offset && located->bytes == plain.size, "entry found by its start and stored size");
            Require(!ArchiveTail::Locate(index, plain.offset, plain.size + 1), "a different stored size is not this entry");
            Require(!ArchiveTail::Locate(index, plain.offset + 1, plain.size), "a different start is not this entry");
            Require(!ArchiveTail::Locate(index, plain.offset, plain.size, std::uint64_t{plain.offset} + plain.size), "an unindexed archive that could hold it rules it out");
            Require(ArchiveTail::Locate(index, plain.offset, plain.size, std::uint64_t{plain.offset} + plain.size - 1).has_value(), "an unindexed archive too small to hold it does not");

            const auto head = ArchiveTail::ReadHead(*located);
            Require(head && head->original == original, "entry head read");
            Require(std::memcmp(head->dds.data(), dds.data(), dds.size()) == 0, "DDS headers decoded");
            const auto info = SkyrimSource::Parse(head->dds, head->original);
            Require(info.has_value(), "DDS headers parsed from the head");
            const auto plan = MipTail::PlanFor(*info, 2, 256, 1);
            Require(plan.has_value(), "planned");
            Require(MipTail::CanSkip(head->frame, plan->tailOffset), "a repacked entry can be skipped into");
            Bytes tail(static_cast<std::size_t>(plan->tailBytes));
            Require(ArchiveTail::ReadTail(*located, *head, plan->tailOffset, plan->tailOffset + plan->tailBytes, tail.data()), "kept mips read");
            Require(std::memcmp(tail.data(), content.data() + plan->tailOffset, tail.size()) == 0, "kept mips match the file");

            const auto other = ArchiveTail::Locate(index, chained.offset, chained.size);
            const auto otherHead = other ? ArchiveTail::ReadHead(*other) : std::nullopt;
            Require(otherHead.has_value(), "linked entry head read");
            Require(!MipTail::CanSkip(otherHead->frame, plan->tailOffset), "a linked entry cannot be skipped into");

            // A full texture, read from the archive without the game's stream: both layouts.
            Bytes whole(content.size(), 0);
            Require(ArchiveTail::ReadEntry(*located, *head, whole.data()) && whole == content, "a repacked entry read whole");
            std::fill(whole.begin(), whole.end(), std::uint8_t{0});
            Require(ArchiveTail::ReadEntry(*other, *otherHead, whole.data()) && whole == content, "a linked entry read whole");
            auto shorter = *otherHead;
            shorter.original -= 1;
            Require(!ArchiveTail::ReadEntry(*other, shorter, whole.data()), "an entry larger than its header says is refused");
        }
        {
            // The same offset in two archives: only a size that tells them apart will do.
            ArchiveIndex::Index index;
            index.archives.push_back({open(), "a", false, true});
            index.archives.push_back({open(), "b", false, false});
            index.entries = {{plain.offset, plain.size, 0}, {plain.offset, plain.size + 10, 1}};
            Require(ArchiveTail::Locate(index, plain.offset, plain.size).has_value(), "told apart by stored size");
            index.entries = {{plain.offset, plain.size, 0}, {plain.offset, plain.size, 1}};
            Require(!ArchiveTail::Locate(index, plain.offset, plain.size).has_value(), "ambiguous between archives");
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    // A small archive with its names, as Archive.exe lays one out.
    void Paths() {
        const auto path = std::filesystem::temp_directory_path() / ("miptail-paths-" + std::to_string(GetCurrentProcessId()) + ".bsa");
        const std::vector<std::pair<std::string, std::vector<std::string>>> folders{
            {"textures\\actors\\character", {"Body.dds", "face.dds"}},
            {"Textures/Architecture", {"wall.dds"}},
        };
        const auto put32 = [](Bytes& a_b, std::uint32_t a_v) { for (int i = 0; i < 4; ++i) a_b.push_back(static_cast<std::uint8_t>(a_v >> (8 * i))); };
        const auto put64 = [&](Bytes& a_b, std::uint64_t a_v) { put32(a_b, static_cast<std::uint32_t>(a_v)); put32(a_b, static_cast<std::uint32_t>(a_v >> 32)); };

        std::uint32_t folderNames = 0, fileNames = 0, files = 0;
        for (const auto& [folder, names] : folders) {
            folderNames += static_cast<std::uint32_t>(folder.size()) + 1;
            for (const auto& name : names) { fileNames += static_cast<std::uint32_t>(name.size()) + 1; ++files; }
        }
        const std::uint32_t tableEnd = 36 + 24 * static_cast<std::uint32_t>(folders.size()) + folderNames + static_cast<std::uint32_t>(folders.size()) + 16 * files;
        const std::uint32_t dataStart = tableEnd + fileNames;
        Bytes file;
        for (const char c : {'B', 'S', 'A', '\0'}) file.push_back(static_cast<std::uint8_t>(c));
        put32(file, 105); put32(file, 36); put32(file, 0x3); put32(file, static_cast<std::uint32_t>(folders.size())); put32(file, files);
        put32(file, folderNames); put32(file, fileNames); put32(file, 2);
        std::uint32_t recordAt = 36 + 24 * static_cast<std::uint32_t>(folders.size());
        for (const auto& [folder, names] : folders) {
            put64(file, 1); put32(file, static_cast<std::uint32_t>(names.size())); put32(file, 0);
            put64(file, std::uint64_t{recordAt} + fileNames);
            recordAt += 1 + static_cast<std::uint32_t>(folder.size()) + 1 + 16 * static_cast<std::uint32_t>(names.size());
        }
        std::uint32_t dataAt = dataStart;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> stored;
        for (const auto& [folder, names] : folders) {
            file.push_back(static_cast<std::uint8_t>(folder.size() + 1));
            file.insert(file.end(), folder.begin(), folder.end());
            file.push_back(0);
            for (std::size_t i = 0; i < names.size(); ++i) {
                const std::uint32_t size = 100 + static_cast<std::uint32_t>(stored.size());
                put64(file, 2); put32(file, size); put32(file, dataAt);
                stored.emplace_back(dataAt, size);
                dataAt += size;
            }
        }
        for (const auto& [folder, names] : folders)
            for (const auto& name : names) { file.insert(file.end(), name.begin(), name.end()); file.push_back(0); }
        file.resize(dataAt, 0xEE);
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        }

        std::vector<std::string> seen;
        std::vector<ArchiveTail::PathEntry> entries;
        const auto match = [&](std::string_view a_path) { seen.emplace_back(a_path); return a_path.find("actors\\character\\") != std::string_view::npos; };
        Require(ArchiveTail::AddArchivePaths(path, match, entries), "paths read");
        Require(seen == std::vector<std::string>{"textures\\actors\\character\\body.dds", "textures\\actors\\character\\face.dds", "textures\\architecture\\wall.dds"}, "paths in lower case with backslashes");
        Require(entries.size() == 3 && entries[0].match && entries[1].match && !entries[2].match, "each entry has its answer");

        using Answer = ArchiveTail::PathAnswer;
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
        Require(ArchiveTail::LookUp(entries, stored[0].first, stored[0].second) == Answer::kYes, "a matching entry");
        Require(ArchiveTail::LookUp(entries, stored[2].first, stored[2].second) == Answer::kNo, "an entry that does not match");
        Require(ArchiveTail::LookUp(entries, stored[2].first, stored[2].second + 1) == Answer::kUnknown, "another size is another entry");
        Require(ArchiveTail::LookUp(entries, 5, 100) == Answer::kUnknown, "an offset no entry has");
        Require(ArchiveTail::LookUp(entries, stored[0].first, stored[0].second, 1, std::uint64_t{stored[0].first} + stored[0].second) == Answer::kUnknown,
                "an unread archive large enough to hold the entry leaves it unknown");
        Require(ArchiveTail::LookUp(entries, stored[0].first, stored[0].second, 1, std::uint64_t{stored[0].first} + stored[0].second - 1) == Answer::kYes,
                "an unread archive too small to hold the entry changes nothing");
        Require(ArchiveTail::LookUp(entries, stored[0].first, stored[0].second, 2) == Answer::kNo, "a bit the test did not set does not count");

        auto clash = entries;
        clash.push_back({stored[2].first, stored[2].second, true});
        std::sort(clash.begin(), clash.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
        Require(ArchiveTail::LookUp(clash, stored[2].first, stored[2].second) == Answer::kUnknown, "two archives that disagree");
        clash.back() = {stored[2].first, stored[2].second, false};
        clash.erase(std::remove_if(clash.begin(), clash.end(), [&](const auto& e) { return e.offset == stored[2].first && e.match; }), clash.end());
        clash.push_back({stored[2].first, stored[2].second, false});
        std::sort(clash.begin(), clash.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
        Require(ArchiveTail::LookUp(clash, stored[2].first, stored[2].second) == Answer::kNo, "two archives that agree");

        Bytes broken(file.begin(), file.begin() + 40);
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(broken.data()), static_cast<std::streamsize>(broken.size()));
        }
        std::vector<ArchiveTail::PathEntry> none;
        Require(!ArchiveTail::AddArchivePaths(path, match, none) && none.empty(), "a broken archive adds nothing");
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    void EmbeddedNames() {
        const auto content = Content((64u << 10) * 4, 51, 30);
        const auto frame = Frame(content, {});
        const auto path = std::filesystem::temp_directory_path() / ("miptail-names-" + std::to_string(GetCurrentProcessId()) + ".bin");
        const std::string name = "textures\\named.dds";
        const auto original = static_cast<std::uint32_t>(content.size());
        Bytes file(512, 0xCD);
        const auto at = static_cast<std::uint32_t>(file.size());
        file.push_back(static_cast<std::uint8_t>(name.size()));
        file.insert(file.end(), name.begin(), name.end());
        file.resize(file.size() + 4);
        std::memcpy(file.data() + file.size() - 4, &original, 4);
        file.insert(file.end(), frame.begin(), frame.end());
        const auto stored = static_cast<std::uint32_t>(file.size() - at);
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        }
        {
            ArchiveIndex::Index index;
            index.archives.push_back({CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr), "named", true, true});
            index.entries = {{at, stored, 0}};
            const auto skip = static_cast<std::uint32_t>(1 + name.size());
            auto located = ArchiveTail::Locate(index, at, stored);
            Require(located && located->offset == at + skip && located->bytes == stored - skip, "from the record offset, past the name");
            located = ArchiveTail::Locate(index, at + skip, stored - skip);
            Require(located && located->offset == at + skip, "from past the name");
            const auto head = ArchiveTail::ReadHead(*located);
            Require(head && head->original == original, "head past the name");
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
}

namespace {
    // Every large texture of a real archive: its small mips read from the end,
    // byte for byte against a full decode of the entry.
    int VerifyArchive(const std::filesystem::path& a_path) {
        std::ifstream in(a_path, std::ios::binary);
        Bsa::Table table;
        const auto problem = Bsa::ReadTable(in, table, false);
        Require(problem.empty(), "archive table of " + a_path.string() + ": " + problem);

        ArchiveIndex::Index index;
        const auto handle = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "archive opened");
        index.archives.push_back({handle, a_path.filename().string(), table.EmbedNames(), true});
        for (const auto& entry : table.entries)
            if (entry.compressed && entry.size >= 8) index.entries.push_back({entry.offset, entry.size, 0});
        std::sort(index.entries.begin(), index.entries.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });

        // Each entry's path, through the same lookup the plugin makes.
        std::vector<ArchiveTail::PathEntry> paths;
        std::vector<std::string> samples;
        std::uint64_t characters = 0;
        const auto character = [&](std::string_view a_path) {
            if (samples.size() < 2) samples.emplace_back(a_path);
            const bool yes = a_path.find("actors\\character\\") != std::string_view::npos;
            characters += yes;
            return yes;
        };
        Require(ArchiveTail::AddArchivePaths(a_path, character, paths), "archive paths read");
        std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
        std::uint64_t answered = 0;
        for (const auto& entry : table.entries)
            answered += ArchiveTail::LookUp(paths, entry.offset, entry.size) != ArchiveTail::PathAnswer::kUnknown;
        std::cout << a_path.filename().string() << ": " << paths.size() << " paths, " << characters << " character textures, " << answered << " of " << table.entries.size()
                  << " answered by lookup; for example " << (samples.empty() ? "" : samples[0]) << '\n';
        Require(answered == table.entries.size(), "every entry answered by lookup");

        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const auto now = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; };
        const auto ms = [&](std::int64_t a_ticks) { return 1000.0 * static_cast<double>(a_ticks) / static_cast<double>(frequency.QuadPart); };

        std::uint64_t entries = 0, unlocated = 0, notDds = 0, noPlan = 0, whole = 0, verified = 0, wrong = 0, undecoded = 0;
        std::uint64_t fileBytes = 0, wholeBytes = 0, skippedBytes = 0, tailBytes = 0;
        std::int64_t tailTicks = 0, fullTicks = 0;
        Bytes raw, decoded, tail;
        for (const auto& entry : index.entries) {
            ++entries;
            const auto located = ArchiveTail::Locate(index, entry.offset, entry.size);
            const auto head = located ? ArchiveTail::ReadHead(*located) : std::nullopt;
            if (!head) { ++unlocated; continue; }
            const auto info = SkyrimSource::Parse(head->dds, head->original);
            if (!info) { ++notDds; continue; }
            const auto plan = MipTail::PlanFor(*info, 2, 512, 256u << 10);
            if (!plan) { ++noPlan; continue; }
            if (!MipTail::CanSkip(head->frame, plan->tailOffset)) { ++whole; wholeBytes += head->original; continue; }

            tail.resize(static_cast<std::size_t>(plan->tailBytes));
            const auto started = now();
            const bool read = ArchiveTail::ReadTail(*located, *head, plan->tailOffset, plan->tailOffset + plan->tailBytes, tail.data());
            tailTicks += now() - started;
            if (!read) { ++undecoded; std::cerr << "not decoded: entry at " << entry.offset << '\n'; continue; }

            raw.resize(located->bytes);
            decoded.resize(head->original);
            const auto fullStarted = now();
            Require(ArchiveIndex::ReadAt(located->handle, located->offset, raw.data(), located->bytes), "entry read");
            LZ4F_dctx* context = nullptr;
            Require(!LZ4F_isError(LZ4F_createDecompressionContext(&context, LZ4F_VERSION)), "decoder created");
            std::size_t srcBytes = raw.size() - 4, dstBytes = decoded.size();
            const auto rc = LZ4F_decompress(context, decoded.data(), &dstBytes, raw.data() + 4, &srcBytes, nullptr);
            LZ4F_freeDecompressionContext(context);
            fullTicks += now() - fullStarted;
            Require(!LZ4F_isError(rc) && dstBytes == decoded.size(), "entry decoded in full");

            if (std::memcmp(tail.data(), decoded.data() + plan->tailOffset, tail.size()) != 0) { ++wrong; std::cerr << "WRONG BYTES: entry at " << entry.offset << '\n'; }
            else ++verified;
            fileBytes += head->original;
            skippedBytes += plan->skippedBytes;
            tailBytes += plan->tailBytes;
        }
        const auto mib = [](std::uint64_t a_bytes) { return static_cast<double>(a_bytes) / (1024.0 * 1024.0); };
        std::cout << a_path.filename().string() << ": " << entries << " entries, " << unlocated << " unlocated, " << notDds << " not supported DDS, " << noPlan << " nothing to gain, "
                  << whole << " whole frame (" << mib(wholeBytes) << " MiB)\n  from the end: " << verified << " verified, " << wrong << " wrong, " << undecoded << " undecoded; "
                  << mib(fileBytes) << " MiB files, " << mib(tailBytes) << " MiB kept, " << mib(skippedBytes) << " MiB left out\n  time: small mips " << ms(tailTicks) << " ms, read and decode whole " << ms(fullTicks) << " ms\n";
        return wrong || undecoded ? 1 : 0;
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        int failed = 0;
        for (int i = 1; i < argc; ++i) failed |= VerifyArchive(argv[i]);
        return failed;
    }
    TailAcrossFrames();
    TotalOnBlockBoundary();
    FalseBlockHeaders();
    TooShortWindowsFail();
    LinkedFramesAreNotSkipped();
    StartOfFrame();
    Plans();
    Headers();
    Archive();
    Paths();
    EmbeddedNames();
    std::cout << "mip tail tests passed\n";
    return 0;
}
