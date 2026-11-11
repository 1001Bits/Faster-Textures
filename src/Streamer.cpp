#include "Streamer.h"

#include "ArchiveTail.h"
#include "DownscalerRules.h"
#include "GpuCompletion.h"
#include "HookPublication.h"
#ifdef FASTER_TEXTURES_BENCH
#include "LoadProfiler.h"
#include "LoadQueueProbe.h"
#endif
#include "MipTail.h"
#include "TailCache.h"

#include "FasterArchives/RuntimePolicy.h"

#include <SimpleIni.h>
#include <d3d11_3.h>
#include <wrl/client.h>

#include <condition_variable>
#include <filesystem>
#include <unordered_set>

namespace {
    using Microsoft::WRL::ComPtr;
    using RE::BSResource::ErrorCode;
    using RE::BSResource::SeekMode;
    using RE::BSResource::Stream;

    enum class Mode { kOff = 0, kMeasure = 1, kStream = 2 };

    struct Settings {
        Mode mode = Mode::kStream;
        std::uint32_t skipMips = 2;
        std::uint32_t minEdge = 512;
        std::uint64_t minSavedBytes = 256u << 10;
        bool cache = true, cacheFill = true;
        std::string cacheFolder;
        bool cacheInLocalAppData = false;
        std::int64_t uploadBytesPerFrame = 16ll << 20;
        std::uint32_t releasesPerFrame = 4;
        std::uint32_t upgradeWorkers = 2;
#ifdef FASTER_TEXTURES_BENCH
        bool profile = false;
        bool profileQueue = false;
        std::uint32_t testWalkSeconds = 0;
        float testWalkSpeed = 800.0f;
#endif
        std::vector<std::string> exclude;
        std::vector<std::string> armorExclude;  // whole beside a mod that copies these textures as they load
    };

    Settings g_settings;
    RuntimePolicy::Layout g_layout{};
    std::uintptr_t g_compressedVtable = 0, g_storedVtable = 0;
    std::atomic<bool> g_streaming{false};
    std::atomic<const ArchiveIndex::Index*> g_index{nullptr};
    std::atomic<const std::vector<ArchiveTail::PathEntry>*> g_excludedPaths{nullptr};
    std::atomic<TailCache::Store*> g_cache{nullptr};
    std::vector<std::uint64_t> g_archiveIds;  // by archive in g_index, set before g_cache
    // The largest archive missing from g_index, and from g_excludedPaths: what it holds is not known. Set before each.
    std::atomic<std::uint64_t> g_unindexedReach{0}, g_unreadPathsReach{0};
    // Path bits that load whole: 1 the exclude list, 2 armor and clothing beside a mod that copies them.
    constexpr std::uint8_t kExcludedBit = 1, kArmorBit = 2;
    std::atomic<std::uint8_t> g_wholeBits{kExcludedBit};
    std::uintptr_t g_gameImageStart = 0, g_gameImageEnd = 0;
    // Set when the game no longer presents through the hooked swap chain: full textures could not arrive.
    std::atomic<bool> g_presentLost{false};

    // ── Timing ───────────────────────────────────────────────────────────────

    std::int64_t Qpc() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

#ifdef FASTER_TEXTURES_BENCH
    std::int64_t QpcFrequency() {
        static const std::int64_t frequency = [] {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return frequency;
    }
#endif

    double QpcSeconds(std::int64_t a_ticks) {
        static const auto frequency = [] {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            return static_cast<double>(value.QuadPart);
        }();
        return static_cast<double>(a_ticks) / frequency;
    }

    double MiB(std::uint64_t a_bytes) { return static_cast<double>(a_bytes) / (1024.0 * 1024.0); }

    // A path for the log. path::string() throws for a character the system's code page lacks, and nothing catches it on these threads.
    std::string Utf8(const std::filesystem::path& a_path) {
        const auto text = a_path.u8string();
        return std::string(text.begin(), text.end());
    }

    // kOther: a stream another mod made. Its own reads could differ from the game's, so it loads whole.
    enum Source { kLoose, kStored, kCompressed, kOther, kSourceCount };

    // By the file's size: a 4K texture with its mips is 11 MiB in BC1 and 21 MiB in BC7.
    constexpr int kSizeCount = 4;
    constexpr std::array<const char*, kSizeCount> kSizeNames{"under 1 MiB", "1-4 MiB", "4-16 MiB", "over 16 MiB"};

    constexpr int SizeOf(std::uint64_t a_fileBytes) {
        return a_fileBytes < (1ull << 20) ? 0 : a_fileBytes < (4ull << 20) ? 1 : a_fileBytes < (16ull << 20) ? 2 : 3;
    }

    struct SizeFigures {
        std::atomic<std::uint64_t> count{0}, bytes{0}, readTicks{0}, createTicks{0};
    };

    struct Totals {
        std::atomic<std::uint64_t> textures{0}, createTicks{0}, readTicks{0}, readBytes{0}, busyTicks{0};
        std::atomic<std::uint64_t> sourceCount[kSourceCount]{}, sourceBytes[kSourceCount]{}, sourceTicks[kSourceCount]{};
        std::atomic<std::uint64_t> smallFromTail{0}, smallFromCache{0}, leftOutBytes{0}, handedToSps{0}, atDownscalerSize{0};
        std::atomic<std::uint64_t> excluded{0}, unnamed{0}, noPlan{0}, wholeFrame{0}, notFound{0}, failed{0}, foreign{0};
        std::atomic<std::uint64_t> possibleBytes{0}, possibleTailBytes{0}, possibleExcludedBytes{0}, possibleUnnamedBytes{0};
        std::atomic<std::uint64_t> renderThreadCount{0}, renderThreadTicks{0}, worstRenderThreadTicks{0};
        std::array<SizeFigures, kSizeCount> whole, small_;

        void Reset() {
            for (auto* value : {&textures, &createTicks, &readTicks, &readBytes, &busyTicks, &smallFromTail, &smallFromCache, &leftOutBytes, &handedToSps, &atDownscalerSize, &excluded, &unnamed, &noPlan,
                                &wholeFrame, &notFound, &failed, &foreign, &possibleBytes, &possibleTailBytes, &possibleExcludedBytes, &possibleUnnamedBytes, &renderThreadCount, &renderThreadTicks, &worstRenderThreadTicks})
                value->store(0, std::memory_order_relaxed);
            for (auto* figures : {&whole, &small_}) {
                for (auto& size : *figures) {
                    size.count.store(0, std::memory_order_relaxed);
                    size.bytes.store(0, std::memory_order_relaxed);
                    size.readTicks.store(0, std::memory_order_relaxed);
                    size.createTicks.store(0, std::memory_order_relaxed);
                }
            }
            for (int i = 0; i < kSourceCount; ++i) {
                sourceCount[i].store(0, std::memory_order_relaxed);
                sourceBytes[i].store(0, std::memory_order_relaxed);
                sourceTicks[i].store(0, std::memory_order_relaxed);
            }
        }
    };

    Totals g_screen, g_session;
    std::atomic<bool> g_loading{false};
    // The MCM's two choices: whether what loads behind a loading screen, and what loads at any other time, loads small first.
    std::atomic<bool> g_smallOnLoadingScreens{true}, g_smallDuringPlay{true};
    std::atomic<std::int64_t> g_loadStarted{0}, g_screenStarted{0}, g_restoreStarted{0};
    std::atomic<std::uint32_t> g_renderThread{0};
    std::atomic<std::uint32_t> g_threadGeneration{1}, g_threadsThisScreen{0};
    std::atomic<std::int32_t> g_inFlight{0};
    std::atomic<std::int64_t> g_busySince{0};

    void Add(std::atomic<std::uint64_t> Totals::* a_field, std::uint64_t a_value) {
        (g_screen.*a_field).fetch_add(a_value, std::memory_order_relaxed);
        (g_session.*a_field).fetch_add(a_value, std::memory_order_relaxed);
    }

    void NoteThread() {
        thread_local std::uint32_t seen = 0;
        const auto generation = g_threadGeneration.load(std::memory_order_relaxed);
        if (seen != generation) {
            seen = generation;
            g_threadsThisScreen.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Time with at least one texture being created, however many at once: the
    // share of a loading screen that shorter texture work could remove.
    struct BusyScope {
        BusyScope() {
            if (g_inFlight.fetch_add(1, std::memory_order_acq_rel) == 0) g_busySince.store(Qpc(), std::memory_order_relaxed);
        }
        ~BusyScope() {
            if (g_inFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) Add(&Totals::busyTicks, static_cast<std::uint64_t>(Qpc() - g_busySince.load(std::memory_order_relaxed)));
        }
    };

    // ── The game's streams ───────────────────────────────────────────────────
    //
    // CommonLib decides these offsets by "is this AE", which is wrong for 1.6.640;
    // Faster Archives keeps the table checked against each runtime.

    std::uint32_t& FlagsOf(const void* a_stream) {
        return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(a_stream) + g_layout.flags);
    }

    std::uint32_t StartOf(const void* a_stream) {
        return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(a_stream) + g_layout.startOffset);
    }

    void AddRef(Stream* a_stream) {
        std::atomic_ref(FlagsOf(a_stream)).fetch_add(0x1000u, std::memory_order_acq_rel);
    }

    void Release(Stream* a_stream) {
        const auto before = std::atomic_ref(FlagsOf(a_stream)).fetch_sub(0x1000u, std::memory_order_acq_rel);
        if (((before - 0x1000u) & 0xFFFFF000u) == 0) {
            const auto destroy = reinterpret_cast<void (*)(void*, std::uint32_t)>((*reinterpret_cast<void***>(a_stream))[0]);
            destroy(a_stream, 1);
        }
    }

    // Opens and closes a stream as the game's BSResourceNiBinaryStream does.
    ErrorCode Open(Stream* a_stream) {
        std::atomic_ref(FlagsOf(a_stream)).fetch_or(0xEu, std::memory_order_acq_rel);
        return a_stream->DoOpen();
    }

    void Close(Stream* a_stream) {
        std::atomic_ref(FlagsOf(a_stream)).fetch_and(0xFFFFFFF1u, std::memory_order_acq_rel);
        a_stream->DoClose();
    }

    // A second stream on the same file, holding one reference, or null.
    Stream* CloneOf(const Stream* a_stream) {
        void* clone = nullptr;
        a_stream->DoClone(*reinterpret_cast<RE::BSTSmartPointer<Stream>*>(&clone));
        return static_cast<Stream*>(clone);
    }

    bool ReadAll(Stream* a_stream, void* a_buffer, std::uint64_t a_bytes) {
        auto* at = static_cast<std::uint8_t*>(a_buffer);
        while (a_bytes > 0) {
            std::uint64_t read = 0;
            if (a_stream->DoRead(at, a_bytes, read) != ErrorCode::kNone || read == 0) return false;
            at += read;
            a_bytes -= read;
        }
        return true;
    }

    // The game's own archive streams, any other stream class of the game's (loose files), or another mod's.
    Source SourceOf(const Stream* a_stream) {
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_stream);
        if (vtable == g_compressedVtable) return kCompressed;
        if (vtable == g_storedVtable) return kStored;
        return vtable >= g_gameImageStart && vtable < g_gameImageEnd ? kLoose : kOther;
    }

    // ── What may load small ──────────────────────────────────────────────────

    enum class Eligible { kYes, kUnnamed, kExcluded };

    // Which lists a path is on, as bits.
    std::uint8_t PathBits(std::string_view a_lowerPath) {
        std::uint8_t bits = 0;
        for (const auto& part : g_settings.exclude)
            if (a_lowerPath.find(part) != std::string_view::npos) {
                bits |= kExcludedBit;
                break;
            }
        for (const auto& part : g_settings.armorExclude)
            if (a_lowerPath.find(part) != std::string_view::npos) {
                bits |= kArmorBit;
                break;
            }
        return bits;
    }

    Eligible CheckPath(const RE::NiSourceTexture* a_texture, const Stream* a_real, Source a_source) {
        const char* name = a_texture->name.c_str();
        if (name && *name) {
            std::array<char, 512> lower;
            std::size_t length = 0;
            for (; name[length] && length < lower.size(); ++length) {
                const char c = name[length] == '/' ? '\\' : name[length];
                lower[length] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
            }
            return (PathBits(std::string_view(lower.data(), length)) & g_wholeBits.load(std::memory_order_acquire)) != 0 ? Eligible::kExcluded : Eligible::kYes;
        }
        // Most textures: the game found them by hash. Only an archive's table knows their path.
        const auto* paths = a_source == kStored || a_source == kCompressed ? g_excludedPaths.load(std::memory_order_acquire) : nullptr;
        if (!paths) return Eligible::kUnnamed;
        switch (ArchiveTail::LookUp(*paths, StartOf(a_real), a_real->totalSize, g_wholeBits.load(std::memory_order_acquire), g_unreadPathsReach.load(std::memory_order_acquire))) {
        case ArchiveTail::PathAnswer::kYes: return Eligible::kExcluded;
        case ArchiveTail::PathAnswer::kNo: return Eligible::kYes;
        default: return Eligible::kUnnamed;
        }
    }

    std::optional<MipTail::Plan> PlanOf(const SkyrimSource::Info& a_file) {
        return MipTail::PlanFor(a_file, g_settings.skipMips, g_settings.minEdge, g_settings.minSavedBytes);
    }

    // ── The stand-in ─────────────────────────────────────────────────────────

    // Which tail of the file now loading whole is worth keeping for the next load.
    struct Capture {
        TailCache::Key key;
        TailCache::What what;
    };

    // Replaces a texture's stream while the game creates the texture. It either
    // serves a smaller DDS from memory, or passes every call on to the real stream
    // and times the reads. Lives on the stack of the creating thread and holds
    // references of its own, so the game's count never reaches zero.
    class StandIn final : public Stream {
    public:
        explicit StandIn(Stream* a_real) : real_(a_real) {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(this) + 0x0C) = 0x10000u;
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(this) + 0x10) = 0x10000u;
        }
        ~StandIn() override = default;

        void Serve(const std::byte* a_header, std::size_t a_headerBytes, const std::uint8_t* a_tail, std::size_t a_tailBytes) {
            header_ = a_header;
            headerBytes_ = a_headerBytes;
            tail_ = a_tail;
            tailBytes_ = a_tailBytes;
            memory_ = true;
        }

        void KeepTail(const Capture& a_capture) { capture_ = &a_capture; }

        [[nodiscard]] std::uint64_t ReadTicks() const { return readTicks_; }
        [[nodiscard]] std::uint64_t ReadBytes() const { return readBytes_; }
        [[nodiscard]] const std::optional<SkyrimSource::Info>& File() const { return file_; }
        [[nodiscard]] const std::optional<MipTail::Plan>& Planned() const { return plan_; }

        ErrorCode DoOpen() override {
            if (memory_) {
                totalSize = static_cast<std::uint32_t>(headerBytes_ + tailBytes_);
                position_ = 0;
                return ErrorCode::kNone;
            }
            const auto result = Open(real_);
            totalSize = real_->totalSize;
            return result;
        }

        void DoClose() override {
            if (!memory_) Close(real_);
        }

        void DoClone(RE::BSTSmartPointer<Stream>& a_out) const override { real_->DoClone(a_out); }

        ErrorCode DoRead(void* a_buffer, std::uint64_t a_toRead, std::uint64_t& a_read) const override {
            if (memory_) {
                a_read = 0;
                auto* out = static_cast<std::uint8_t*>(a_buffer);
                const auto take = [&](const void* a_from, std::uint64_t a_start, std::uint64_t a_bytes) {
                    if (position_ < a_start || position_ >= a_start + a_bytes || a_read == a_toRead) return;
                    const auto n = std::min(a_toRead - a_read, a_start + a_bytes - position_);
                    std::memcpy(out + a_read, static_cast<const std::uint8_t*>(a_from) + (position_ - a_start), static_cast<std::size_t>(n));
                    a_read += n;
                    position_ += n;
                };
                take(header_, 0, headerBytes_);
                take(tail_, headerBytes_, tailBytes_);
                return ErrorCode::kNone;
            }
            const auto started = Qpc();
            const auto result = real_->DoRead(a_buffer, a_toRead, a_read);
            readTicks_ += static_cast<std::uint64_t>(Qpc() - started);
            if (result != ErrorCode::kNone) return result;
            readBytes_ += a_read;
            // The game's loader reads the whole file in its first read.
            if (position_ == 0 && a_read == totalSize && a_read >= MipTail::kHeaderMax) OnWholeFile(static_cast<std::byte*>(a_buffer), a_read);
            position_ += a_read;
            return result;
        }

        ErrorCode DoWrite(const void*, std::uint64_t, std::uint64_t&) const override { return ErrorCode::kUnsupported; }

        ErrorCode DoSeek(std::uint64_t a_toSeek, SeekMode a_mode, std::uint64_t& a_sought) const override {
            if (!memory_) return real_->DoSeek(a_toSeek, a_mode, a_sought);
            const std::uint64_t total = headerBytes_ + tailBytes_;
            switch (a_mode) {
            case SeekMode::kSet: position_ = a_toSeek; break;
            case SeekMode::kCur: position_ += a_toSeek; break;
            case SeekMode::kEnd: position_ = total + a_toSeek; break;
            }
            position_ = std::min(position_, total);
            a_sought = position_;
            return ErrorCode::kNone;
        }

        bool DoGetName(RE::BSFixedString& a_dst) const override { return real_->DoGetName(a_dst); }

    private:
        // Plans for the file just read. With a cut, moves the kept mips up against
        // headers rewritten for them: the file was read whole anyway, this saves
        // only the upload. The reported size stays, as the loader insists on it;
        // the bytes past the kept mips are ignored.
        void OnWholeFile(std::byte* a_data, std::uint64_t a_bytes) const {
            if (capture_ && std::uint64_t{capture_->what.tailOffset} + capture_->what.tailBytes <= a_bytes) {
                if (auto* cache = g_cache.load(std::memory_order_acquire)) cache->Add(capture_->key, capture_->what, reinterpret_cast<const std::uint8_t*>(a_data) + capture_->what.tailOffset);
            }
            file_ = SkyrimSource::Parse(std::span<const std::byte>(a_data, MipTail::kHeaderMax), a_bytes);
            if (!file_) return;
            plan_ = PlanOf(*file_);
        }

        Stream* real_;
        const std::byte* header_ = nullptr;
        const std::uint8_t* tail_ = nullptr;
        std::size_t headerBytes_ = 0, tailBytes_ = 0;
        const Capture* capture_ = nullptr;
        bool memory_ = false;
        mutable std::uint64_t position_ = 0, readTicks_ = 0, readBytes_ = 0;
        mutable std::optional<SkyrimSource::Info> file_;
        mutable std::optional<MipTail::Plan> plan_;
    };

    // ── Reading the small mips ───────────────────────────────────────────────

    struct SmallLoad {
        SkyrimSource::Info file;
        MipTail::Plan plan;
        std::uint64_t fileBytes{};
        bool fromCache = false;
    };

    struct LoadBuffers {
        std::array<std::byte, MipTail::kHeaderMax> header;
        std::vector<std::uint8_t> tail;
    };

    // From an archive entry: from its end when its blocks can be reached from there,
    // otherwise from the cache, or else whole, keeping the small mips for next time.
    std::optional<SmallLoad> FromCompressed(const Stream* a_real, LoadBuffers& a_out, std::atomic<std::uint64_t> Totals::*& a_why, std::optional<Capture>& a_capture) {
        const auto* index = g_index.load(std::memory_order_acquire);
        const auto entry = index ? ArchiveTail::Locate(*index, StartOf(a_real), a_real->totalSize, g_unindexedReach.load(std::memory_order_acquire)) : std::nullopt;
        const auto head = entry ? ArchiveTail::ReadHead(*entry) : std::nullopt;
        if (!head) {
            a_why = &Totals::notFound;
            return std::nullopt;
        }
        const auto file = SkyrimSource::Parse(head->dds, head->original);
        const auto plan = file ? PlanOf(*file) : std::nullopt;
        if (!plan) {
            a_why = &Totals::noPlan;
            return std::nullopt;
        }
        if (!MipTail::CanSkip(head->frame, plan->tailOffset)) {
            auto* cache = g_cache.load(std::memory_order_acquire);
            if (const auto archive = cache ? g_archiveIds[entry->archive] : 0; archive != 0) {
                const TailCache::Key key{archive, entry->offset};
                const auto what = TailCache::WhatOf(*head, *plan);
                a_out.tail.resize(what.tailBytes);
                if (cache->Read(key, what, a_out.tail.data())) {
                    a_out.header = head->dds;
                    return SmallLoad{*file, *plan, head->original, true};
                }
                a_capture = Capture{key, what};
            }
            a_why = &Totals::wholeFrame;
            return std::nullopt;
        }
        a_out.tail.resize(static_cast<std::size_t>(plan->tailBytes));
        if (!ArchiveTail::ReadTail(*entry, *head, plan->tailOffset, plan->tailOffset + plan->tailBytes, a_out.tail.data())) {
            a_why = &Totals::failed;
            return std::nullopt;
        }
        a_out.header = head->dds;
        return SmallLoad{*file, *plan, head->original, false};
    }

    // From a loose file or a stored archive entry: seek past the top mips. On a
    // second stream, because the game opens the real one itself.
    std::optional<SmallLoad> FromFile(const Stream* a_real, LoadBuffers& a_out, std::atomic<std::uint64_t> Totals::*& a_why) {
        auto* clone = CloneOf(a_real);
        if (!clone) {
            a_why = &Totals::failed;
            return std::nullopt;
        }
        std::optional<SmallLoad> result;
        a_why = &Totals::failed;
        if (Open(clone) == ErrorCode::kNone && clone->totalSize >= MipTail::kHeaderMax && ReadAll(clone, a_out.header.data(), a_out.header.size())) {
            const auto file = SkyrimSource::Parse(a_out.header, clone->totalSize);
            const auto plan = file ? PlanOf(*file) : std::nullopt;
            if (!plan || plan->tailOffset < a_out.header.size()) {
                a_why = &Totals::noPlan;
            } else {
                std::uint64_t sought = 0;
                a_out.tail.resize(static_cast<std::size_t>(plan->tailBytes));
                if (clone->DoSeek(plan->tailOffset - a_out.header.size(), SeekMode::kCur, sought) == ErrorCode::kNone && ReadAll(clone, a_out.tail.data(), plan->tailBytes))
                    result = SmallLoad{*file, *plan, clone->totalSize, false};
            }
        }
        Close(clone);
        Release(clone);
        return result;
    }

    // ── Textures whose full version is still to come ─────────────────────────

    struct Record {
        RE::NiSourceTexture* owner = nullptr;
        Stream* stream = nullptr;  // holds a reference
        ID3D11Texture2D* expectedTexture = nullptr;
        ID3D11ShaderResourceView* expectedView = nullptr;
        SkyrimSource::Info file;
        std::uint64_t fileBytes = 0;
        std::uint32_t downscalerSkip = 0;  // the largest levels Texture Downscaler leaves out: the full version arrives without them
        bool queued = true, drawn = false, dead = false;
    };

    using RecordPtr = std::shared_ptr<Record>;

    std::mutex g_lock;
    std::condition_variable g_wake;
    std::unordered_map<const RE::NiSourceTexture*, RecordPtr> g_records;
    std::deque<RecordPtr> g_waiting, g_drawnFirst;
    std::atomic<std::uint32_t> g_pending{0};

    // Frame times between loading screens: what full textures arriving cost, if
    // anything. Most of them arrive in the first seconds, so those count apart.
    struct FrameFigures {
        std::atomic<std::uint64_t> frames{0}, ticks{0}, worstTicks{0}, over33{0}, over50{0}, over100{0};

        void Reset() {
            for (auto* value : {&frames, &ticks, &worstTicks, &over33, &over50, &over100}) value->store(0, std::memory_order_relaxed);
        }

        void Count(std::uint64_t a_ticks, double a_seconds) {
            frames.fetch_add(1, std::memory_order_relaxed);
            ticks.fetch_add(a_ticks, std::memory_order_relaxed);
            if (a_ticks > worstTicks.load(std::memory_order_relaxed)) worstTicks.store(a_ticks, std::memory_order_relaxed);
            if (a_seconds > 0.033) over33.fetch_add(1, std::memory_order_relaxed);
            if (a_seconds > 0.050) over50.fetch_add(1, std::memory_order_relaxed);
            if (a_seconds > 0.100) over100.fetch_add(1, std::memory_order_relaxed);
        }
    };

    constexpr double kEarlyFrameSeconds = 5.0;
    FrameFigures g_framesEarly, g_framesLater;
#ifdef FASTER_TEXTURES_BENCH
    FrameFigures g_gpuEarly, g_gpuLater;  // the GPU's time from one frame's end to the next, by timestamp
    std::atomic<std::uint64_t> g_presentOk{0}, g_presentOccluded{0}, g_presentOther{0}, g_gpuLost{0};
    std::atomic<std::uint32_t> g_presentOtherCode{0};

    ID3D11Device* Device();
    ID3D11DeviceContext* Context();

    // Render thread only. A frame's queries are read back eight frames later, never waited for.
    struct GpuStamp {
        ComPtr<ID3D11Query> disjoint, stamp;
        std::int64_t sinceLoad = -1;  // ticks after the loading screen, or -1 outside the window
        std::uint64_t frame = 0;
        bool issued = false;
    };
    std::array<GpuStamp, 8> g_gpuStamps;
    std::uint64_t g_gpuFrame = 0, g_gpuPrevFrame = 0, g_gpuPrevStamp = 0;
    bool g_gpuStampsReady = false, g_gpuStampsFailed = false;

    void ReadGpuStamp(ID3D11DeviceContext* a_context, GpuStamp& a_slot) {
        if (!a_slot.issued) return;
        a_slot.issued = false;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 stamp = 0;
        if (a_context->GetData(a_slot.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            a_context->GetData(a_slot.stamp.Get(), &stamp, sizeof(stamp), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || disjoint.Disjoint || disjoint.Frequency == 0) {
            g_gpuLost.fetch_add(1, std::memory_order_relaxed);
            g_gpuPrevFrame = 0;
            return;
        }
        if (g_gpuPrevFrame != 0 && a_slot.frame == g_gpuPrevFrame + 1 && stamp > g_gpuPrevStamp && a_slot.sinceLoad >= 0) {
            const double seconds = static_cast<double>(stamp - g_gpuPrevStamp) / static_cast<double>(disjoint.Frequency);
            const auto ticks = static_cast<std::uint64_t>(seconds * static_cast<double>(QpcFrequency()));
            (QpcSeconds(a_slot.sinceLoad) < kEarlyFrameSeconds ? g_gpuEarly : g_gpuLater).Count(ticks, seconds);
        }
        g_gpuPrevFrame = a_slot.frame;
        g_gpuPrevStamp = stamp;
    }

    // At the end of each frame's commands, just before Present.
    void StampGpuFrame(std::int64_t a_sinceLoad) {
        if (g_gpuStampsFailed) return;
        auto* context = Context();
        if (!context) return;
        if (!g_gpuStampsReady) {
            for (auto& slot : g_gpuStamps) {
                D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
                if (FAILED(Device()->CreateQuery(&disjoint, slot.disjoint.GetAddressOf())) || FAILED(Device()->CreateQuery(&stamp, slot.stamp.GetAddressOf()))) {
                    g_gpuStampsFailed = true;
                    SKSE::log::warn("[frames] GPU timestamps are not available; only the time between Present calls is logged");
                    return;
                }
            }
            g_gpuStampsReady = true;
        }
        auto& slot = g_gpuStamps[++g_gpuFrame % g_gpuStamps.size()];
        ReadGpuStamp(context, slot);
        context->Begin(slot.disjoint.Get());
        context->End(slot.stamp.Get());
        context->End(slot.disjoint.Get());
        slot.issued = true;
        slot.frame = g_gpuFrame;
        slot.sinceLoad = a_sinceLoad;
    }
#endif
    std::atomic<std::int64_t> g_lastPresent{0}, g_framesFrom{0};
    std::atomic<std::uint64_t> g_swaps{0}, g_swapTicks{0}, g_worstSwapTicks{0};
    std::atomic<std::uint64_t> g_releases{0}, g_releaseTicks{0}, g_worstReleaseTicks{0};
    std::atomic<std::uint64_t> g_creations{0}, g_createdBytes{0}, g_worstCreationTicks{0};

    // What the frame before a hitch held, for its log line.
    constexpr std::uint32_t kHitchLinesPerScreen = 8;
    enum class WorkerDoing : std::uint32_t { kNothing, kReading, kWaitingForRoom, kCreating };
    std::atomic<WorkerDoing> g_workerDoing{WorkerDoing::kNothing};
    std::atomic<std::uint64_t> g_frameCreations{0}, g_frameCreatedBytes{0}, g_frameLongestCreation{0};
    std::atomic<std::int64_t> g_lastHookTicks{0}, g_lastPresentCallTicks{0};
    std::atomic<std::uint32_t> g_lastFrameSwaps{0}, g_lastFrameReleases{0}, g_hitchLines{0};

    // Full textures' uploads, in bytes the next one may start with. Refilled every
    // frame and allowed to go below zero: a large texture is paid for by the frames after it.
    std::atomic<std::int64_t> g_uploadCredit{0};

    // The frames right after a loading screen have the least room, and a frame that
    // ran late means the graphics card is behind: sending it more makes the next one
    // late too. Relative to the game's usual frame, so a game that always runs at 30
    // still gets its textures, and held only so long, so they always arrive.
    constexpr double kUploadRampSeconds = 1.0;
    constexpr std::uint32_t kMostFramesHeld = 5;
    std::atomic<std::int64_t> g_typicalFrameTicks{0};
    std::atomic<std::uint32_t> g_framesHeld{0}, g_framesHeldThisScreen{0};

    void RefillUploadCredit(std::int64_t a_frameTicks, std::int64_t a_sinceLoad) {
        const auto perFrame = g_settings.uploadBytesPerFrame;
        if (perFrame <= 0) return;

        auto typical = g_typicalFrameTicks.load(std::memory_order_relaxed);
        const bool late = typical != 0 && a_frameTicks > typical + typical / 2 && QpcSeconds(a_frameTicks) > 0.020;
        typical = typical == 0 ? a_frameTicks : typical + (std::min(a_frameTicks, typical * 2) - typical) / 16;
        g_typicalFrameTicks.store(typical, std::memory_order_relaxed);

        const auto held = g_framesHeld.load(std::memory_order_relaxed);
        if (late && held < kMostFramesHeld) {
            g_framesHeld.store(held + 1, std::memory_order_relaxed);
            g_framesHeldThisScreen.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_framesHeld.store(0, std::memory_order_relaxed);

        auto budget = perFrame;
        if (a_sinceLoad > 0) {
            const auto seconds = QpcSeconds(a_sinceLoad);
            if (seconds < kUploadRampSeconds) budget = std::max(perFrame / 8, static_cast<std::int64_t>(static_cast<double>(perFrame) * seconds / kUploadRampSeconds));
        }
        const auto credit = g_uploadCredit.fetch_add(budget, std::memory_order_acq_rel) + budget;
        if (credit > budget) g_uploadCredit.fetch_sub(credit - budget, std::memory_order_acq_rel);
    }

    void WaitForUploadRoom(std::uint64_t a_bytes) {
        if (g_settings.uploadBytesPerFrame <= 0) return;
        g_workerDoing.store(WorkerDoing::kWaitingForRoom, std::memory_order_relaxed);
        while (g_uploadCredit.load(std::memory_order_acquire) <= 0) {
            // No frames, so nothing to protect and nothing to refill it.
            if (QpcSeconds(Qpc() - g_lastPresent.load(std::memory_order_relaxed)) > 0.1) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        g_uploadCredit.fetch_sub(static_cast<std::int64_t>(a_bytes), std::memory_order_acq_rel);
    }

    struct Ready {
        RecordPtr record;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11Query> completion;  // for the texture it replaces: made by the worker, so the swap creates nothing
    };
    std::deque<Ready> g_ready;
    std::atomic<std::uint32_t> g_activeWorkers{0};
    std::atomic<std::uint64_t> g_upgradeReadBytes{0}, g_upgradeReadTicks{0}, g_upgradeCreateTicks{0};
    std::mutex g_creditLock;

    void FinishLocked(const RecordPtr& a_record) {
        if (a_record->stream) {
            Release(a_record->stream);
            a_record->stream = nullptr;
        }
        if (g_records.erase(a_record->owner) != 0) g_pending.fetch_sub(1, std::memory_order_relaxed);
    }

    // ── Texture Downscaler ───────────────────────────────────────────────────
    //
    // It cuts every texture the game creates down to a size it reads from its ini, and so would the full versions this plugin
    // creates, but without a name to go on on that thread it would cut them all to its Diffuse limit, folders it keeps at full
    // size included. So the size it gives each texture is worked out here from the same rules, the full version is created at
    // that size past its hook, and a texture whose small version is already that small is final. Its own hook still cuts the
    // small versions: it wraps this plugin's, installed later, and names the texture from the real stream.

    bool g_downscalerInstalled = false;
    std::atomic<std::shared_ptr<const DownscalerRules::Rules>> g_downscaler;  // null when it is not installed or switched off

    // Its ini again: its menu writes there, and a change applies to textures loaded afterwards.
    void ReadDownscalerRules() {
        if (!g_downscalerInstalled) return;
        auto rules = std::make_shared<DownscalerRules::Rules>();
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile("Data/SKSE/Plugins/TextureDownscaler.ini") >= 0) {
            if (!ini.GetBoolValue("General", "Enabled", true)) {
                if (g_downscaler.exchange(nullptr)) SKSE::log::info("[stream] Texture Downscaler is switched off: full textures arrive at full size");
                return;
            }
            for (std::size_t i = 0; i < DownscalerRules::kKinds; ++i)
                rules->maxSize[i] = DownscalerRules::Clamp(ini.GetLongValue("Textures", DownscalerRules::kKindNames[i].data(), DownscalerRules::kDefaultLimit));
            CSimpleIniA::TNamesDepend keys;
            if (ini.GetAllKeys("Folders", keys)) {
                keys.sort(CSimpleIniA::Entry::LoadOrder());
                for (const auto& key : keys) DownscalerRules::AddFolder(*rules, key.pItem, ini.GetLongValue("Folders", key.pItem, 0));
            }
        }
        const bool first = !g_downscaler.load(std::memory_order_acquire);
        if (first)
            SKSE::log::info("[stream] Texture Downscaler is installed: full textures arrive at its sizes (Diffuse {}, Normal {}, Parallax {}, Material {}, Glow {}, Mask {}, {} folder rules)",
                            rules->maxSize[0], rules->maxSize[1], rules->maxSize[2], rules->maxSize[3], rules->maxSize[4], rules->maxSize[5], rules->folders.size());
        g_downscaler.store(std::move(rules), std::memory_order_release);
    }

    // The name it goes by: the stream's own, or else the texture's. Many come through without either, and it then uses the
    // Diffuse limit; so does this.
    std::string DownscalerName(const RE::NiSourceTexture* a_texture, const Stream* a_real) {
        RE::BSFixedString name;
        a_real->DoGetName(name);
        std::string_view text = name.c_str() ? name.c_str() : "";
        if (text.empty() && a_texture->name.c_str()) text = a_texture->name.c_str();
        return DownscalerRules::Fold(text);
    }

    void Track(RE::NiSourceTexture* a_texture, Stream* a_stream, const SkyrimSource::Info& a_file, std::uint64_t a_fileBytes, std::uint32_t a_downscalerSkip = 0) {
        auto record = std::make_shared<Record>();
        record->owner = a_texture;
        record->stream = a_stream;
        AddRef(a_stream);
        record->expectedTexture = reinterpret_cast<ID3D11Texture2D*>(a_texture->rendererTexture->texture);
        record->expectedView = reinterpret_cast<ID3D11ShaderResourceView*>(a_texture->rendererTexture->resourceView);
        record->file = a_file;
        record->fileBytes = a_fileBytes;
        record->downscalerSkip = a_downscalerSkip;
        {
            std::scoped_lock lock(g_lock);
            if (const auto old = g_records.find(a_texture); old != g_records.end()) {
                // Created again before its full version arrived: the older request is void.
                old->second->dead = true;
                FinishLocked(old->second);
            }
            g_records.emplace(a_texture, record);
            g_waiting.push_back(std::move(record));
            g_pending.fetch_add(1, std::memory_order_relaxed);
        }
        g_wake.notify_one();
    }

    // ── Sharing textures with Skyrim Performance Solution ────────────────────
    //
    // SPS keeps each texture's size during play. Upgrading what it could manage would
    // fight its VRAM limits, and a texture without its tag it ignores for good. So a
    // texture it can manage is handed over: SPS's own tag says the file is larger and
    // SPS reduced it, and SPS brings it back by need and free VRAM. What it cannot see
    // (no name, or on its exclude list) stays with this plugin.
    //
    // The tag is SPS's private format (TextureStream.cpp in SPS 1.0.71) and SPS updates
    // often. If SPS has not replaced a single handed texture after two minutes of play,
    // it no longer reads the tag: they all go to the worker, so nothing stays small.

    constexpr GUID kSpsTagGuid{0x5b0c2e71, 0x9a4d, 0x4c1e, {0x8f, 0x2a, 0x53, 0x50, 0x53, 0x54, 0x61, 0x67}};
    constexpr std::uint32_t kSpsTagMagic = 0x53505354, kSpsTagFile = 1, kSpsTagReduced = 2;
    struct SpsTag {
        std::uint32_t magic, flags, fullWidth, fullHeight, fullMips, outsideCap;
    };
    static_assert(sizeof(SpsTag) == 24);

    enum class Sharing { kNone, kProbing, kConfirmed, kFailed };
    std::atomic<Sharing> g_sharing{Sharing::kNone};
    std::vector<std::string> g_spsExclude;
    std::unordered_map<const RE::NiSourceTexture*, RecordPtr> g_handed;  // while probing, under g_lock
    std::atomic<std::uint32_t> g_handedCount{0};
    std::atomic<std::int64_t> g_probingSince{0};

    // What became of the handed textures, for the log. SPS tags every texture it swaps in.
    struct HandedWatch {
        const ID3D11Texture2D* loaded;
        std::uint32_t fullWidth;
    };
    std::unordered_map<const RE::NiSourceTexture*, HandedWatch> g_handedWatch;  // under g_lock
    std::atomic<std::uint32_t> g_handedWatchCount{0};

    void LogHanded() {
        std::scoped_lock lock(g_lock);
        if (g_handedWatch.empty()) return;
        std::uint32_t asLoaded = 0, bySps = 0, atFull = 0, other = 0;
        for (const auto& [texture, watch] : g_handedWatch) {
            const auto* renderer = texture->rendererTexture;
            auto* current = renderer ? reinterpret_cast<ID3D11Texture2D*>(renderer->texture) : nullptr;
            if (current == watch.loaded) {
                ++asLoaded;
                continue;
            }
            SpsTag tag{};
            UINT size = sizeof(tag);
            if (!current || FAILED(current->GetPrivateData(kSpsTagGuid, &size, &tag)) || size != sizeof(tag) || tag.magic != kSpsTagMagic) {
                ++other;
                continue;
            }
            ++bySps;
            D3D11_TEXTURE2D_DESC desc{};
            current->GetDesc(&desc);
            if (desc.Width >= watch.fullWidth) ++atFull;
        }
        SKSE::log::info("[stream] of {} textures handed to Skyrim Performance Solution and still loaded: {} as loaded, {} replaced by it ({} at full size), {} changed by something else", g_handedWatch.size(), asLoaded, bySps, atFull, other);
    }

    bool SpsCanManage(const RE::NiSourceTexture* a_texture) {
        const char* name = a_texture->name.c_str();
        if (!name || !*name) return false;
        std::string path(name);
        for (auto& c : path) c = c == '/' ? '\\' : c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        if (path.starts_with("data\\")) path.erase(0, 5);
        if (!path.starts_with("textures\\") || !path.ends_with(".dds")) return false;
        return std::ranges::none_of(g_spsExclude, [&](const std::string& a_part) { return path.find(a_part) != std::string::npos; });
    }

    void HandToSps(RE::NiSourceTexture* a_texture, Stream* a_stream, const SkyrimSource::Info& a_file, std::uint64_t a_fileBytes) {
        auto* resource = reinterpret_cast<ID3D11Resource*>(a_texture->rendererTexture->texture);
        const SpsTag tag{kSpsTagMagic, kSpsTagFile | kSpsTagReduced, a_file.width, a_file.height, a_file.mips, 0};
        resource->SetPrivateData(kSpsTagGuid, sizeof(tag), &tag);
        Add(&Totals::handedToSps, 1);
        {
            std::scoped_lock lock(g_lock);
            g_handedWatch.insert_or_assign(a_texture, HandedWatch{reinterpret_cast<ID3D11Texture2D*>(resource), a_file.width});
            g_handedWatchCount.store(static_cast<std::uint32_t>(g_handedWatch.size()), std::memory_order_release);
        }
        if (g_sharing.load(std::memory_order_acquire) != Sharing::kProbing) return;

        auto record = std::make_shared<Record>();
        record->owner = a_texture;
        record->stream = a_stream;
        AddRef(a_stream);
        record->expectedTexture = reinterpret_cast<ID3D11Texture2D*>(a_texture->rendererTexture->texture);
        record->expectedView = reinterpret_cast<ID3D11ShaderResourceView*>(a_texture->rendererTexture->resourceView);
        record->file = a_file;
        record->fileBytes = a_fileBytes;
        record->queued = false;
        std::scoped_lock lock(g_lock);
        if (const auto old = g_handed.find(a_texture); old != g_handed.end()) {
            Release(old->second->stream);
            g_handed.erase(old);
        }
        g_handed.emplace(a_texture, std::move(record));
        g_handedCount.store(static_cast<std::uint32_t>(g_handed.size()), std::memory_order_release);
    }

    // Called now and then by the worker while it is not known whether SPS reads the tag.
    void CheckSharing() {
        if (g_sharing.load(std::memory_order_acquire) != Sharing::kProbing || g_handedCount.load(std::memory_order_acquire) == 0) return;
        static auto lastCheck = Qpc();
        const auto now = Qpc();
        if (QpcSeconds(now - lastCheck) < 1.0) return;
        lastCheck = now;

        std::scoped_lock lock(g_lock);
        const bool taken = std::ranges::any_of(g_handed, [](const auto& a_entry) {
            const auto* current = a_entry.second->owner->rendererTexture;
            return current && reinterpret_cast<ID3D11Texture2D*>(current->texture) != a_entry.second->expectedTexture;
        });
        if (taken) {
            for (auto& [texture, record] : g_handed) Release(record->stream);
            g_handed.clear();
            g_handedCount.store(0, std::memory_order_release);
            g_sharing.store(Sharing::kConfirmed, std::memory_order_release);
            SKSE::log::info("[stream] Skyrim Performance Solution is managing the textures handed to it");
            return;
        }
        const auto since = g_probingSince.load(std::memory_order_acquire);
        if (g_loading.load(std::memory_order_acquire) || since == 0 || QpcSeconds(now - since) < 120.0) return;

        SKSE::log::warn("[stream] Skyrim Performance Solution took over none of the {} textures handed to it in two minutes, so it no longer reads the tag; upgrading them here", g_handed.size());
        for (auto& [texture, record] : g_handed) {
            record->queued = true;
            g_records.insert_or_assign(texture, record);
            g_waiting.push_back(record);
            g_pending.fetch_add(1, std::memory_order_relaxed);
        }
        g_handed.clear();
        g_handedCount.store(0, std::memory_order_release);
        g_sharing.store(Sharing::kFailed, std::memory_order_release);
        g_wake.notify_one();
    }

    // ── Creating textures ────────────────────────────────────────────────────

    using CreateTexture_t = void (*)(void*, RE::NiSourceTexture*);
    std::atomic<CreateTexture_t> g_originalCreate{nullptr};

    void Hook_CreateTexture(void* a_manager, RE::NiSourceTexture* a_texture) {
        const auto original = g_originalCreate.load(std::memory_order_acquire);
        auto* real = a_texture ? a_texture->resourceStream : nullptr;
        if (!real) return original(a_manager, a_texture);

        const BusyScope busy;
#ifdef FASTER_TEXTURES_BENCH
        const LoadProfiler::TextureReadScope profTex;
#endif
        NoteThread();
        const auto started = Qpc();
        const auto source = SourceOf(real);
        const auto eligible = CheckPath(a_texture, real, source);
        const bool stream = g_streaming.load(std::memory_order_acquire) && !g_presentLost.load(std::memory_order_acquire) &&
                            (g_loading.load(std::memory_order_acquire) ? g_smallOnLoadingScreens : g_smallDuringPlay).load(std::memory_order_relaxed);

        thread_local LoadBuffers buffers;
        std::optional<SmallLoad> reduced;
        std::atomic<std::uint64_t> Totals::* why = nullptr;
        std::optional<Capture> capture;
        std::uint64_t smallTicks = 0;
        if (source == kOther) why = &Totals::foreign;
        else if (eligible == Eligible::kExcluded) why = &Totals::excluded;
        else if (eligible == Eligible::kUnnamed) why = &Totals::unnamed;
        else if (stream) {
            // Nothing may unwind into the game's loader: out of memory here only means this texture loads whole.
            try {
                reduced = source == kCompressed ? FromCompressed(real, buffers, why, capture) : FromFile(real, buffers, why);
            } catch (const std::exception&) {
                reduced.reset();
                why = &Totals::failed;
            }
            smallTicks = static_cast<std::uint64_t>(Qpc() - started);
        }

        StandIn standIn(real);
        if (capture) standIn.KeepTail(*capture);
        if (reduced) {
            MipTail::RewriteHeader(buffers.header, reduced->file, reduced->plan);
            standIn.Serve(buffers.header.data(), reduced->file.offset, buffers.tail.data(), buffers.tail.size());
        }

        a_texture->resourceStream = &standIn;
        original(a_manager, a_texture);
        a_texture->resourceStream = real;

        bool loadedSmall = reduced.has_value();
        if (loadedSmall && !(a_texture->rendererTexture && a_texture->rendererTexture->texture)) {
            // The game could not make a texture of what it was shown: give it the file.
            Add(&Totals::failed, 1);
            loadedSmall = false;
            original(a_manager, a_texture);
        }

        const auto elapsed = static_cast<std::uint64_t>(Qpc() - started);
        const auto readTicks = standIn.ReadTicks() + smallTicks;
        Add(&Totals::textures, 1);
        Add(&Totals::createTicks, elapsed);
        Add(&Totals::readTicks, readTicks);
        Add(&Totals::readBytes, reduced ? reduced->plan.tailBytes : standIn.ReadBytes());
        if (GetCurrentThreadId() == g_renderThread.load(std::memory_order_relaxed)) {
            Add(&Totals::renderThreadCount, 1);
            Add(&Totals::renderThreadTicks, elapsed);
            for (auto* totals : {&g_screen, &g_session}) {
                auto worst = totals->worstRenderThreadTicks.load(std::memory_order_relaxed);
                while (elapsed > worst && !totals->worstRenderThreadTicks.compare_exchange_weak(worst, elapsed, std::memory_order_relaxed)) {}
            }
        }

        const auto fileBytes = reduced ? reduced->fileBytes : standIn.ReadBytes();
        const auto addSource = [&](std::atomic<std::uint64_t>(Totals::*a_field)[kSourceCount], std::uint64_t a_value) {
            (g_screen.*a_field)[source].fetch_add(a_value, std::memory_order_relaxed);
            (g_session.*a_field)[source].fetch_add(a_value, std::memory_order_relaxed);
        };
        addSource(&Totals::sourceCount, 1);
        addSource(&Totals::sourceBytes, fileBytes);
        addSource(&Totals::sourceTicks, readTicks);
        for (auto* totals : {&g_screen, &g_session}) {
            auto& figures = (loadedSmall ? totals->small_ : totals->whole)[SizeOf(fileBytes)];
            figures.count.fetch_add(1, std::memory_order_relaxed);
            figures.bytes.fetch_add(fileBytes, std::memory_order_relaxed);
            figures.readTicks.fetch_add(readTicks, std::memory_order_relaxed);
            figures.createTicks.fetch_add(elapsed > readTicks ? elapsed - readTicks : 0, std::memory_order_relaxed);
        }

        if (loadedSmall) {
            const auto& file = reduced->file;
            const auto& plan = reduced->plan;
            Add(reduced->fromCache ? &Totals::smallFromCache : &Totals::smallFromTail, 1);
            Add(&Totals::leftOutBytes, plan.skippedBytes);
            try {
                std::uint32_t downscalerSkip = 0;
                if (const auto rules = g_downscaler.load(std::memory_order_acquire)) {
                    const auto name = DownscalerName(a_texture, real);
                    downscalerSkip = DownscalerRules::Skip(DownscalerRules::LimitFor(*rules, name), file.width, file.height, file.mips, DownscalerRules::BlockCompressed(file.format));
                    // What the game made of the small version, after Texture Downscaler cut it too: when the full texture at its
                    // size is no larger, there is nothing more to fetch.
                    D3D11_TEXTURE2D_DESC made{};
                    reinterpret_cast<ID3D11Texture2D*>(a_texture->rendererTexture->texture)->GetDesc(&made);
                    if ((file.width >> downscalerSkip) <= made.Width && (file.height >> downscalerSkip) <= made.Height) {
                        Add(&Totals::atDownscalerSize, 1);
                        return;
                    }
                }
                const auto sharing = g_sharing.load(std::memory_order_acquire);
                if ((sharing == Sharing::kProbing || sharing == Sharing::kConfirmed) && SpsCanManage(a_texture)) HandToSps(a_texture, real, file, fileBytes);
                else Track(a_texture, real, file, fileBytes, downscalerSkip);
            } catch (const std::exception&) {
                Add(&Totals::failed, 1);  // stays small, nothing worse
            }
            return;
        }
        if (why) {
            Add(why, 1);
        } else if (!stream && source != kOther && standIn.File() && !standIn.Planned() && eligible == Eligible::kYes) {
            Add(&Totals::noPlan, 1);
        }
        if (source == kOther) {
            // Not this plugin's to read.
        } else if (!stream && standIn.Planned() && eligible == Eligible::kExcluded) {
            Add(&Totals::possibleExcludedBytes, standIn.Planned()->skippedBytes);
        } else if (!stream && standIn.Planned() && eligible == Eligible::kUnnamed) {
            Add(&Totals::possibleUnnamedBytes, standIn.Planned()->skippedBytes);
        } else if (!stream && standIn.Planned()) {
            // What streaming could have left out, and what of it needs no unpacking.
            Add(&Totals::possibleBytes, standIn.Planned()->skippedBytes);
            bool tail = source != kCompressed;
            if (!tail) {
                if (const auto* index = g_index.load(std::memory_order_acquire)) {
                    if (const auto entry = ArchiveTail::Locate(*index, StartOf(real), real->totalSize, g_unindexedReach.load(std::memory_order_acquire))) {
                        if (const auto head = ArchiveTail::ReadHead(*entry)) tail = MipTail::CanSkip(head->frame, standIn.Planned()->tailOffset);
                    }
                }
            }
            if (tail) Add(&Totals::possibleTailBytes, standIn.Planned()->skippedBytes);
        }
    }

    // ── Lifetime ─────────────────────────────────────────────────────────────

    using Destructor_t = void* (*)(RE::NiSourceTexture*, std::uint32_t);
    std::atomic<Destructor_t> g_originalDestructor{nullptr};

    void* Hook_Destructor(RE::NiSourceTexture* a_texture, std::uint32_t a_flags) {
        if (g_pending.load(std::memory_order_acquire) != 0 || g_handedCount.load(std::memory_order_acquire) != 0 || g_handedWatchCount.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lock(g_lock);
            if (const auto it = g_records.find(a_texture); it != g_records.end()) {
                const auto record = it->second;
                record->dead = true;
                FinishLocked(record);
            }
            if (g_handedWatch.erase(a_texture) != 0) g_handedWatchCount.store(static_cast<std::uint32_t>(g_handedWatch.size()), std::memory_order_release);
            if (const auto it = g_handed.find(a_texture); it != g_handed.end()) {
                Release(it->second->stream);
                g_handed.erase(it);
                g_handedCount.store(static_cast<std::uint32_t>(g_handed.size()), std::memory_order_release);
            }
        }
        return g_originalDestructor.load(std::memory_order_acquire)(a_texture, a_flags);
    }

    // ── What has been drawn: without a lock on the render path ───────────────

    constexpr std::uint32_t kDrawnSlots = 1u << 15;
    std::array<std::atomic<const RE::NiSourceTexture*>, kDrawnSlots> g_drawn{};
    std::atomic<std::uint32_t> g_drawnHead{0};

    using SetupGeometry_t = void (*)(RE::BSShader*, RE::BSRenderPass*, std::uint32_t);
    std::atomic<SetupGeometry_t> g_originalSetupGeometry{nullptr};
    // The thread that last drew with the lighting shader, so with the device context: swaps happen only at its
    // Presents. The game loads its data files on another thread, so the thread of kDataLoaded is not it.
    std::atomic<std::uint32_t> g_drawThread{0};
    // Only ever asked whether it moved, so a lost count does not matter and it costs no locked instruction per draw.
    std::atomic<std::uint64_t> g_lightingDraws{0};

    void Hook_SetupGeometry(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_flags) {
        g_originalSetupGeometry.load(std::memory_order_acquire)(a_shader, a_pass, a_flags);
        g_drawThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_lightingDraws.store(g_lightingDraws.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (g_pending.load(std::memory_order_relaxed) == 0) return;
        if (!a_pass || !a_pass->shaderProperty || !a_pass->shaderProperty->material) return;
        auto* base = a_pass->shaderProperty->material;
        if (base->GetType() != RE::BSShaderMaterial::Type::kLighting) return;
        // The material writes all its textures at once: Community Shaders' landscape material has up to 28.
        std::array<RE::NiSourceTexture*, 64> textures{};
        const auto count = std::min<std::uint32_t>(static_cast<RE::BSLightingShaderMaterialBase*>(base)->GetTextures(textures.data()), 64);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (textures[i]) g_drawn[g_drawnHead.fetch_add(1, std::memory_order_relaxed) & (kDrawnSlots - 1)].store(textures[i], std::memory_order_relaxed);
        }
    }

    // ── The worker ───────────────────────────────────────────────────────────

    void MoveDrawnForward() {
        std::scoped_lock lock(g_lock);
        static std::uint32_t tail = 0;
        const auto head = g_drawnHead.load(std::memory_order_relaxed);
        if (head - tail > kDrawnSlots) tail = head - kDrawnSlots;
        for (; tail != head; ++tail) {
            const auto it = g_records.find(g_drawn[tail & (kDrawnSlots - 1)].load(std::memory_order_relaxed));
            if (it != g_records.end() && it->second->queued && !it->second->drawn) {
                it->second->drawn = true;
                g_drawnFirst.push_back(it->second);
            }
        }
    }

    RecordPtr NextRecord() {
        std::scoped_lock lock(g_lock);
        for (auto* queue : {&g_drawnFirst, &g_waiting}) {
            while (!queue->empty()) {
                auto record = std::move(queue->front());
                queue->pop_front();
                if (record->queued && !record->dead) {
                    record->queued = false;
                    return record;
                }
            }
        }
        return nullptr;
    }

    ID3D11Device* Device() { return reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice()); }

    void GiveUp(const RecordPtr& a_record) {
        std::scoped_lock lock(g_lock);
        if (!a_record->dead) FinishLocked(a_record);
    }

    // An archive entry is decoded here from the archive itself: a clone of the game's
    // stream would run every mod hooked into its class, at a time the game never
    // clones it, after it closed. Faster File Copy, when it served the entry from its
    // own cache, moves such a clone before it is open, and the game crashes.
    // Anything else (loose files, stored entries) is read through a clone, in one
    // read.
    bool ReadWhole(const Record& a_record, std::vector<std::byte>& a_data) {
        std::unique_lock lock(g_lock);
        if (a_record.dead || !a_record.stream) return false;
        if (SourceOf(a_record.stream) == kCompressed) {
            const auto start = StartOf(a_record.stream);
            const auto stored = a_record.stream->totalSize;
            lock.unlock();
            const auto* index = g_index.load(std::memory_order_acquire);
            const auto entry = index ? ArchiveTail::Locate(*index, start, stored, g_unindexedReach.load(std::memory_order_acquire)) : std::nullopt;
            const auto head = entry ? ArchiveTail::ReadHead(*entry) : std::nullopt;
            if (!head || head->original != a_record.fileBytes) return false;
            a_data.resize(a_record.fileBytes);
            return ArchiveTail::ReadEntry(*entry, *head, reinterpret_cast<std::uint8_t*>(a_data.data()));
        }
        auto* clone = CloneOf(a_record.stream);
        lock.unlock();
        if (!clone) return false;
        bool ok = false;
        if (Open(clone) == ErrorCode::kNone && clone->totalSize == a_record.fileBytes) {
            a_data.resize(a_record.fileBytes);
            ok = ReadAll(clone, a_data.data(), a_data.size());
        }
        Close(clone);
        Release(clone);
        return ok;
    }

    // Beside Texture Downscaler, the full version goes past its CreateTexture2D hook, which would cut it again to its Diffuse
    // limit: CreateTexture2D1 is another entry of the same device that it leaves alone (checked in downscaler_bypass_tests).
    // Where the device offers no ID3D11Device3, as some wrappers may not, its hook gets the texture already at its size and
    // can only cut it further.
    ComPtr<ID3D11Device3> g_device3;

    HRESULT CreateFull(const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out) {
        if (g_downscalerInstalled && g_device3) {
            D3D11_TEXTURE2D_DESC1 desc{};
            desc.Width = a_desc.Width;
            desc.Height = a_desc.Height;
            desc.MipLevels = a_desc.MipLevels;
            desc.ArraySize = a_desc.ArraySize;
            desc.Format = a_desc.Format;
            desc.SampleDesc = a_desc.SampleDesc;
            desc.Usage = a_desc.Usage;
            desc.BindFlags = a_desc.BindFlags;
            desc.CPUAccessFlags = a_desc.CPUAccessFlags;
            desc.MiscFlags = a_desc.MiscFlags;
            desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
            ComPtr<ID3D11Texture2D1> texture;
            const auto result = g_device3->CreateTexture2D1(&desc, a_data, texture.GetAddressOf());
            if (SUCCEEDED(result)) *a_out = texture.Detach();
            return result;
        }
        return Device()->CreateTexture2D(&a_desc, a_data, a_out);
    }

    void Upgrade(const RecordPtr& a_record, std::vector<std::byte>& a_data) {
        const auto readStarted = Qpc();
        if (!ReadWhole(*a_record, a_data)) {
            SKSE::log::warn("[stream] a texture's file could not be read back; it stays small");
            return GiveUp(a_record);
        }
        g_upgradeReadTicks.fetch_add(static_cast<std::uint64_t>(Qpc() - readStarted), std::memory_order_relaxed);
        g_upgradeReadBytes.fetch_add(a_record->fileBytes, std::memory_order_relaxed);

        const auto file = SkyrimSource::Parse(std::span<const std::byte>(a_data.data(), std::min<std::size_t>(a_data.size(), MipTail::kHeaderMax)), a_record->fileBytes);
        const auto& expected = a_record->file;
        if (!file || file->width != expected.width || file->height != expected.height || file->mips != expected.mips || file->format != expected.format) {
            SKSE::log::warn("[stream] a texture's file does not match what was loaded; it stays small");
            return GiveUp(a_record);
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        {
            std::scoped_lock lock(g_lock);
            if (a_record->dead) return;
            const auto* current = a_record->owner->rendererTexture;
            if (!current || reinterpret_cast<ID3D11Texture2D*>(current->texture) != a_record->expectedTexture ||
                reinterpret_cast<ID3D11ShaderResourceView*>(current->resourceView) != a_record->expectedView) {
                FinishLocked(a_record);
                return;
            }
            a_record->expectedTexture->GetDesc(&textureDesc);
            a_record->expectedView->GetDesc(&viewDesc);
        }
        if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return GiveUp(a_record);
        // The file's bytes go to the graphics card as they are, which is only right in the format the game gave them.
        if (!SkyrimSource::Compatible(expected.format, static_cast<std::uint32_t>(textureDesc.Format), static_cast<std::uint32_t>(viewDesc.Format))) {
            SKSE::log::warn("[stream] the game made format {} of a texture whose file has format {}; it stays small", static_cast<std::uint32_t>(textureDesc.Format), expected.format);
            return GiveUp(a_record);
        }
        const auto skip = std::min(a_record->downscalerSkip, expected.mips - 1);
        textureDesc.Width = expected.width >> skip;
        textureDesc.Height = expected.height >> skip;
        textureDesc.MipLevels = expected.mips - skip;
        viewDesc.Texture2D.MostDetailedMip = 0;
        viewDesc.Texture2D.MipLevels = static_cast<UINT>(-1);

        {
            std::scoped_lock creditLock(g_creditLock);
            WaitForUploadRoom(a_record->fileBytes);
        }
        g_workerDoing.store(WorkerDoing::kCreating, std::memory_order_relaxed);
        const auto creationStarted = Qpc();
        Ready ready;
        ready.record = a_record;
        const auto mips = MipTail::MipsOf(expected);
        std::vector<D3D11_SUBRESOURCE_DATA> initial(mips.size() - skip);
        for (std::size_t i = 0; i < initial.size(); ++i) {
            const auto& mip = mips[i + skip];
            initial[i].pSysMem = a_data.data() + mip.offset;
            initial[i].SysMemPitch = mip.rowPitch;
            initial[i].SysMemSlicePitch = mip.slicePitch;
        }
        if (FAILED(CreateFull(textureDesc, initial.data(), ready.texture.GetAddressOf())) ||
            FAILED(Device()->CreateShaderResourceView(ready.texture.Get(), &viewDesc, ready.view.GetAddressOf())) ||
            !(ready.completion = SkyrimGpu::CreateCompletion(Device()))) {
            SKSE::log::warn("[stream] a texture's full version could not be created; it stays small");
            return GiveUp(a_record);
        }
        const auto creationTicks = static_cast<std::uint64_t>(Qpc() - creationStarted);
        g_upgradeCreateTicks.fetch_add(creationTicks, std::memory_order_relaxed);
        g_creations.fetch_add(1, std::memory_order_relaxed);
        g_createdBytes.fetch_add(a_record->fileBytes, std::memory_order_relaxed);
        if (creationTicks > g_worstCreationTicks.load(std::memory_order_relaxed)) g_worstCreationTicks.store(creationTicks, std::memory_order_relaxed);
        g_frameCreations.fetch_add(1, std::memory_order_relaxed);
        g_frameCreatedBytes.fetch_add(a_record->fileBytes, std::memory_order_relaxed);
        if (creationTicks > g_frameLongestCreation.load(std::memory_order_relaxed)) g_frameLongestCreation.store(creationTicks, std::memory_order_relaxed);
        std::scoped_lock lock(g_lock);
        if (!a_record->dead) g_ready.push_back(std::move(ready));
    }

    void FreeDone();

    void Worker(std::uint32_t a_workerIndex) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::vector<std::byte> data;
        for (;;) {
            {
                std::unique_lock lock(g_lock);
                g_wake.wait_for(lock, 20ms, [] { return !g_waiting.empty() || !g_drawnFirst.empty(); });
            }
            FreeDone();
            MoveDrawnForward();
            if (a_workerIndex == 0) CheckSharing();
            // Loading screens wait for their textures, not for these; nothing can swap them in once Present is lost.
            if (g_loading.load(std::memory_order_acquire) || g_presentLost.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(20ms);
                continue;
            }
            g_activeWorkers.fetch_add(1, std::memory_order_acq_rel);
            auto record = NextRecord();
            if (!record) {
                const auto left = g_activeWorkers.fetch_sub(1, std::memory_order_acq_rel) - 1;
                std::vector<std::byte>().swap(data);  // nothing to read for now: no reason to keep the memory
                if (left == 0 && g_pending.load(std::memory_order_relaxed) == 0) {
                    if (const auto since = g_restoreStarted.exchange(0, std::memory_order_acq_rel)) {
                        const auto rBytes = g_upgradeReadBytes.exchange(0, std::memory_order_relaxed);
                        const auto rTicks = g_upgradeReadTicks.exchange(0, std::memory_order_relaxed);
                        const auto cTicks = g_upgradeCreateTicks.exchange(0, std::memory_order_relaxed);
                        SKSE::log::info("[stream] all full textures in place {:.2f} s after the loading screen (worker read {:.0f} MiB in {:.2f} s, create {:.2f} s)",
                                        QpcSeconds(Qpc() - since), MiB(rBytes), QpcSeconds(static_cast<std::int64_t>(rTicks)),
                                        QpcSeconds(static_cast<std::int64_t>(cTicks)));
                    }
                }
                continue;
            }
            g_workerDoing.store(WorkerDoing::kReading, std::memory_order_relaxed);
            try {
                Upgrade(record, data);
            } catch (const std::exception& error) {
                SKSE::log::warn("[stream] {} while fetching a full texture; it stays small", error.what());
                GiveUp(record);
                std::vector<std::byte>().swap(data);
            }
            g_workerDoing.store(WorkerDoing::kNothing, std::memory_order_relaxed);
            g_activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // ── Present ──────────────────────────────────────────────────────────────

    struct Retired {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11Query> completion;
    };
    std::deque<Retired> g_retired;
    // What the graphics card is done with. A worker frees it: on the render thread that waited behind whatever texture a
    // worker was creating, 17 to 23 ms in the frames logged.
    std::mutex g_doneLock;
    std::vector<Retired> g_done;

    void FreeDone() {
        std::vector<Retired> done;
        {
            std::scoped_lock lock(g_doneLock);
            done.swap(g_done);
        }
    }

    constexpr std::size_t kSlot_Present = 8;
    constexpr std::size_t kSlot_SetupGeometry = 6;
    constexpr std::size_t kSwapsPerPresent = 32;

    using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    std::atomic<Present_t> g_originalPresent{nullptr};
    std::atomic<decltype(RE::BSGraphics::Renderer::GetCurrentRenderWindow())> g_window{nullptr};
    std::atomic<std::uint64_t> g_gamePresents{0}, g_otherThreadPresents{0};
    std::atomic<std::uint32_t> g_otherPresentThread{0};

    IDXGISwapChain* GameSwapChain() {
        auto* window = g_window.load(std::memory_order_acquire);
        return window ? reinterpret_cast<IDXGISwapChain*>(window->swapChain) : nullptr;
    }

    ID3D11DeviceContext* Context() {
        auto* data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        return data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
    }

    // The game remembers what each pixel shader slot holds and skips binding a
    // pointer it thinks is already there. Once the old view is gone a new one can
    // land on its address, which would then count as bound: a black texture.
    void ForgetBound(ID3D11ShaderResourceView* a_view) {
        auto* state = RE::BSGraphics::RendererShadowState::GetSingleton();
        if (!state) return;
        const auto forget = [&](auto& a_data) {
            for (std::uint32_t slot = 0; slot < 16; ++slot) {
                if (reinterpret_cast<ID3D11ShaderResourceView*>(a_data.PSTexture[slot]) == a_view) {
                    a_data.PSTexture[slot] = nullptr;
                    a_data.PSResourceModifiedBits |= 1u << slot;
                }
            }
        };
        if (REL::Module::IsVR()) forget(state->GetVRRuntimeData());
        else forget(state->GetRuntimeData());
    }

    void SwapIn(Ready& a_ready) {
        const auto& record = a_ready.record;
        std::scoped_lock lock(g_lock);
        if (record->dead) return;
        auto* target = record->owner->rendererTexture;
        // Replaced by something else since it loaded: not ours to overwrite.
        if (!target || reinterpret_cast<ID3D11Texture2D*>(target->texture) != record->expectedTexture ||
            reinterpret_cast<ID3D11ShaderResourceView*>(target->resourceView) != record->expectedView) {
            FinishLocked(record);
            return;
        }
        Retired retired;
        retired.completion = std::move(a_ready.completion);
        Context()->End(retired.completion.Get());
        retired.texture.Attach(record->expectedTexture);
        retired.view.Attach(record->expectedView);
        g_retired.push_back(std::move(retired));
        ForgetBound(record->expectedView);
        target->texture = reinterpret_cast<decltype(target->texture)>(a_ready.texture.Detach());
        target->resourceView = reinterpret_cast<decltype(target->resourceView)>(a_ready.view.Detach());
        FinishLocked(record);
    }

    HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* a_swapChain, UINT a_interval, UINT a_flags) {
        // Only the game's own frames. Other swap chains of the same class share this
        // vtable (a frame generator's, an overlay's), a test present shows nothing,
        // and the swaps below belong on the thread that draws. Before anything was
        // drawn (measuring only, or the menus) there is nothing to swap yet.
        if (a_swapChain != GameSwapChain() || (a_flags & DXGI_PRESENT_TEST) != 0) return g_originalPresent.load(std::memory_order_acquire)(a_swapChain, a_interval, a_flags);
        const auto rtid = GetCurrentThreadId();
        const auto drawing = g_drawThread.load(std::memory_order_relaxed);
        if (drawing != 0 && rtid != drawing) {
            g_otherPresentThread.store(rtid, std::memory_order_relaxed);
            g_otherThreadPresents.fetch_add(1, std::memory_order_relaxed);
            return g_originalPresent.load(std::memory_order_acquire)(a_swapChain, a_interval, a_flags);
        }
        g_gamePresents.fetch_add(1, std::memory_order_relaxed);
        g_renderThread.store(rtid, std::memory_order_relaxed);
#ifdef FASTER_TEXTURES_BENCH
        LoadProfiler::NoteRenderThread(rtid);
#endif
        const auto presented = Qpc();
        const auto lastPresent = g_lastPresent.exchange(presented, std::memory_order_relaxed);
        const auto workerDoing = g_workerDoing.load(std::memory_order_relaxed);
        const auto frameCreations = g_frameCreations.exchange(0, std::memory_order_relaxed);
        const auto frameCreatedBytes = g_frameCreatedBytes.exchange(0, std::memory_order_relaxed);
        const auto frameLongestCreation = g_frameLongestCreation.exchange(0, std::memory_order_relaxed);
        if (const auto from = g_framesFrom.load(std::memory_order_relaxed); from != 0 && lastPresent >= from) {
            const auto ticks = presented - lastPresent;
            const auto seconds = QpcSeconds(ticks);
            const auto since = QpcSeconds(presented - from);
            (since < kEarlyFrameSeconds ? g_framesEarly : g_framesLater).Count(static_cast<std::uint64_t>(ticks), seconds);
            if (seconds > 0.033 && g_hitchLines.fetch_add(1, std::memory_order_relaxed) < kHitchLinesPerScreen) {
                static constexpr std::array<const char*, 4> kDoing{"idle", "reading", "waiting for room", "creating"};
                SKSE::log::info("[hitch] {:.1f} ms, {:.2f} s after the loading screen: this plugin {:.1f} ms, Present {:.1f} ms; {} full textures created ({:.0f} MiB), longest {:.1f} ms, worker {}; {} swapped and {} released the frame before", 1000.0 * seconds, since,
                                1000.0 * QpcSeconds(g_lastHookTicks.load(std::memory_order_relaxed)), 1000.0 * QpcSeconds(g_lastPresentCallTicks.load(std::memory_order_relaxed)), frameCreations, MiB(frameCreatedBytes),
                                1000.0 * QpcSeconds(static_cast<std::int64_t>(frameLongestCreation)), kDoing[static_cast<std::size_t>(workerDoing)], g_lastFrameSwaps.load(std::memory_order_relaxed), g_lastFrameReleases.load(std::memory_order_relaxed));
            }
        }
        if (lastPresent != 0) {
            const auto from = g_framesFrom.load(std::memory_order_relaxed);
            RefillUploadCredit(presented - lastPresent, from != 0 ? presented - from : 0);
        }
        std::deque<Ready> ready;
        {
            std::scoped_lock lock(g_lock);
            for (std::size_t i = 0; i < kSwapsPerPresent && !g_ready.empty(); ++i) {
                ready.push_back(std::move(g_ready.front()));
                g_ready.pop_front();
            }
        }
        if (!ready.empty()) {
            const auto swapStarted = Qpc();
            for (auto& item : ready) SwapIn(item);
            // What was not swapped in after all (its texture went away meanwhile) is a worker's to free as well.
            std::unique_lock done(g_doneLock, std::defer_lock);
            for (auto& item : ready) {
                if (!item.texture) continue;
                if (!done.owns_lock()) done.lock();
                g_done.push_back({std::move(item.texture), std::move(item.view), std::move(item.completion)});
            }
            if (done.owns_lock()) done.unlock();
            const auto ticks = static_cast<std::uint64_t>(Qpc() - swapStarted);
            g_swaps.fetch_add(ready.size(), std::memory_order_relaxed);
            g_swapTicks.fetch_add(ticks, std::memory_order_relaxed);
            if (ticks > g_worstSwapTicks.load(std::memory_order_relaxed)) g_worstSwapTicks.store(ticks, std::memory_order_relaxed);
        }
        g_lastFrameSwaps.store(static_cast<std::uint32_t>(ready.size()), std::memory_order_relaxed);

        // Only asked here whether the graphics card is done with them, a few each frame; a worker frees them.
        std::uint32_t released = 0;
        if (!g_retired.empty()) {
            const auto releaseStarted = Qpc();
            const auto limit = g_settings.releasesPerFrame;
            std::unique_lock done(g_doneLock, std::defer_lock);
            while (!g_retired.empty() && (limit == 0 || released < limit) && SkyrimGpu::PollCompletion(Context(), g_retired.front().completion.Get()) != S_FALSE) {
                if (!done.owns_lock()) done.lock();
                g_done.push_back(std::move(g_retired.front()));
                g_retired.pop_front();
                ++released;
            }
            if (released != 0) {
                const auto ticks = static_cast<std::uint64_t>(Qpc() - releaseStarted);
                g_releases.fetch_add(released, std::memory_order_relaxed);
                g_releaseTicks.fetch_add(ticks, std::memory_order_relaxed);
                if (ticks > g_worstReleaseTicks.load(std::memory_order_relaxed)) g_worstReleaseTicks.store(ticks, std::memory_order_relaxed);
            }
        }
        g_lastFrameReleases.store(released, std::memory_order_relaxed);

#ifdef FASTER_TEXTURES_BENCH
        {
            const auto from = g_framesFrom.load(std::memory_order_relaxed);
            StampGpuFrame(from != 0 ? presented - from : -1);
        }
#endif
        const auto presentCall = Qpc();
        g_lastHookTicks.store(presentCall - presented, std::memory_order_relaxed);
        const auto result = g_originalPresent.load(std::memory_order_acquire)(a_swapChain, a_interval, a_flags);
        g_lastPresentCallTicks.store(Qpc() - presentCall, std::memory_order_relaxed);
#ifdef FASTER_TEXTURES_BENCH
        if (result == S_OK) {
            g_presentOk.fetch_add(1, std::memory_order_relaxed);
        } else if (result == DXGI_STATUS_OCCLUDED) {
            g_presentOccluded.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_presentOther.fetch_add(1, std::memory_order_relaxed);
            g_presentOtherCode.store(static_cast<std::uint32_t>(result), std::memory_order_relaxed);
        }
#endif
        return result;
    }

    // ── Loading screens ──────────────────────────────────────────────────────

    void LogLine(const char* a_title, double a_seconds, const Totals& a_totals) {
        const auto count = a_totals.textures.load();
        if (count == 0 && a_seconds < 0.5) return;
        const auto u = [](const std::atomic<std::uint64_t>& a_value) { return a_value.load(std::memory_order_relaxed); };
        const auto busy = QpcSeconds(static_cast<std::int64_t>(u(a_totals.busyTicks)));
        const auto create = QpcSeconds(static_cast<std::int64_t>(u(a_totals.createTicks)));
        const auto read = QpcSeconds(static_cast<std::int64_t>(u(a_totals.readTicks)));
        SKSE::log::info("[{}] {:.2f} s | {} textures: busy {:.2f} s ({:.0f} %), sum {:.2f} s = read and unpack {:.2f} s + create {:.2f} s, read {:.0f} MiB", a_title, a_seconds, count, busy,
                        a_seconds > 0 ? 100.0 * busy / a_seconds : 0.0, create, read, create - read, MiB(u(a_totals.readBytes)));
        const auto atSize = g_downscalerInstalled ? std::format(", {} of them already at Texture Downscaler's size", u(a_totals.atDownscalerSize)) : std::string();
        SKSE::log::info("[{}]   loaded small: {} from the end of the file, {} from the cache{}; left out {:.0f} MiB | kept whole: excluded {}, unnamed {}, nothing to gain {}, whole frame {}, not located {}, failed {}, other mods' files {}",
                        a_title, u(a_totals.smallFromTail), u(a_totals.smallFromCache), atSize, MiB(u(a_totals.leftOutBytes)), u(a_totals.excluded), u(a_totals.unnamed), u(a_totals.noPlan),
                        u(a_totals.wholeFrame), u(a_totals.notFound), u(a_totals.failed), u(a_totals.foreign));
        SKSE::log::info("[{}]   loose {} ({:.0f} MiB, {:.2f} s) | stored {} ({:.0f} MiB, {:.2f} s) | compressed {} ({:.0f} MiB, {:.2f} s) | could leave out {:.0f} MiB, {:.0f} MiB of it without unpacking; excluded paths hold {:.0f} MiB more, unnamed {:.0f} MiB",
                        a_title, u(a_totals.sourceCount[kLoose]), MiB(u(a_totals.sourceBytes[kLoose])), QpcSeconds(static_cast<std::int64_t>(u(a_totals.sourceTicks[kLoose]))), u(a_totals.sourceCount[kStored]),
                        MiB(u(a_totals.sourceBytes[kStored])), QpcSeconds(static_cast<std::int64_t>(u(a_totals.sourceTicks[kStored]))), u(a_totals.sourceCount[kCompressed]),
                        MiB(u(a_totals.sourceBytes[kCompressed])), QpcSeconds(static_cast<std::int64_t>(u(a_totals.sourceTicks[kCompressed]))), MiB(u(a_totals.possibleBytes)), MiB(u(a_totals.possibleTailBytes)), MiB(u(a_totals.possibleExcludedBytes)), MiB(u(a_totals.possibleUnnamedBytes)));
        const auto sizes = [&](const char* a_kind, const std::array<SizeFigures, kSizeCount>& a_figures) {
            std::string line;
            for (int i = 0; i < kSizeCount; ++i) {
                const auto n = u(a_figures[i].count);
                if (n == 0) continue;
                line += std::format("{}{} {} ({:.0f} MiB, {:.2f} s read, {:.2f} s create)", line.empty() ? "" : " | ", kSizeNames[i], n, MiB(u(a_figures[i].bytes)),
                                    QpcSeconds(static_cast<std::int64_t>(u(a_figures[i].readTicks))), QpcSeconds(static_cast<std::int64_t>(u(a_figures[i].createTicks))));
            }
            if (!line.empty()) SKSE::log::info("[{}]   {}: {}", a_title, a_kind, line);
        };
        sizes("whole", a_totals.whole);
        sizes("small first", a_totals.small_);
        if (u(a_totals.handedToSps) != 0) SKSE::log::info("[{}]   handed to Skyrim Performance Solution: {}", a_title, u(a_totals.handedToSps));
        if (u(a_totals.renderThreadCount) != 0)
            SKSE::log::info("[{}]   on the render thread: {} textures, {:.2f} s, worst {:.1f} ms", a_title, u(a_totals.renderThreadCount), QpcSeconds(static_cast<std::int64_t>(u(a_totals.renderThreadTicks))),
                            1000.0 * QpcSeconds(static_cast<std::int64_t>(u(a_totals.worstRenderThreadTicks))));
    }

    void LogFrames() {
        const auto describe = [](const FrameFigures& a_figures) {
            const auto frames = a_figures.frames.load(std::memory_order_relaxed);
            return std::format("{} frames, average {:.1f} ms, worst {:.1f} ms, over 33 ms {}, over 50 ms {}, over 100 ms {}", frames,
                               frames ? 1000.0 * QpcSeconds(static_cast<std::int64_t>(a_figures.ticks.load(std::memory_order_relaxed))) / static_cast<double>(frames) : 0.0,
                               1000.0 * QpcSeconds(static_cast<std::int64_t>(a_figures.worstTicks.load(std::memory_order_relaxed))), a_figures.over33.load(std::memory_order_relaxed),
                               a_figures.over50.load(std::memory_order_relaxed), a_figures.over100.load(std::memory_order_relaxed));
        };
        if (g_framesEarly.frames.load(std::memory_order_relaxed) != 0) {
            const auto seconds = [](const std::atomic<std::uint64_t>& a_ticks) { return 1000.0 * QpcSeconds(static_cast<std::int64_t>(a_ticks.load(std::memory_order_relaxed))); };
            auto line = std::format("[frames] first {:.0f} s: {} | after: {} | full textures swapped in: {} in {:.1f} ms, worst frame's {:.1f} ms | released: {} in {:.1f} ms, worst frame's {:.1f} ms | created: {} ({:.0f} MiB), longest {:.1f} ms | uploads held for {} late frames",
                                    kEarlyFrameSeconds, describe(g_framesEarly), describe(g_framesLater), g_swaps.load(std::memory_order_relaxed), seconds(g_swapTicks), seconds(g_worstSwapTicks), g_releases.load(std::memory_order_relaxed),
                                    seconds(g_releaseTicks), seconds(g_worstReleaseTicks), g_creations.load(std::memory_order_relaxed), MiB(g_createdBytes.load(std::memory_order_relaxed)), seconds(g_worstCreationTicks), g_framesHeldThisScreen.load(std::memory_order_relaxed));
#ifdef FASTER_TEXTURES_BENCH
            line += std::format(" | GPU first {:.0f} s: {} | GPU after: {} | GPU frames lost {} | Present: shown {}, occluded {}, other {} (last 0x{:08X})", kEarlyFrameSeconds, describe(g_gpuEarly), describe(g_gpuLater), g_gpuLost.load(std::memory_order_relaxed),
                                g_presentOk.load(std::memory_order_relaxed), g_presentOccluded.load(std::memory_order_relaxed), g_presentOther.load(std::memory_order_relaxed), g_presentOtherCode.load(std::memory_order_relaxed));
#endif
            SKSE::log::info("{}", line);
        }
        g_framesEarly.Reset();
        g_framesLater.Reset();
#ifdef FASTER_TEXTURES_BENCH
        g_gpuEarly.Reset();
        g_gpuLater.Reset();
        for (auto* value : {&g_presentOk, &g_presentOccluded, &g_presentOther, &g_gpuLost}) value->store(0, std::memory_order_relaxed);
#endif
        for (auto* value : {&g_swaps, &g_swapTicks, &g_worstSwapTicks, &g_releases, &g_releaseTicks, &g_worstReleaseTicks, &g_creations, &g_createdBytes, &g_worstCreationTicks}) value->store(0, std::memory_order_relaxed);
        g_hitchLines.store(0, std::memory_order_relaxed);
        g_framesHeldThisScreen.store(0, std::memory_order_relaxed);
    }

    void BeginScreen() {
        g_screen.Reset();
        g_threadsThisScreen.store(0, std::memory_order_relaxed);
        g_threadGeneration.fetch_add(1, std::memory_order_relaxed);
        g_screenStarted.store(Qpc(), std::memory_order_relaxed);
    }

#ifdef FASTER_TEXTURES_BENCH
    // ── Test walk ────────────────────────────────────────────────────────────
    //
    // For the bench, with [Streamer] TestWalkSeconds: after the first load the player
    // is moved north at a steady speed, well above the ground, so that exterior cells
    // load one row after another as they do when travelling, and the cost is logged.

    std::string DescribeFrames(const FrameFigures& a_first, const FrameFigures& a_second) {
        const auto u = [](const std::atomic<std::uint64_t>& a_value) { return a_value.load(std::memory_order_relaxed); };
        const auto frames = u(a_first.frames) + u(a_second.frames);
        const auto ticks = u(a_first.ticks) + u(a_second.ticks);
        return std::format("{} frames, average {:.1f} ms, worst {:.1f} ms, over 33 ms {}, over 50 ms {}, over 100 ms {}", frames,
                           frames ? 1000.0 * QpcSeconds(static_cast<std::int64_t>(ticks)) / static_cast<double>(frames) : 0.0,
                           1000.0 * QpcSeconds(static_cast<std::int64_t>(std::max(u(a_first.worstTicks), u(a_second.worstTicks)))), u(a_first.over33) + u(a_second.over33),
                           u(a_first.over50) + u(a_second.over50), u(a_first.over100) + u(a_second.over100));
    }

    // The player and its move, looked up here rather than through CommonLib's PlayerCharacter and Actor sources, which would bring
    // half the library into the build for a bench tool.
    RE::PlayerCharacter* Player() {
        static REL::Relocation<RE::NiPointer<RE::PlayerCharacter>*> singleton{RELOCATION_ID(517014, 403521)};
        return singleton->get();
    }

    void MovePlayer(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_position) {
        REL::RelocateVirtual<decltype(&RE::Actor::SetPosition)>(0x0A9, 0x0AB, a_player, a_position, true);
    }

    void WalkThread() {
        std::this_thread::sleep_for(std::chrono::seconds(8));  // the first place settles
        auto* player = Player();
        auto* tasks = SKSE::GetTaskInterface();
        if (!player || !tasks) return;
        const auto start = player->GetPosition();
        const auto z = start.z + 3000.0f;
        const double seconds = g_settings.testWalkSeconds;
        const double speed = g_settings.testWalkSpeed;

        LogLine("before walk", 8.0, g_screen);
        LogFrames();
        BeginScreen();
        const auto begun = Qpc();
        g_framesFrom.store(begun, std::memory_order_relaxed);

        constexpr float kCellUnits = 4096.0f;
        auto cell = static_cast<std::int64_t>(std::floor(start.y / kCellUnits));
        std::uint64_t crossings = 0, samples = 0, depthSum = 0, nonEmpty = 0, bursts = 0;
        std::int64_t burstSince = 0, burstTicks = 0, longestBurst = 0;
        int deepest = 0;
        bool bursting = false;
        for (;;) {
            const auto now = Qpc();
            const auto elapsed = QpcSeconds(now - begun);
            if (elapsed >= seconds) break;
            // Out and back, so that a long walk stays inside the world.
            constexpr double kLegSeconds = 90.0;
            const double leg = std::fmod(elapsed, 2.0 * kLegSeconds);
            const double travelled = speed * (leg < kLegSeconds ? leg : 2.0 * kLegSeconds - leg);
            const RE::NiPoint3 position{start.x, start.y + static_cast<float>(travelled), z};
            tasks->AddTask([position] {
                if (auto* mover = Player()) MovePlayer(mover, position);
            });
            if (const auto reached = static_cast<std::int64_t>(std::floor(position.y / kCellUnits)); reached != cell) {
                cell = reached;
                ++crossings;
            }
            if (const auto depth = LoadQueueProbe::QueueDepth(); depth >= 0) {
                ++samples;
                depthSum += static_cast<std::uint64_t>(depth);
                deepest = std::max(deepest, depth);
                if (depth > 0) {
                    ++nonEmpty;
                    if (!bursting) {  // the loader has work again: until it is through, what comes next waits
                        bursting = true;
                        burstSince = now;
                        ++bursts;
                    }
                } else if (bursting) {
                    bursting = false;
                    burstTicks += now - burstSince;
                    longestBurst = std::max(longestBurst, now - burstSince);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        const auto walked = QpcSeconds(Qpc() - begun);
        SKSE::log::info("[walk] {:.1f} s at {:.0f} units/s, {} cell rows crossed", walked, speed, crossings);
        SKSE::log::info("[walk] CPU frames: {}", DescribeFrames(g_framesEarly, g_framesLater));
        SKSE::log::info("[walk] GPU frames: {}", DescribeFrames(g_gpuEarly, g_gpuLater));
        if (samples != 0)
            SKSE::log::info("[walk] loading queue: average {:.1f} tasks, deepest {}, not empty {:.1f} % of the time; {} bursts of work, average {:.0f} ms, longest {:.0f} ms", static_cast<double>(depthSum) / static_cast<double>(samples), deepest,
                            100.0 * static_cast<double>(nonEmpty) / static_cast<double>(samples), bursts, bursts ? 1000.0 * QpcSeconds(burstTicks) / static_cast<double>(bursts) : 0.0, 1000.0 * QpcSeconds(longestBurst));
        LogLine("walk", walked, g_screen);
        LogFrames();
        LoadQueueProbe::Log("walk");
        BeginScreen();
    }
#endif

    // ── The player's choices from the in-game menus ──────────────────────────
    //
    // SKSE Menu Framework's page saves to the file MCM Helper keeps, so both menus, and the reads below, agree on one place.
    // The page changes them on the render thread, which does no file work: a save waits here and runs in the background,
    // and the reads below write anything still waiting first, so a change can never be undone by an older file.

    constexpr const char* kChoicesFile = "Data/MCM/Settings/FasterTextures.ini";
    std::atomic<std::shared_ptr<const std::string>> g_cacheFolderInUse;  // for the menu's status line

    std::mutex g_choicesLock;
    struct PendingChoices {
        std::optional<bool> onLoadingScreens, duringPlay;
        std::optional<Streamer::CacheChoice> cache;
    } g_pendingChoices;  // under g_choicesLock

    void WriteChoicesLocked() {
        auto& pending = g_pendingChoices;
        if (!pending.onLoadingScreens && !pending.duringPlay && !pending.cache) return;
        try {
            std::filesystem::create_directories(std::filesystem::path(kChoicesFile).parent_path());
        } catch (const std::exception&) {
        }
        CSimpleIniA ini;
        ini.SetUnicode();
        ini.LoadFile(kChoicesFile);  // keeps what is there: missing is fine
        if (pending.onLoadingScreens) ini.SetLongValue("Streamer", "bSmallOnLoadingScreens", *pending.onLoadingScreens ? 1 : 0);
        if (pending.duringPlay) ini.SetLongValue("Streamer", "bSmallDuringPlay", *pending.duringPlay ? 1 : 0);
        if (pending.cache) {
            ini.SetLongValue("Streamer", "iCacheLocation", pending.cache->location);
            ini.SetValue("Streamer", "sCacheFolder", pending.cache->folder.c_str());
        }
        if (ini.SaveFile(kChoicesFile) < 0) {
            SKSE::log::warn("[menu] {} could not be written; the choice lasts until the game closes", kChoicesFile);
            return;  // kept pending: the next change or loading screen tries again
        }
        pending = {};
    }

    void SaveChoicesLater() {
        std::thread([] {
            try {
                std::scoped_lock lock(g_choicesLock);
                WriteChoicesLocked();
            } catch (...) {
            }
        }).detach();
    }

    // Read again at each loading screen and after the menu that holds the MCM closes, so a change needs no restart. MCM Helper writes only what the
    // player changed, to a file of its own, so that file wins where it has the key.
    void ReadSmallFirstChoices() {
        std::scoped_lock choices(g_choicesLock);
        WriteChoicesLocked();
        bool onLoadingScreens = true, duringPlay = true;
        for (const char* file : {"Data/SKSE/Plugins/FasterTextures.ini", "Data/MCM/Settings/FasterTextures.ini"}) {
            CSimpleIniA ini;
            ini.SetUnicode();
            if (ini.LoadFile(file) < 0) continue;
            onLoadingScreens = ini.GetBoolValue("Streamer", "bSmallOnLoadingScreens", onLoadingScreens);
            duringPlay = ini.GetBoolValue("Streamer", "bSmallDuringPlay", duringPlay);
        }
        if (g_smallOnLoadingScreens.exchange(onLoadingScreens, std::memory_order_relaxed) != onLoadingScreens)
            SKSE::log::info("[stream] loading screens now load textures {}", onLoadingScreens ? "small first" : "whole");
        if (g_smallDuringPlay.exchange(duringPlay, std::memory_order_relaxed) != duringPlay)
            SKSE::log::info("[stream] textures that load during play now load {}", duringPlay ? "small first" : "whole");
    }

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (a_event && !a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
                // The MCM lives in this menu, and MCM Helper may write its file just after it closes.
                std::thread([] {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    try {
                        ReadSmallFirstChoices();
                        ReadDownscalerRules();
                    } catch (...) {
                    }
                }).detach();
                return RE::BSEventNotifyControl::kContinue;
            }
            if (!a_event || a_event->menuName != RE::LoadingMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
            const auto now = Qpc();
            if (a_event->opening) {
                // What loaded since the last loading screen happened during play: stutter, if anywhere.
                LogLine("play", QpcSeconds(now - g_screenStarted.load(std::memory_order_relaxed)), g_screen);
                LogHanded();
                g_framesFrom.store(0, std::memory_order_relaxed);
                LogFrames();
#ifdef FASTER_TEXTURES_BENCH
                LoadQueueProbe::Log("play");
#endif
                BeginScreen();
                ReadSmallFirstChoices();
                ReadDownscalerRules();
                g_loadStarted.store(now, std::memory_order_relaxed);
                g_loading.store(true, std::memory_order_release);
#ifdef FASTER_TEXTURES_BENCH
                LoadProfiler::OnScreenStart();
#endif
            } else if (g_loading.exchange(false, std::memory_order_acq_rel)) {
                const auto loadSeconds = QpcSeconds(now - g_loadStarted.load(std::memory_order_relaxed));
                LogLine("load", loadSeconds, g_screen);
                SKSE::log::info("[load]   on {} threads, {} full textures still to come", g_threadsThisScreen.load(std::memory_order_relaxed), g_pending.load(std::memory_order_relaxed));
#ifdef FASTER_TEXTURES_BENCH
                LoadProfiler::OnScreenEnd(loadSeconds);
                LoadQueueProbe::Log("load");
                if (g_settings.testWalkSeconds != 0) {
                    static std::once_flag walking;
                    std::call_once(walking, [] { std::thread(WalkThread).detach(); });
                }
#endif
                g_restoreStarted.store(now, std::memory_order_release);
                g_framesFrom.store(now, std::memory_order_relaxed);
                g_probingSince.store(now, std::memory_order_release);
                BeginScreen();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Full textures swap in at the game's Present. Should it stop reaching the hook
    // while the game goes on drawing (its swap chain replaced by another of a class
    // that was not hooked, or presented from another thread), none would arrive:
    // from then on textures load whole, and what was waiting is let go.
    void WatchPresent(IDXGISwapChain* a_hooked) {
        constexpr auto kQuiet = std::chrono::seconds(10);
        auto seen = g_gamePresents.load(std::memory_order_relaxed);
        auto elsewhere = g_otherThreadPresents.load(std::memory_order_relaxed);
        auto lastSeen = std::chrono::steady_clock::now();
        auto draws = g_lightingDraws.load(std::memory_order_relaxed);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const auto now = std::chrono::steady_clock::now();
            const auto drawsNow = g_lightingDraws.load(std::memory_order_relaxed);
            const bool drewJustNow = drawsNow != draws;
            draws = drawsNow;
            const auto presents = g_gamePresents.load(std::memory_order_relaxed);
            if (presents != seen) {
                seen = presents;
                elsewhere = g_otherThreadPresents.load(std::memory_order_relaxed);
                lastSeen = now;
                continue;
            }
            if (now - lastSeen < kQuiet) continue;
            const bool replaced = GameSwapChain() != a_hooked;
            // Skyrim VR has a thread of its own that goes on presenting this swap chain while the game's stands still.
            const bool otherThread = drewJustNow && g_otherThreadPresents.load(std::memory_order_relaxed) != elsewhere;
            if (!replaced && !otherThread) continue;  // the game is not drawing: minimised, or inside a long load
            g_presentLost.store(true, std::memory_order_release);
            std::size_t abandoned = 0;
            {
                std::scoped_lock lock(g_lock);
                std::vector<RecordPtr> records;
                for (const auto& entry : g_records) records.push_back(entry.second);
                for (const auto& record : records) {
                    record->dead = true;
                    FinishLocked(record);
                }
                abandoned = records.size();
                g_ready.clear();
                g_drawnFirst.clear();
                g_waiting.clear();
            }
            SKSE::log::warn("[stream] the game no longer presents through the hooked swap chain ({}): textures load whole from here on, and {} that loaded small stay small",
                            replaced ? "it was replaced"s
                                     : std::format("thread {} presents it while thread {} draws", g_otherPresentThread.load(std::memory_order_relaxed), g_drawThread.load(std::memory_order_relaxed)),
                            abandoned);
            return;
        }
    }

    // ── Other plugins ────────────────────────────────────────────────────────

    // Skyrim Performance Solution swaps the same texture objects; its texture
    // streaming can be off with the rest of it on.
    bool SpsStreams() {
        if (!GetModuleHandleW(L"SPS.dll") && !GetModuleHandleW(L"SkyrimPerf.dll")) return false;
        bool enabled = true;
        for (const auto* file : {"Data/SKSE/Plugins/SPS.ini", "Data/SKSE/Plugins/SPS_User.ini"}) {
            CSimpleIniA ini;
            ini.SetUnicode();
            if (ini.LoadFile(file) >= 0) enabled = ini.GetBoolValue("TextureStream", "bEnabled", enabled);
        }
        return enabled;
    }

    std::vector<std::string> SplitPaths(std::string a_list) {
        std::vector<std::string> parts;
        for (auto& c : a_list) c = c == '/' ? '\\' : c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        for (std::size_t at = 0; at <= a_list.size();) {
            const auto comma = std::min(a_list.find(',', at), a_list.size());
            auto part = a_list.substr(at, comma - at);
            std::erase(part, ' ');
            if (!part.empty()) parts.push_back(std::move(part));
            at = comma + 1;
        }
        return parts;
    }

    // SPS's exclude list, its own default when no ini says otherwise. Characters are decided by its own switches.
    void ReadSpsExclude() {
        std::string list = "interface\\,\\maps\\,mapmarker,\\lod\\,terrain\\,fonts\\,book,\\sky\\,effects\\,cubemaps\\";
        for (const auto* file : {"Data/SKSE/Plugins/SPS.ini", "Data/SKSE/Plugins/SPS_User.ini"}) {
            CSimpleIniA ini;
            ini.SetUnicode();
            if (ini.LoadFile(file) >= 0) list = ini.GetValue("TextureStream", "sExclude", list.c_str());
        }
        g_spsExclude = SplitPaths(std::move(list));
        std::erase(g_spsExclude, std::string("actors\\character\\"));
    }

    void ReadSettings() {
        CSimpleIniA ini;
        ini.SetUnicode();
        ini.LoadFile("Data/SKSE/Plugins/FasterTextures.ini");
        ReadSmallFirstChoices();
        const auto mode = ini.GetLongValue("Streamer", "Mode", static_cast<long>(g_settings.mode));
        g_settings.mode = mode <= 0 ? Mode::kOff : mode == 1 ? Mode::kMeasure : Mode::kStream;
        g_settings.skipMips = static_cast<std::uint32_t>(std::clamp<long>(ini.GetLongValue("Streamer", "SkipMips", 2), 1, 4));
        g_settings.minEdge = static_cast<std::uint32_t>(std::clamp<long>(ini.GetLongValue("Streamer", "MinEdge", 512), 64, 4096));
        g_settings.minSavedBytes = static_cast<std::uint64_t>(std::max<long>(ini.GetLongValue("Streamer", "MinSavedKiB", 256), 0)) << 10;
        g_settings.cache = ini.GetBoolValue("Streamer", "Cache", true);
        g_settings.cacheFill = ini.GetBoolValue("Streamer", "CacheFill", true);
        // The MCM's choice of folder where the player made one (0 beside the game, 1 local app data, 2 the folder typed there), otherwise the one in this file.
        CSimpleIniA mcm;
        mcm.SetUnicode();
        mcm.LoadFile("Data/MCM/Settings/FasterTextures.ini");
        const auto location = mcm.GetLongValue("Streamer", "iCacheLocation", -1);
        g_settings.cacheFolder = location == 2 ? mcm.GetValue("Streamer", "sCacheFolder", "") : location < 0 ? ini.GetValue("Streamer", "CacheFolder", "") : "";
        g_settings.cacheInLocalAppData = location == 1;
        g_settings.uploadBytesPerFrame = static_cast<std::int64_t>(std::clamp<long>(ini.GetLongValue("Streamer", "UploadMiBPerFrame", 16), 0, 1024)) << 20;
        g_settings.releasesPerFrame = static_cast<std::uint32_t>(std::clamp<long>(ini.GetLongValue("Streamer", "ReleasesPerFrame", 4), 0, 4096));
        g_settings.upgradeWorkers = static_cast<std::uint32_t>(std::clamp<long>(ini.GetLongValue("Streamer", "UpgradeWorkers", 2), 1, 8));
#ifdef FASTER_TEXTURES_BENCH
        g_settings.profile = ini.GetBoolValue("Streamer", "Profile", false);
        g_settings.profileQueue = ini.GetBoolValue("Streamer", "ProfileQueue", false);
        g_settings.testWalkSeconds = static_cast<std::uint32_t>(std::clamp<long>(ini.GetLongValue("Streamer", "TestWalkSeconds", 0), 0, 3600));
        g_settings.testWalkSpeed = static_cast<float>(std::clamp<double>(ini.GetDoubleValue("Streamer", "TestWalkSpeed", 800.0), 50.0, 20000.0));
#endif

        g_settings.exclude = SplitPaths(ini.GetValue("Streamer", "Exclude", "interface\\,\\maps\\,mapmarker,\\lod\\,terrain\\,fonts\\,book,\\sky\\,effects\\,cubemaps\\,actors\\character\\"));
        g_settings.armorExclude = SplitPaths(ini.GetValue("Streamer", "ArmorExclude", "armor\\,clothes\\,clothing\\"));
    }

    // Mods that make lasting copies of armor and clothing textures as they load: tints,
    // dyes, generated normal maps. They would copy the small version, so beside them
    // those textures load whole.
    const char* ArmorCopierLoaded() {
        constexpr std::pair<const wchar_t*, const char*> kCopiers[]{
            {L"skee64.dll", "RaceMenu"}, {L"skeevr.dll", "RaceMenu"}, {L"MuDynamicNormalMap.dll", "Mu Dynamic NormalMap"},
            {L"MaterialSwapperFramework.dll", "Material Swapper Framework"}, {L"FittingRoom.dll", "Fitting Room"}};
        for (const auto& [file, name] : kCopiers)
            if (GetModuleHandleW(file)) return name;
        return nullptr;
    }

    // Beside the game rather than in Data, which mod managers redirect, and on its disk if it can be.
    void OpenCache(const ArchiveIndex::Index& a_index, const std::filesystem::path& a_gameFolder) {
        g_archiveIds.reserve(a_index.archives.size());
        for (const auto& archive : a_index.archives) g_archiveIds.push_back(TailCache::ArchiveId(archive.handle, archive.name));

        std::vector<std::filesystem::path> folders;
        if (!g_settings.cacheFolder.empty()) folders.emplace_back(std::filesystem::u8path(g_settings.cacheFolder));
        folders.push_back(a_gameFolder / "FasterTextures Cache");
        wchar_t local[MAX_PATH]{};
        if (const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH); length != 0 && length < MAX_PATH) folders.push_back(std::filesystem::path(local) / "FasterTextures" / "Cache");

        // Local app data chosen in the MCM: first, and beside the game becomes what is tried if it cannot be written.
        if (g_settings.cacheInLocalAppData && folders.size() == 2) std::swap(folders[0], folders[1]);

        auto cache = std::make_unique<TailCache::Store>();
        for (const auto& folder : folders) {
            if (!cache->Open(folder, (std::numeric_limits<std::uint64_t>::max)(), g_archiveIds)) continue;  // no limit: only a full disk stops it
            const auto figures = cache->Current();
            SKSE::log::info("[stream] small mips cache in {}: {} textures, {:.0f} MiB", Utf8(folder), figures.entries, MiB(figures.bytes));
            g_cacheFolderInUse.store(std::make_shared<const std::string>(Utf8(folder)), std::memory_order_release);
            g_cache.store(cache.release(), std::memory_order_release);  // never freed: in use until the game exits
            return;
        }
        SKSE::log::warn("[stream] no folder could hold the small mips cache; archive entries that cannot be read from their end load whole");
    }

    struct FillItem {
        std::uint32_t archive{}, offset{}, size{};
        std::uint8_t bits{};
    };

    // Fills the cache ahead of the first load of each texture, on the game's own
    // time: at background priority for processor and disk alike, and never while a
    // loading screen or a full texture waits.
    void FillCache(const ArchiveIndex::Index& a_index, std::vector<FillItem> a_items) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        while (!g_streaming.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::seconds(1));
        auto* cache = g_cache.load(std::memory_order_acquire);
        if (!cache) return;

        std::sort(a_items.begin(), a_items.end(), [](const FillItem& a, const FillItem& b) { return a.archive != b.archive ? a.archive < b.archive : a.offset < b.offset; });
        const auto planOf = [](const SkyrimSource::Info& a_file) { return PlanOf(a_file); };
        const auto started = Qpc();
        TailCache::FillBuffers buffers;
        std::uint64_t added = 0, failed = 0;
        bool stoppedFull = false;
        const auto whole = g_wholeBits.load(std::memory_order_acquire);
        for (const auto& item : a_items) {
            if ((item.bits & whole) != 0) continue;
            if (cache->WriteFailed()) break;  // its disk is full: nothing more can be kept
            while (g_loading.load(std::memory_order_acquire) || g_pending.load(std::memory_order_relaxed) != 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto archive = g_archiveIds[item.archive];
            const auto entry = archive != 0 ? ArchiveTail::EntryAt(a_index, item.archive, item.offset, item.size) : std::nullopt;
            if (!entry) continue;
            try {
                const auto result = TailCache::FillEntry(*cache, *entry, archive, planOf, buffers);
                if (result == TailCache::Filled::kAdded) ++added;
                else if (result == TailCache::Filled::kFailed) ++failed;
                if (result == TailCache::Filled::kFull) {
                    stoppedFull = true;
                    break;
                }
            } catch (const std::exception&) {
                ++failed;
                buffers = {};
            }
        }
        cache->Drain();
        const auto figures = cache->Current();
        SKSE::log::info("[stream] small mips cache filled: {} added, {} failed, in {:.0f} s; it holds {} textures, {:.0f} MiB{}", added, failed, QpcSeconds(Qpc() - started), figures.entries, MiB(figures.bytes), stoppedFull ? ", and is full" : "");
        if (cache->WriteFailed()) SKSE::log::warn("[stream] the small mips cache could not be written; is the disk full?");
    }

    void BuildIndex() {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const auto started = Qpc();
        const auto gameFolder = std::filesystem::path(exe).parent_path();
        const auto data = gameFolder / "Data";
        auto index = ArchiveIndex::Build(data);
        SKSE::log::info("[stream] indexed {} archives, {} repacked, {} compressed entries, in {:.0f} ms", index->archives.size(), index->repacked, index->entries.size(), 1000.0 * QpcSeconds(Qpc() - started));
        {
            // An archive the index left out that may hold compressed entries: a stream
            // from it could look like an entry of another archive.
            std::unordered_set<std::string> indexed;
            for (const auto& archive : index->archives) indexed.insert(archive.name);
            std::uint64_t reach = 0;
            std::size_t unindexed = 0;
            std::error_code walkError;
            for (const auto& file : std::filesystem::directory_iterator(data, walkError)) {
                auto extension = file.path().extension().wstring();
                for (auto& c : extension) c = static_cast<wchar_t>(std::towlower(c));
                if (extension != L".bsa") continue;
                const auto name = file.path().filename().u8string();
                if (indexed.contains(std::string(name.begin(), name.end()))) continue;
                if (const auto info = Bsa::Inspect(file.path()); info && info->codec == Bsa::Codec::kNone) continue;
                std::error_code sizeError;
                const auto bytes = std::filesystem::file_size(file.path(), sizeError);
                reach = std::max<std::uint64_t>(reach, sizeError ? ~0ull : bytes);
                ++unindexed;
            }
            g_unindexedReach.store(reach, std::memory_order_release);
            if (unindexed != 0)
                SKSE::log::warn("[stream] {} archives could not be indexed, the largest {:.0f} MiB: compressed textures that could be in them load whole", unindexed, MiB(reach));
        }
        // Never freed: the handles and entries are in use until the game exits.
        const auto* published = index.release();
        const bool useCache = g_settings.mode == Mode::kStream && g_settings.cache;
        if (useCache) OpenCache(*published, gameFolder);
        g_index.store(published, std::memory_order_release);

        std::unordered_map<std::string, std::uint32_t> archiveNumbers;
        for (std::uint32_t i = 0; i < published->archives.size(); ++i) archiveNumbers.emplace(published->archives[i].name, i);
        std::vector<FillItem> fillItems;

        const auto namesStarted = Qpc();
        auto paths = std::make_unique<std::vector<ArchiveTail::PathEntry>>();
        std::size_t archives = 0, unread = 0;
        std::uint64_t unreadReach = 0;
        std::error_code error;
        for (const auto& file : std::filesystem::directory_iterator(data, error)) {
            auto extension = file.path().extension().wstring();
            for (auto& c : extension) c = static_cast<wchar_t>(std::towlower(c));
            if (extension != L".bsa") continue;
            std::vector<char> isTexture;
            const auto match = [&](std::string_view a_path) {
                isTexture.push_back(a_path.ends_with(".dds"));
                return PathBits(a_path);
            };
            const auto first = paths->size();
            if (!ArchiveTail::AddArchivePaths(file.path(), match, *paths)) {
                ++unread;
                std::error_code sizeError;
                const auto bytes = std::filesystem::file_size(file.path(), sizeError);
                unreadReach = std::max<std::uint64_t>(unreadReach, sizeError ? ~0ull : bytes);
                continue;
            }
            ++archives;
            const auto name = file.path().filename().u8string();
            if (const auto number = archiveNumbers.find(std::string(name.begin(), name.end())); useCache && g_settings.cacheFill && (g_smallOnLoadingScreens.load(std::memory_order_relaxed) || g_smallDuringPlay.load(std::memory_order_relaxed)) && number != archiveNumbers.end()) {
                for (std::size_t i = 0; i < isTexture.size(); ++i) {
                    const auto& entry = (*paths)[first + i];
                    if (isTexture[i] && !(entry.match & kExcludedBit)) fillItems.push_back({number->second, entry.offset, entry.size, entry.match});
                }
            }
        }
        std::sort(paths->begin(), paths->end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
        const auto excluded = std::count_if(paths->begin(), paths->end(), [](const auto& e) { return (e.match & kExcludedBit) != 0; });
        const auto armor = std::count_if(paths->begin(), paths->end(), [](const auto& e) { return e.match == kArmorBit; });
        SKSE::log::info("[stream] read the paths of {} archives ({} without), {} entries, {} excluded, {} more armor and clothing, in {:.0f} ms", archives, unread, paths->size(), excluded, armor,
                        1000.0 * QpcSeconds(Qpc() - namesStarted));
        if (unread != 0) SKSE::log::warn("[stream] the paths of {} archives could not be read, the largest {:.0f} MiB: nameless textures that could be in them load whole", unread, MiB(unreadReach));
        g_unreadPathsReach.store(unreadReach, std::memory_order_release);
        g_excludedPaths.store(paths.release(), std::memory_order_release);
        if (!fillItems.empty()) FillCache(*published, std::move(fillItems));
    }
}

void Streamer::Install() {
    ReadSettings();
    if (g_settings.mode == Mode::kOff) return;

    const auto version = REL::Module::get().version();
    const bool vr = REL::Module::IsVR();
    const auto layout = RuntimePolicy::Verified(vr, version.major(), version.minor(), version.patch(), version.build());
    const auto vtables = RuntimePolicy::VtablesOf(vr, version.major(), version.minor(), version.patch(), version.build());
    if (!layout || !vtables) {
        SKSE::log::warn("[stream] {} has not been checked for texture streaming; textures load as usual", version.string("."sv));
        g_settings.mode = Mode::kOff;
        return;
    }
    g_layout = *layout;
    g_compressedVtable = REL::Module::get().base() + vtables->compressed;
    g_storedVtable = REL::Module::get().base() + vtables->archive;
    {
        const auto base = REL::Module::get().base();
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_gameImageStart = base;
        g_gameImageEnd = base + nt->OptionalHeader.SizeOfImage;
    }

    REL::Relocation<std::uintptr_t> manager{RE::VTABLE_BSShaderResourceManager[0]};
    REL::Relocation<std::uintptr_t> texture{RE::VTABLE_NiSourceTexture[0]};
    constexpr std::size_t kSlot_CreateTexture = 0xD0 / sizeof(void*);
    if (!SkyrimHooks::Publish(reinterpret_cast<void**>(texture.address()), &Hook_Destructor, g_originalDestructor) ||
        !SkyrimHooks::Publish(reinterpret_cast<void**>(manager.address()) + kSlot_CreateTexture, &Hook_CreateTexture, g_originalCreate)) {
        SKSE::log::error("[stream] couldn't hook texture creation; textures load as usual");
        g_settings.mode = Mode::kOff;
        return;
    }
    BeginScreen();
#ifdef FASTER_TEXTURES_BENCH
    if (g_settings.profileQueue) LoadQueueProbe::Install();
#endif
    SKSE::log::info("[stream] measuring texture loads on {}{}", vr ? "VR " : "", version.string("."sv));
    std::thread([] {
        // Nothing may leave this thread: an exception here would end the game.
        try {
            BuildIndex();
        } catch (const std::exception& error) {
            SKSE::log::warn("[stream] {} while reading the archives; textures that could not be looked up load whole", error.what());
        } catch (...) {
            SKSE::log::warn("[stream] the archives could not be read; textures that could not be looked up load whole");
        }
    }).detach();
}

void Streamer::OnDataLoaded() {
    if (g_settings.mode == Mode::kOff) return;

    static MenuSink sink;
    if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);

    auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
    auto* swapChain = window ? reinterpret_cast<IDXGISwapChain*>(window->swapChain) : nullptr;
    if (!swapChain || !Device() || !Context()) {
        SKSE::log::error("[stream] no swap chain or device yet; textures load whole");
        return;
    }
    // When measuring too: it tells which thread renders.
    g_window.store(window, std::memory_order_release);
    if (!SkyrimHooks::Publish(*reinterpret_cast<void***>(swapChain) + kSlot_Present, &Hook_Present, g_originalPresent)) {
        SKSE::log::error("[stream] couldn't hook Present; textures load whole");
        return;
    }
    {
        // Where the hooked table lives and what it continues into: a wrapper's (ENB, ReShade, a frame generator) or dxgi's.
        const auto moduleOf = [](const void* a_address) {
            HMODULE module{};
            wchar_t file[MAX_PATH]{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_address), &module) ||
                !GetModuleFileNameW(module, file, MAX_PATH))
                return std::string("no module");
            return Utf8(std::filesystem::path(file).filename());
        };
        auto* const* slot = *reinterpret_cast<void***>(swapChain) + kSlot_Present;
        MEMORY_BASIC_INFORMATION page{};
        VirtualQuery(slot, &page, sizeof(page));
        SKSE::log::info("[stream] Present hooked in a table of {} (page protection 0x{:X}), continuing into {}", moduleOf(slot), page.Protect,
                        moduleOf(reinterpret_cast<const void*>(g_originalPresent.load(std::memory_order_acquire))));
    }
#ifdef FASTER_TEXTURES_BENCH
    if (g_settings.profile) {
        LoadProfiler::Install(Device(), g_compressedVtable, g_storedVtable, g_layout.startOffset);
    }
#endif
    if (g_settings.mode != Mode::kStream) return;
    g_downscalerInstalled = GetModuleHandleW(L"TextureDownscaler.dll") != nullptr;
    if (g_downscalerInstalled) {
        if (FAILED(Device()->QueryInterface(IID_PPV_ARGS(g_device3.GetAddressOf())))) {
            g_device3.Reset();
            SKSE::log::warn("[stream] the graphics device offers no ID3D11Device3: beside Texture Downscaler, some full textures may arrive smaller than it would make them");
        }
        ReadDownscalerRules();
    }
    // SPS turns its own texture streaming off beside Texture Downscaler, so nothing is handed to it then.
    if (!g_downscalerInstalled && SpsStreams()) {
        ReadSpsExclude();
        g_sharing.store(Sharing::kProbing, std::memory_order_release);  // the two minutes start at the first world load: no scene, no takeover
        SKSE::log::info("[stream] Skyrim Performance Solution's texture streaming is on: textures it can manage are handed to it after loading small");
    }
    REL::Relocation<std::uintptr_t> shader{RE::VTABLE_BSLightingShader[0]};
    if (!SkyrimHooks::Publish(reinterpret_cast<void**>(shader.address()) + kSlot_SetupGeometry, &Hook_SetupGeometry, g_originalSetupGeometry)) {
        SKSE::log::error("[stream] couldn't hook drawing; textures load whole");
        return;
    }
    if (const auto* copier = ArmorCopierLoaded(); copier && !g_settings.armorExclude.empty()) {
        g_wholeBits.store(kExcludedBit | kArmorBit, std::memory_order_release);
        SKSE::log::info("[stream] {} copies armor textures as they load, so armor and clothing load whole", copier);
    }
    for (std::uint32_t w = 0; w < g_settings.upgradeWorkers; ++w) {
        std::thread(Worker, w).detach();
    }
    std::thread(WatchPresent, swapChain).detach();
    g_streaming.store(true, std::memory_order_release);
    SKSE::log::info("[stream] loading textures small first: up to {} top mips, sides of at least {}, {} KiB saved at least, {} workers",
                    g_settings.skipMips, g_settings.minEdge, g_settings.minSavedBytes >> 10, g_settings.upgradeWorkers);
}

void Streamer::LogSummary() {
    if (g_settings.mode == Mode::kOff) return;
    LogLine("session", 0.0, g_session);
}

bool Streamer::SmallFirst(bool a_loadingScreens) {
    return (a_loadingScreens ? g_smallOnLoadingScreens : g_smallDuringPlay).load(std::memory_order_relaxed);
}

void Streamer::SetSmallFirst(bool a_loadingScreens, bool a_on) {
    if ((a_loadingScreens ? g_smallOnLoadingScreens : g_smallDuringPlay).exchange(a_on, std::memory_order_relaxed) == a_on) return;
    SKSE::log::info("[menu] {} now load {}", a_loadingScreens ? "loading screens" : "textures that load during play", a_on ? "small first" : "whole");
    {
        std::scoped_lock lock(g_choicesLock);
        (a_loadingScreens ? g_pendingChoices.onLoadingScreens : g_pendingChoices.duringPlay) = a_on;
    }
    SaveChoicesLater();
}

Streamer::CacheChoice Streamer::SavedCacheChoice() {
    std::scoped_lock lock(g_choicesLock);
    if (g_pendingChoices.cache) return *g_pendingChoices.cache;
    CSimpleIniA mcm;
    mcm.SetUnicode();
    mcm.LoadFile(kChoicesFile);
    CacheChoice choice;
    if (const auto location = mcm.GetLongValue("Streamer", "iCacheLocation", -1); location >= 0) {
        choice.location = static_cast<int>(std::min<long>(location, 2));
        choice.folder = mcm.GetValue("Streamer", "sCacheFolder", "");
        return choice;
    }
    // No choice made in a menu: the SKSE ini's folder, if it names one.
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile("Data/SKSE/Plugins/FasterTextures.ini");
    choice.folder = ini.GetValue("Streamer", "CacheFolder", "");
    choice.location = choice.folder.empty() ? 0 : 2;
    return choice;
}

void Streamer::SetCacheChoice(const CacheChoice& a_choice) {
    SKSE::log::info("[menu] cache location set to {}{}; applies after a restart", a_choice.location == 1 ? "local app data" : a_choice.location == 2 ? "a folder: " : "beside the game",
                    a_choice.location == 2 ? a_choice.folder : std::string());
    {
        std::scoped_lock lock(g_choicesLock);
        g_pendingChoices.cache = a_choice;
    }
    SaveChoicesLater();
}

std::string Streamer::Status() {
    std::string status;
    if (g_settings.mode == Mode::kOff) status = "Off: Mode=0 in Data\\SKSE\\Plugins\\FasterTextures.ini.";
    else if (g_settings.mode == Mode::kMeasure) status = "Measuring only: Mode=1 in Data\\SKSE\\Plugins\\FasterTextures.ini, textures load whole.";
    else if (!g_streaming.load(std::memory_order_acquire)) status = "Small first did not start this session; FasterTextures.log says why.";
    else if (g_presentLost.load(std::memory_order_acquire)) status = "Small first stopped: the game no longer presents through the hooked swap chain; see FasterTextures.log.";
    else status = "Loading textures small first.";
    if (g_downscaler.load(std::memory_order_acquire)) status += "\nTexture Downscaler: full textures arrive at the sizes it gives them.";
    const auto sharing = g_sharing.load(std::memory_order_acquire);
    if (sharing == Sharing::kProbing || sharing == Sharing::kConfirmed) status += "\nSkyrim Performance Solution streams textures: those it can manage are handed to it.";
    if (const auto folder = g_cacheFolderInUse.load(std::memory_order_acquire)) status += "\nSmall mips cache: " + *folder;
    return status;
}
