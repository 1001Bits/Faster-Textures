// usage: tail_cache_tests                     the store and the filler on made-up data
//        tail_cache_tests <folder> <archive>  fills a store in <folder> from real archives,
//                                             each tail checked against a full decode: sizes and times
#include "TailCache.h"

#include <lz4frame.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    Bytes Frame(const Bytes& a_content, LZ4F_blockSizeID_t a_size, LZ4F_blockMode_t a_mode, bool a_checksums = false) {
        LZ4F_preferences_t preferences{};
        preferences.frameInfo.blockSizeID = a_size;
        preferences.frameInfo.blockMode = a_mode;
        preferences.frameInfo.blockChecksumFlag = a_checksums ? LZ4F_blockChecksumEnabled : LZ4F_noBlockChecksum;
        preferences.frameInfo.contentChecksumFlag = a_checksums ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
        Bytes out(LZ4F_compressFrameBound(a_content.size(), &preferences));
        const auto written = LZ4F_compressFrame(out.data(), out.size(), a_content.data(), a_content.size(), &preferences);
        Require(!LZ4F_isError(written), "frame compressed");
        out.resize(written);
        return out;
    }

    void Decodes() {
        for (const auto noise : {0, 40, 100}) {
            for (const auto size : {std::size_t{1}, std::size_t{70000}, std::size_t{9u << 20}}) {
                const auto content = Content(size, static_cast<std::uint32_t>(size) + noise, noise);
                for (const auto block : {LZ4F_max64KB, LZ4F_max256KB, LZ4F_max1MB, LZ4F_max4MB}) {
                    for (const auto mode : {LZ4F_blockLinked, LZ4F_blockIndependent}) {
                        for (const bool checksums : {false, true}) {
                            const auto frame = Frame(content, block, mode, checksums);
                            Lz4Blocks::Header header;
                            Require(Lz4Blocks::ParseHeader(frame.data(), frame.size(), header), "header parsed");
                            Bytes out(content.size());
                            Require(TailCache::DecodeFrame(frame.data(), frame.size(), header, out.data(), out.size()), "frame decoded");
                            Require(out == content, "decoded bytes match");
                            if (content.size() > 1) {
                                Require(!TailCache::DecodeFrame(frame.data(), frame.size() / 2, header, out.data(), out.size()), "cut frame fails");
                                Bytes shorter(content.size() - 1);
                                Require(!TailCache::DecodeFrame(frame.data(), frame.size(), header, shorter.data(), shorter.size()), "a smaller total fails");
                            }
                        }
                    }
                }
            }
        }
    }

    std::filesystem::path NewFolder(const std::string& a_name) {
        const auto folder = std::filesystem::temp_directory_path() / ("tailcache-" + a_name + "-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(folder);
        return folder;
    }

    TailCache::What WhatFor(const Bytes& a_tail, std::uint64_t a_head = 7) {
        return {a_head, 1000000, 500000, static_cast<std::uint32_t>(a_tail.size())};
    }

    bool Matches(TailCache::Store& a_store, const TailCache::Key& a_key, const TailCache::What& a_what, const Bytes& a_tail) {
        Bytes out(a_tail.size());
        return a_store.Read(a_key, a_what, out.data()) && out == a_tail;
    }

    void Stores() {
        const auto folder = NewFolder("store");
        const std::vector<std::uint64_t> live{1, 2};
        const auto tailA = Content(300000, 1, 40), tailB = Content(5000, 2, 40);
        const TailCache::Key a{1, 4096}, b{2, 4096}, c{1, 8192};
        {
            TailCache::Store store;
            Require(store.Open(folder, 1u << 30, live), "opens anew");
            TailCache::Store second;
            Require(!second.Open(folder, 1u << 30, live), "a second game cannot hold it");
            Require(!store.Has(a, WhatFor(tailA)), "empty at first");
            Require(store.Add(a, WhatFor(tailA), tailA.data()), "added");
            Require(store.Add(b, WhatFor(tailB), tailB.data()), "added second");
            store.Drain();
            Require(store.Current().entries == 2 && store.Current().bytes == tailA.size() + tailB.size(), "counted");
            Require(Matches(store, a, WhatFor(tailA), tailA), "reads back");
            Require(Matches(store, b, WhatFor(tailB), tailB), "reads back second");
            Require(!store.Add(a, WhatFor(tailA), tailA.data()), "the same tail again is not needed");
            Bytes out(tailA.size());
            Require(!store.Read(a, WhatFor(tailA, 8), out.data()), "other headers are another tail");
            Require(!store.Read(c, WhatFor(tailA), out.data()), "another entry");
            auto other = WhatFor(tailA);
            other.tailOffset += 16;
            Require(!store.Read(a, other, out.data()), "another plan");
        }
        {
            TailCache::Store store;
            Require(store.Open(folder, 1u << 30, live), "opens again");
            Require(Matches(store, a, WhatFor(tailA), tailA), "kept across runs");
            Require(Matches(store, b, WhatFor(tailB), tailB), "kept across runs, second");
            // A new plan for the same entry replaces the old tail.
            auto other = WhatFor(tailB);
            other.tailOffset += 16;
            Require(store.Add(b, other, tailB.data()), "replaced");
            store.Drain();
            Bytes out(tailB.size());
            Require(!store.Read(b, WhatFor(tailB), out.data()), "the old plan is gone");
            Require(Matches(store, b, other, tailB), "the new plan reads");
        }
        const auto records = folder / "tails.idx", data = folder / "tails.dat";
        const auto recordBytes = std::filesystem::file_size(records), dataBytes = std::filesystem::file_size(data);

        // A wrong byte in a tail: that entry is gone, nothing else, and it can come back.
        {
            std::fstream file(data, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(100);
            file.put('\x7F');
            file.seekp(101);
            file.put('\x01');
        }
        {
            TailCache::Store store;
            Require(store.Open(folder, 1u << 30, live), "opens with a damaged tail");
            Bytes out(tailA.size());
            Require(!store.Read(a, WhatFor(tailA), out.data()), "a damaged tail is not served");
            Require(!store.Has(a, WhatFor(tailA)), "and forgotten");
            Require(store.Add(a, WhatFor(tailA), tailA.data()), "and can be stored again");
            store.Drain();
            Require(Matches(store, a, WhatFor(tailA), tailA), "reads after storing again");
        }

        // A crash while writing: half a record, or a record whose tail never landed.
        {
            const auto folder2 = NewFolder("crash");
            {
                TailCache::Store store;
                Require(store.Open(folder2, 1u << 30, live), "opens");
                store.Add(a, WhatFor(tailA), tailA.data());
                store.Drain();
                store.Add(b, WhatFor(tailB), tailB.data());
                store.Drain();
            }
            const auto idx = folder2 / "tails.idx", dat = folder2 / "tails.dat";
            std::filesystem::resize_file(idx, std::filesystem::file_size(idx) - 10);
            {
                TailCache::Store store;
                Require(store.Open(folder2, 1u << 30, live), "opens after half a record");
                Require(Matches(store, a, WhatFor(tailA), tailA), "first survives half a record");
                Require(!store.Has(b, WhatFor(tailB)), "second is lost");
                Require(store.Current().bytes == tailA.size(), "its unfinished tail is dropped");
                Require(store.Add(b, WhatFor(tailB), tailB.data()), "stores again");
                store.Drain();
                Require(Matches(store, b, WhatFor(tailB), tailB), "reads after half a record");
            }
            std::filesystem::resize_file(dat, tailA.size() + 10);
            {
                TailCache::Store store;
                Require(store.Open(folder2, 1u << 30, live), "opens with a short file");
                Require(Matches(store, a, WhatFor(tailA), tailA), "first survives a short file");
                Require(!store.Has(b, WhatFor(tailB)), "second is lost with its tail");
            }
            std::filesystem::remove_all(folder2);
        }

        // Garbage instead of a store: starts anew.
        {
            const auto folder3 = NewFolder("garbage");
            std::filesystem::create_directories(folder3);
            std::ofstream(folder3 / "tails.idx", std::ios::binary) << "not a store at all, and long enough to read";
            std::ofstream(folder3 / "tails.dat", std::ios::binary) << "whatever";
            TailCache::Store store;
            Require(store.Open(folder3, 1u << 30, live), "opens over garbage");
            Require(store.Current().entries == 0 && store.Current().bytes == 0, "empty");
            Require(store.Add(a, WhatFor(tailA), tailA.data()), "usable");
            store.Drain();
            Require(Matches(store, a, WhatFor(tailA), tailA), "reads");
            store.Close();
            std::filesystem::remove_all(folder3);
        }

        // An updated mod's old tails: kept while few, cleared when they waste a quarter of the file.
        {
            const auto folder4 = NewFolder("stale");
            const auto big = Content(900000, 3, 40), little = Content(100000, 4, 40);
            {
                TailCache::Store store;
                Require(store.Open(folder4, 1u << 30, {1, 2}), "opens");
                store.Add(a, WhatFor(big), big.data());
                store.Add(b, WhatFor(little), little.data());
                store.Drain();
            }
            {
                TailCache::Store store;
                Require(store.Open(folder4, 1u << 30, {1}), "little stale");
                Require(Matches(store, a, WhatFor(big), big), "live tails stay");
                Require(!store.Has(b, WhatFor(little)), "a gone archive's tail is not offered");
                Require(store.Current().bytes == big.size() + little.size(), "not worth clearing yet");
            }
            {
                TailCache::Store store;
                Require(store.Open(folder4, 1u << 30, {2}), "much stale");
                Require(store.Current().entries == 0 && store.Current().bytes == 0, "cleared");
            }
            std::filesystem::remove_all(folder4);
        }

        // The limit.
        {
            const auto folder5 = NewFolder("limit");
            const auto tail = Content(1000, 5, 40);
            TailCache::Store store;
            Require(store.Open(folder5, 2500, live), "opens");
            Require(store.Add({1, 1}, WhatFor(tail), tail.data()), "first fits");
            Require(store.Add({1, 2}, WhatFor(tail), tail.data()), "second fits");
            Require(!store.HasRoom(1000), "no room left");
            Require(!store.Add({1, 3}, WhatFor(tail), tail.data()), "third does not fit");
            store.Drain();
            Require(store.Current().entries == 2 && store.Current().bytes == 2000 && store.Current().full, "stopped at the limit");
            store.Close();
            std::filesystem::remove_all(folder5);
        }

        // Only the setting limits it, whatever the disk has free; a full disk stops it at the first write that fails.
        {
            const auto folder7 = NewFolder("disk");
            const auto tail = Content(1000, 7, 40);
            {
                TailCache::Store store;
                Require(store.Open(folder7, 1u << 30, live), "opens");
                Require(store.Add(a, WhatFor(tail), tail.data()), "added");
                store.Drain();
            }
            TailCache::Store store;
            Require(store.Open(folder7, 1u << 30, live), "opens again");
            Require(store.Current().maxBytes == (1u << 30), "the setting decides");
            Require(Matches(store, a, WhatFor(tail), tail), "still reads");
            Require(store.HasRoom(1) && !store.WriteFailed(), "has room, nothing failed");
            store.Close();
            std::filesystem::remove_all(folder7);
        }

        // From many threads: what the loading threads and the filler do together.
        {
            const auto folder6 = NewFolder("threads");
            TailCache::Store store;
            Require(store.Open(folder6, 1u << 30, live), "opens");
            const auto tail = Content(20000, 6, 40);
            std::vector<std::thread> threads;
            std::atomic<int> bad{0};
            for (std::uint64_t t = 0; t < 8; ++t) {
                threads.emplace_back([&, t] {
                    Bytes out(tail.size());
                    for (std::uint64_t i = 0; i < 300; ++i) {
                        const TailCache::Key key{1, 1 + i};  // the same keys from every thread
                        store.WaitForQueueBelow(8u << 20);
                        store.Add(key, WhatFor(tail), tail.data());
                        if (store.Read({1, 1 + (i * 7 + t) % 300}, WhatFor(tail), out.data()) && out != tail) ++bad;
                    }
                });
            }
            for (auto& thread : threads) thread.join();
            store.Drain();
            Require(bad == 0, "no thread read wrong bytes");
            Require(store.Current().entries == 300 && store.Current().bytes == 300 * tail.size(), "each key stored once");
            store.Close();
            std::filesystem::remove_all(folder6);
        }

        std::filesystem::remove_all(folder);
        (void)recordBytes;
        (void)dataBytes;
    }

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

    const auto kPlanOf = [](const SkyrimSource::Info& a_file) { return MipTail::PlanFor(a_file, 2, 512, 256u << 10); };

    void Fills() {
        constexpr std::uint32_t bc1 = 71;
        const auto mipData = TextureFormat::ChainBytes(static_cast<std::uint8_t>(bc1), 2048, 2048, 0, 12);
        auto content = Content(static_cast<std::size_t>(148 + mipData), 41, 40);
        const auto dds = Dds(2048, 2048, 12, bc1);
        std::memcpy(content.data(), dds.data(), dds.size());
        auto smallContent = Content(148 + 2048, 42, 40);
        const auto smallDds = Dds(32, 32, 1, bc1);
        std::memcpy(smallContent.data(), smallDds.data(), smallDds.size());

        const auto path = std::filesystem::temp_directory_path() / ("tailcache-archive-" + std::to_string(GetCurrentProcessId()) + ".bin");
        Bytes file(4096, 0xAB);
        const auto append = [&](const Bytes& a_content, const Bytes& a_frame) {
            const auto at = static_cast<std::uint32_t>(file.size());
            const auto original = static_cast<std::uint32_t>(a_content.size());
            file.resize(file.size() + 4);
            std::memcpy(file.data() + file.size() - 4, &original, 4);
            file.insert(file.end(), a_frame.begin(), a_frame.end());
            return ArchiveIndex::Entry{at, static_cast<std::uint32_t>(file.size() - at), 0};
        };
        const auto linked = append(content, Frame(content, LZ4F_max4MB, LZ4F_blockLinked));
        const auto single = append(content, Frame(content, LZ4F_max4MB, LZ4F_blockIndependent));
        const auto repacked = append(content, Frame(content, LZ4F_max64KB, LZ4F_blockIndependent));
        const auto tiny = append(smallContent, Frame(smallContent, LZ4F_max4MB, LZ4F_blockLinked));
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        }
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "archive opened");
        const auto located = [&](const ArchiveIndex::Entry& a_entry) { return ArchiveTail::Located{handle, a_entry.offset, a_entry.size, 0}; };

        const auto id = TailCache::ArchiveId(handle, "Some Archive.BSA");
        Require(id != 0 && id == TailCache::ArchiveId(handle, "some archive.bsa"), "archive identity ignores case");
        Require(id != TailCache::ArchiveId(handle, "another.bsa"), "archive identity sees the name");

        const auto folder = NewFolder("fill");
        TailCache::Store store;
        Require(store.Open(folder, 1u << 30, {id}), "opens");
        TailCache::FillBuffers buffers;
        Require(TailCache::FillEntry(store, located(linked), id, kPlanOf, buffers) == TailCache::Filled::kAdded, "a linked frame is filled");
        Require(TailCache::FillEntry(store, located(single), id, kPlanOf, buffers) == TailCache::Filled::kAdded, "a single-block frame is filled");
        Require(TailCache::FillEntry(store, located(repacked), id, kPlanOf, buffers) == TailCache::Filled::kNotNeeded, "a repacked frame reads from its end");
        Require(TailCache::FillEntry(store, located(tiny), id, kPlanOf, buffers) == TailCache::Filled::kNotNeeded, "a small texture loads whole");
        store.Drain();
        Require(TailCache::FillEntry(store, located(linked), id, kPlanOf, buffers) == TailCache::Filled::kHad, "not filled twice");

        // What the plugin does on a load: the real head says what to ask for.
        for (const auto& entry : {linked, single}) {
            const auto l = located(entry);
            const auto head = ArchiveTail::ReadHead(l);
            Require(head.has_value(), "head read");
            const auto info = SkyrimSource::Parse(head->dds, head->original);
            const auto plan = kPlanOf(*info);
            Require(plan.has_value(), "planned");
            Bytes tail(static_cast<std::size_t>(plan->tailBytes));
            Require(store.Read({id, l.offset}, TailCache::WhatOf(*head, *plan), tail.data()), "tail found");
            Require(std::memcmp(tail.data(), content.data() + plan->tailOffset, tail.size()) == 0, "tail holds the file's small mips");
        }
        store.Close();
        CloseHandle(handle);
        std::filesystem::remove_all(folder);
        std::filesystem::remove(path);
    }

    // ── Real archives ───────────────────────────────────────────────────────

    int RealArchives(const std::filesystem::path& a_folder, const std::vector<std::filesystem::path>& a_archives) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const auto now = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; };
        const auto seconds = [&](std::int64_t a_ticks) { return static_cast<double>(a_ticks) / static_cast<double>(frequency.QuadPart); };
        const auto mib = [](std::uint64_t a_bytes) { return static_cast<double>(a_bytes) / (1024.0 * 1024.0); };
        const std::vector<std::string_view> exclude{"interface\\", "\\maps\\", "mapmarker", "\\lod\\", "terrain\\", "fonts\\", "book", "\\sky\\", "effects\\", "cubemaps\\", "actors\\character\\"};

        std::vector<std::uint64_t> live;
        std::vector<HANDLE> handles;
        for (const auto& archive : a_archives) {
            const auto handle = CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, nullptr);
            Require(handle != INVALID_HANDLE_VALUE, "archive opened");
            handles.push_back(handle);
            live.push_back(TailCache::ArchiveId(handle, archive.filename().string()));
        }
        TailCache::Store store;
        Require(store.Open(a_folder, 64ull << 30, live), "store opened");

        std::uint64_t added = 0, had = 0, notNeeded = 0, failed = 0, tailBytes = 0, packedBytes = 0, wrong = 0, excluded = 0, notTexture = 0;
        std::int64_t fillTicks = 0;
        const auto allStarted = now();
        Bytes packed;
        struct Check { TailCache::Key key; TailCache::What what; };
        std::vector<Check> checks;
        for (std::size_t a = 0; a < a_archives.size(); ++a) {
            std::ifstream in(a_archives[a], std::ios::binary);
            Bsa::Table table;
            Require(Bsa::ReadTable(in, table, true).empty(), "table read");
            std::vector<ArchiveTail::PathEntry> paths;
            std::vector<char> isTexture;
            const auto match = [&](std::string_view a_path) {
                isTexture.push_back(a_path.ends_with(".dds"));
                for (const auto part : exclude)
                    if (a_path.find(part) != std::string_view::npos) return true;
                return false;
            };
            Require(ArchiveTail::AddArchivePaths(a_archives[a], match, paths), "paths read");
            TailCache::FillBuffers buffers;
            for (std::size_t i = 0; i < paths.size(); ++i) {
                if (!isTexture[i]) { ++notTexture; continue; }
                if (paths[i].match) { ++excluded; continue; }
                const auto& p = paths[i];
                ArchiveIndex::Index one;
                one.archives.push_back({handles[a], a_archives[a].filename().string(), table.EmbedNames(), false});
                const auto entry = ArchiveTail::EntryAt(one, 0, p.offset, p.size);
                one.archives.clear();  // the handle is not its to close
                if (!entry) continue;
                const auto located = *entry;
                const auto started = now();
                const auto result = TailCache::FillEntry(store, located, live[a], kPlanOf, buffers);
                fillTicks += now() - started;
                switch (result) {
                case TailCache::Filled::kHad: ++had; break;
                case TailCache::Filled::kNotNeeded: ++notNeeded; break;
                case TailCache::Filled::kFailed: ++failed; break;
                default: break;
                }
                if (result != TailCache::Filled::kAdded) continue;
                ++added;
                const auto head = ArchiveTail::ReadHead(located);
                const auto info = SkyrimSource::Parse(head->dds, head->original);
                const auto plan = kPlanOf(*info);
                const auto what = TailCache::WhatOf(*head, *plan);
                tailBytes += what.tailBytes;
                checks.push_back({{live[a], located.offset}, what});
                // How far would compressing the tails get? And the frame against LZ4's own decoder.
                packed.resize(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(what.tailBytes))));
                packedBytes += static_cast<std::uint64_t>(LZ4_compress_default(reinterpret_cast<const char*>(buffers.decoded.data() + what.tailOffset), reinterpret_cast<char*>(packed.data()), static_cast<int>(what.tailBytes), static_cast<int>(packed.size())));
                Bytes oracle(head->original);
                LZ4F_dctx* context = nullptr;
                Require(!LZ4F_isError(LZ4F_createDecompressionContext(&context, LZ4F_VERSION)), "decoder created");
                std::size_t src = buffers.raw.size() - 4, dst = oracle.size();
                const auto rc = LZ4F_decompress(context, oracle.data(), &dst, buffers.raw.data() + 4, &src, nullptr);
                LZ4F_freeDecompressionContext(context);
                Require(!LZ4F_isError(rc) && dst == oracle.size(), "oracle decoded");
                if (oracle != buffers.decoded) { ++wrong; std::cerr << "WRONG DECODE: " << a_archives[a].filename().string() << " at " << p.offset << '\n'; }
            }
            std::cout << a_archives[a].filename().string() << ": " << added << " tails so far, " << mib(tailBytes) << " MiB\n" << std::flush;
        }
        store.Drain();
        const auto fillSeconds = seconds(fillTicks);
        const auto allSeconds = seconds(now() - allStarted);

        // Reading them all back, in the order they were written and scattered, with the hash check.
        Bytes out;
        std::int64_t readTicks = 0;
        std::uint64_t readBack = 0;
        std::mt19937_64 random(1);
        for (int round = 0; round < 2; ++round) {
            if (round == 1) std::shuffle(checks.begin(), checks.end(), random);
            for (const auto& check : checks) {
                out.resize(check.what.tailBytes);
                const auto started = now();
                const bool ok = store.Read(check.key, check.what, out.data());
                readTicks += now() - started;
                Require(ok, "tail reads back");
                ++readBack;
            }
        }
        const auto figures = store.Current();
        std::cout << "tails " << added << ", already had " << had << ", not needed " << notNeeded << ", failed " << failed << ", wrong " << wrong << "; excluded " << excluded << ", not textures " << notTexture << '\n'
                  << "store " << figures.entries << " entries, " << mib(figures.bytes) << " MiB, LZ4 would make it " << mib(packedBytes) << " MiB\n"
                  << "filling: " << fillSeconds << " s in the filler, " << allSeconds << " s with the checks\n"
                  << "reading: " << (readBack ? 1e6 * seconds(readTicks) / static_cast<double>(readBack) : 0.0) << " us per tail, average " << (added ? mib(tailBytes) / static_cast<double>(added) : 0.0) << " MiB\n";
        for (const auto handle : handles) CloseHandle(handle);
        return failed || wrong ? 1 : 0;
    }
}

int main(int argc, char** argv) {
    if (argc > 2) {
        std::vector<std::filesystem::path> archives(argv + 2, argv + argc);
        return RealArchives(argv[1], archives);
    }
    Decodes();
    Stores();
    Fills();
    std::cout << "tail cache tests passed\n";
    return 0;
}
