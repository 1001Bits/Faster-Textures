#pragma once

// Reading a texture's small mips straight from its archive entry, past the
// stream the game holds: a stream reads an entry from its start, and Faster
// Archives answers a stream's first read by decoding the whole entry. Uses Faster
// Archives' own archive index and format code, so repacked archives and the
// plugin stay in step, and needs no plugin running to read them. Game-independent
// for the tests.

#include "MipTail.h"

#include "FasterArchives/ArchiveIndex.h"

#include <lz4frame.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ArchiveTail {
    // What a test of its path said about each archive entry. Most textures the game
    // creates have no name: it finds them by hash, and a stream only learns its name
    // when opened, and only in some archives. Every archive's table lists every path,
    // so one pass over the tables, off the loading path, answers for all of them.
    struct PathEntry {
        std::uint32_t offset{}, size{};
        std::uint8_t match{};  // what a_match said: bits, so that one table answers several tests
    };

    // Adds an archive's entries, a_match seeing each path in lower case with
    // backslashes and returning bits. False, adding nothing, for a file that is not
    // such an archive or whose table holds no names.
    template <class Match>
    [[nodiscard]] bool AddArchivePaths(const std::filesystem::path& a_file, const Match& a_match, std::vector<PathEntry>& a_out) {
        std::ifstream in(a_file, std::ios::binary);
        Bsa::Table table;
        if (!in || !Bsa::ReadTable(in, table, true).empty()) return false;
        const auto& h = table.header;
        if (!(h.archiveFlags & Bsa::kDirectoryStrings) || !(h.archiveFlags & Bsa::kFileStrings)) return false;
        const auto& meta = table.meta;
        if (meta.size() < h.totalFileNameLength) return false;

        const auto lower = [](char c) { return c == '/' ? '\\' : c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        std::size_t names = meta.size() - h.totalFileNameLength;
        std::size_t at = sizeof(Bsa::Header) + std::size_t{h.folderCount} * sizeof(Bsa::FolderRecord);
        std::size_t entry = 0;
        const auto start = a_out.size();
        std::string path;
        for (std::uint32_t f = 0; f < h.folderCount; ++f) {
            Bsa::FolderRecord folder;
            std::memcpy(&folder, meta.data() + sizeof(Bsa::Header) + std::size_t{f} * sizeof(Bsa::FolderRecord), sizeof(folder));
            const std::size_t length = meta[at];  // counts its terminating zero
            path.clear();
            for (std::size_t i = 0; i + 1 < length; ++i) path.push_back(lower(static_cast<char>(meta[at + 1 + i])));
            path.push_back('\\');
            const auto folderLength = path.size();
            at += 1 + length + std::size_t{folder.count} * sizeof(Bsa::FileRecord);
            for (std::uint32_t i = 0; i < folder.count; ++i, ++entry) {
                path.resize(folderLength);
                for (;; ++names) {
                    if (names >= meta.size()) {
                        a_out.resize(start);
                        return false;
                    }
                    if (meta[names] == 0) break;
                    path.push_back(lower(static_cast<char>(meta[names])));
                }
                ++names;
                a_out.push_back({table.entries[entry].offset, table.entries[entry].size, static_cast<std::uint8_t>(a_match(std::string_view(path)))});
            }
        }
        return true;
    }

    enum class PathAnswer { kUnknown, kNo, kYes };

    // Whether an archive missing from the tables could hold an entry starting at
    // a_start: a_unknownReach is the size of the largest such archive.
    [[nodiscard]] constexpr bool MaybeUnknown(std::uint32_t a_start, std::uint32_t a_stored, std::uint64_t a_unknownReach) {
        return std::uint64_t{a_start} + a_stored <= a_unknownReach;
    }

    // What the test said about the entry a stream starting at a_start reads,
    // a_stored being the stream's size before it is opened, as in Locate. a_sorted
    // is by offset; a_bits picks which of the test's bits count. Archives can agree
    // on an offset and size: then they must agree on the answer too. A stream does
    // not say which archive holds it, so an archive whose table could not be read
    // leaves unknown what it could hold.
    [[nodiscard]] inline PathAnswer LookUp(const std::vector<PathEntry>& a_sorted, std::uint32_t a_start, std::uint32_t a_stored,
                                           std::uint8_t a_bits = 1, std::uint64_t a_unknownReach = 0) {
        if (MaybeUnknown(a_start, a_stored, a_unknownReach)) return PathAnswer::kUnknown;
        const auto end = std::upper_bound(a_sorted.begin(), a_sorted.end(), a_start, [](std::uint32_t v, const PathEntry& e) { return v < e.offset; });
        auto first = end;
        while (first != a_sorted.begin() && a_start - (first - 1)->offset <= 256)  // 1 + 255: the longest embedded name
            --first;
        std::optional<bool> answer;
        for (auto it = first; it != end; ++it) {
            const std::uint32_t delta = a_start - it->offset;
            if (a_stored != it->size && (delta == 0 || a_stored != it->size - delta)) continue;
            const bool match = (it->match & a_bits) != 0;
            if (answer && *answer != match) return PathAnswer::kUnknown;
            answer = match;
        }
        return !answer ? PathAnswer::kUnknown : *answer ? PathAnswer::kYes : PathAnswer::kNo;
    }

    struct Located {
        HANDLE handle{};
        std::uint64_t offset{};  // of the entry's original-size field
        std::uint32_t bytes{};   // from there to the entry's end
        std::uint32_t archive{};  // in the index
    };

    // The entry a stream starting at a_start reads, a_stored being the stream's
    // size before it is opened. The stream may start at the entry's record offset
    // or, in archives with embedded names, past the name. A stream does not say
    // which archive holds it, so the answer must be unique among all archives, and
    // no archive missing from the index (a_unknownReach, as in LookUp) may hold it.
    [[nodiscard]] inline std::optional<Located> Locate(const ArchiveIndex::Index& a_index, std::uint32_t a_start, std::uint32_t a_stored,
                                                       std::uint64_t a_unknownReach = 0) {
        if (MaybeUnknown(a_start, a_stored, a_unknownReach)) return std::nullopt;
        const auto end = std::upper_bound(a_index.entries.begin(), a_index.entries.end(), a_start,
                                          [](std::uint32_t v, const ArchiveIndex::Entry& e) { return v < e.offset; });
        auto first = end;
        while (first != a_index.entries.begin() && a_start - (first - 1)->offset <= 256)  // 1 + 255: the longest embedded name
            --first;

        const ArchiveIndex::Entry* match = nullptr;
        std::uint32_t name = 0;
        for (auto it = first; it != end; ++it) {
            const auto& e = *it;
            const auto& archive = a_index.archives[e.archive];
            std::uint32_t skip = 0;
            if (archive.embedNames) {
                std::uint8_t length = 0;
                if (!ArchiveIndex::ReadAt(archive.handle, e.offset, &length, 1)) return std::nullopt;  // cannot be ruled out
                skip = 1u + length;
            }
            const std::uint32_t delta = a_start - e.offset;
            if ((delta != 0 && delta != skip) || skip + 8 > e.size) continue;
            if (a_stored != e.size && a_stored != e.size - delta) continue;
            // Records sharing one blob (identical files packed once) are one entry.
            if (match && (match->archive != e.archive || match->offset != e.offset)) return std::nullopt;
            match = &e;
            name = skip;
        }
        if (!match) return std::nullopt;
        return Located{a_index.archives[match->archive].handle, std::uint64_t{match->offset} + name, match->size - name, match->archive};
    }

    // The same for an entry whose archive is known: a table's record of it.
    [[nodiscard]] inline std::optional<Located> EntryAt(const ArchiveIndex::Index& a_index, std::uint32_t a_archive, std::uint32_t a_offset, std::uint32_t a_size) {
        const auto& archive = a_index.archives[a_archive];
        std::uint32_t name = 0;
        if (archive.embedNames) {
            std::uint8_t length = 0;
            if (!ArchiveIndex::ReadAt(archive.handle, a_offset, &length, 1)) return std::nullopt;
            name = 1u + length;
        }
        if (name + 8 > a_size) return std::nullopt;
        return Located{archive.handle, std::uint64_t{a_offset} + name, a_size - name, a_archive};
    }

    struct Head {
        Lz4Blocks::Header frame;
        std::uint32_t original{};      // decoded size of the entry
        std::uint64_t frameOffset{};   // in the archive
        std::uint64_t frameBytes{};
        std::array<std::byte, MipTail::kHeaderMax> dds{};
    };

    // The entry's frame header and its first decoded bytes, in one small read.
    // Compression cannot shrink 148 bytes into less than a few, and cannot grow
    // them much either, so 4 KiB always reaches them.
    [[nodiscard]] inline std::optional<Head> ReadHead(const Located& a_entry) {
        std::array<std::uint8_t, 4096> raw;
        const auto bytes = std::min<std::uint32_t>(a_entry.bytes, static_cast<std::uint32_t>(raw.size()));
        if (bytes < 4 + 7 || !ArchiveIndex::ReadAt(a_entry.handle, a_entry.offset, raw.data(), bytes)) return std::nullopt;
        Head head;
        std::memcpy(&head.original, raw.data(), 4);
        if (head.original < MipTail::kHeaderMax || !Lz4Blocks::ParseHeader(raw.data() + 4, bytes - 4, head.frame)) return std::nullopt;
        head.frameOffset = a_entry.offset + 4;
        head.frameBytes = a_entry.bytes - 4;
        if (!MipTail::DecodeStart(raw.data() + 4, bytes - 4, head.frame, reinterpret_cast<std::uint8_t*>(head.dds.data()), head.dds.size()))
            return std::nullopt;
        return head;
    }

    // Decoded bytes [a_from, a_to) of the entry into a_dst. Only for frames
    // MipTail::CanSkip accepts. Reads the frame's end: first what those blocks
    // likely take, then what they can take at most.
    [[nodiscard]] inline bool ReadTail(const Located& a_entry, const Head& a_head, std::uint64_t a_from, std::uint64_t a_to, std::uint8_t* a_dst) {
        thread_local std::vector<std::uint8_t> window;
        const auto body = a_head.frameBytes - a_head.frame.size;
        const auto attempt = [&](std::uint64_t a_bytes) {
            const auto bytes = std::min(a_bytes, body);
            if (bytes > (1ull << 30)) return false;
            window.resize(static_cast<std::size_t>(bytes));
            const auto at = a_head.frameOffset + a_head.frameBytes - bytes;
            return ArchiveIndex::ReadAt(a_entry.handle, at, window.data(), static_cast<std::uint32_t>(bytes)) &&
                   MipTail::DecodeTail(window.data(), window.size(), a_head.frame, a_head.original, a_from, a_to, a_dst);
        };
        const auto likely = MipTail::WindowLikely(a_head.frame, a_head.frameBytes, a_head.original, a_from);
        const auto certain = MipTail::WindowCertain(a_head.frame, a_head.original, a_from);
        const bool ok = attempt(likely) || (certain > likely && attempt(certain));
        if (window.capacity() > (16u << 20)) std::vector<std::uint8_t>().swap(window);  // one huge texture need not stay in memory
        return ok;
    }

    // The whole decoded entry, a_head.original bytes, into a_dst: any frame layout, linked blocks too.
    [[nodiscard]] inline bool ReadEntry(const Located& a_entry, const Head& a_head, std::uint8_t* a_dst) {
        thread_local std::vector<std::uint8_t> frame;
        if (a_head.frameBytes > (1ull << 30)) return false;
        frame.resize(static_cast<std::size_t>(a_head.frameBytes));
        bool ok = ArchiveIndex::ReadAt(a_entry.handle, a_head.frameOffset, frame.data(), static_cast<std::uint32_t>(frame.size()));
        LZ4F_dctx* context = nullptr;
        if (ok && !LZ4F_isError(LZ4F_createDecompressionContext(&context, LZ4F_VERSION))) {
            std::size_t written = a_head.original, read = frame.size();
            const auto left = LZ4F_decompress(context, a_dst, &written, frame.data(), &read, nullptr);
            ok = left == 0 && written == a_head.original;  // 0: the frame ended, all of it decoded
            LZ4F_freeDecompressionContext(context);
        } else {
            ok = false;
        }
        if (frame.capacity() > (16u << 20)) std::vector<std::uint8_t>().swap(frame);
        return ok;
    }
}
