#pragma once

// Measures the game's file-loading queue while cells load, with [Streamer] ProfileQueue=1. AE 1.7.104 only.
//
// IOManager::FlushTasks (140fc6be0) runs on the main thread when a cell loads. It first re-files every queued
// task in the task map under a new key (the pass at 140fc6ea0, called from 140fc6cd2), retrying a taken key
// with the next one (IOManager::Func18, its call of BSTaskManager::Func18 at 140fc6b76), and then sleeps in
// 1-50 ms steps until the loader threads have run every task. A loading profile put 22% of loading CPU in
// that map's lookups. Whether it costs loading time depends on which part ends last: if the wait after the
// pass is short, the pass holds loading up; if it is long, the loader threads do.
//
// Each wrapper is installed only where the bytes are a call to the expected function, so another runtime or
// another mod's patch leaves it out. Measurement only: the game's own functions run unchanged.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace LoadQueueProbe {
    namespace detail {
        inline std::int64_t Qpc() noexcept {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return now.QuadPart;
        }

        inline double Seconds(std::int64_t a_ticks) noexcept {
            static const double frequency = [] {
                LARGE_INTEGER value;
                QueryPerformanceFrequency(&value);
                return static_cast<double>(value.QuadPart);
            }();
            return static_cast<double>(a_ticks) / frequency;
        }

        struct Figures {
            std::atomic<std::uint64_t> flushes{0}, flushTicks{0}, longestFlush{0}, passTicks{0}, passWhenLongest{0};
            std::atomic<std::uint64_t> filings{0}, filingsInPass{0}, retries{0}, retriesInPass{0}, insertTicksInPass{0};
            std::atomic<std::uint64_t> waitShort{0}, waitLong{0};  // flushes whose wait after the pass was under / over 50 ms

            void Reset() {
                for (auto* value : {&flushes, &flushTicks, &longestFlush, &passTicks, &passWhenLongest, &filings, &filingsInPass, &retries, &retriesInPass, &insertTicksInPass, &waitShort, &waitLong})
                    value->store(0, std::memory_order_relaxed);
            }
        };

        inline Figures g_figures;
        inline bool g_installed = false;
        inline std::atomic<void*> g_manager{nullptr};

        inline thread_local bool t_inPass = false;
        inline thread_local std::int64_t t_passTicks = 0;

        using Flush_t = void (*)(void*, std::int32_t);
        using Pass_t = void (*)(void*, std::int32_t, std::int32_t);
        using Insert_t = std::uint64_t (*)(void*, void*, void*, void*);
        inline Flush_t g_flush = nullptr;
        inline Pass_t g_pass = nullptr;
        inline Insert_t g_insert = nullptr;

        inline void Max(std::atomic<std::uint64_t>& a_value, std::uint64_t a_candidate) {
            auto current = a_value.load(std::memory_order_relaxed);
            while (a_candidate > current && !a_value.compare_exchange_weak(current, a_candidate, std::memory_order_relaxed)) {}
        }

        inline void Flush(void* a_manager, std::int32_t a_level) {
            g_manager.store(a_manager, std::memory_order_relaxed);
            const auto started = Qpc();
            t_passTicks = 0;
            g_flush(a_manager, a_level);
            const auto ticks = static_cast<std::uint64_t>(Qpc() - started);
            const auto pass = static_cast<std::uint64_t>(t_passTicks);
            auto& f = g_figures;
            f.flushes.fetch_add(1, std::memory_order_relaxed);
            f.flushTicks.fetch_add(ticks, std::memory_order_relaxed);
            ((Seconds(static_cast<std::int64_t>(ticks - std::min(ticks, pass))) < 0.05) ? f.waitShort : f.waitLong).fetch_add(1, std::memory_order_relaxed);
            if (ticks > f.longestFlush.load(std::memory_order_relaxed)) {
                f.longestFlush.store(ticks, std::memory_order_relaxed);
                f.passWhenLongest.store(pass, std::memory_order_relaxed);
            }
        }

        inline void Pass(void* a_manager, std::int32_t a_level, std::int32_t a_from) {
            const auto started = Qpc();
            t_inPass = true;
            g_pass(a_manager, a_level, a_from);
            t_inPass = false;
            const auto ticks = Qpc() - started;
            t_passTicks += ticks;
            g_figures.passTicks.fetch_add(static_cast<std::uint64_t>(ticks), std::memory_order_relaxed);
        }

        inline std::uint64_t Insert(void* a_tasks, void* a_task, void* a_3, void* a_4) {
            const auto started = t_inPass ? Qpc() : 0;
            const auto result = g_insert(a_tasks, a_task, a_3, a_4);
            auto& f = g_figures;
            const bool filed = (result & 0xFF) != 0;
            (filed ? f.filings : f.retries).fetch_add(1, std::memory_order_relaxed);
            if (t_inPass) {
                (filed ? f.filingsInPass : f.retriesInPass).fetch_add(1, std::memory_order_relaxed);
                f.insertTicksInPass.fetch_add(static_cast<std::uint64_t>(Qpc() - started), std::memory_order_relaxed);
            }
            return result;
        }

        // A 5-byte call at a_site to a_target, or null.
        inline bool CallsTo(std::uintptr_t a_site, std::uintptr_t a_target) {
            const auto* code = reinterpret_cast<const std::uint8_t*>(a_site);
            if (code[0] != 0xE8) return false;
            std::int32_t offset;
            std::memcpy(&offset, code + 1, 4);
            return a_site + 5 + static_cast<std::intptr_t>(offset) == a_target;
        }
    }

    inline void Install() {
        using namespace detail;
        if (REL::Module::IsVR() || REL::Module::get().version() != REL::Version{1, 7, 104, 0}) {
            SKSE::log::warn("[queue] the loading-queue probe knows only AE 1.7.104; not installed");
            return;
        }
        const auto base = REL::Module::get().base();
        const auto flush = base + 0xFC6BE0, pass = base + 0xFC6EA0, insert = base + 0xFC2C50;
        constexpr std::array<std::uint32_t, 16> kFlushSites{0x615D01, 0x1AAFA2, 0x1AAFD7, 0x1A2F6B, 0x1A2FA0, 0x1A3540, 0x1A3575, 0x1A4217,
                                                           0x743942, 0x75EF46, 0x1C3766, 0x1C4C05, 0x2A271A, 0x654243, 0x65B77D, 0x65E5D9};
        constexpr std::uint32_t kPassSite = 0xFC6CD2, kInsertSite = 0xFC6B76;
        if (!CallsTo(base + kPassSite, pass) || !CallsTo(base + kInsertSite, insert)) {
            SKSE::log::warn("[queue] the loading queue's code is not as expected (another mod's patch?); probe not installed");
            return;
        }
        static SKSE::Trampoline trampoline{"FasterTextures.QueueProbe"};
        trampoline.create(14 * (kFlushSites.size() + 2) + 64);
        g_flush = reinterpret_cast<Flush_t>(flush);
        g_pass = reinterpret_cast<Pass_t>(pass);
        g_insert = reinterpret_cast<Insert_t>(insert);
        trampoline.write_call<5>(base + kPassSite, &Pass);
        trampoline.write_call<5>(base + kInsertSite, &Insert);
        std::size_t sites = 0;
        for (const auto rva : kFlushSites) {
            if (!CallsTo(base + rva, flush)) continue;
            trampoline.write_call<5>(base + rva, &Flush);
            ++sites;
        }
        g_installed = true;
        SKSE::log::info("[queue] loading-queue probe installed at {} of {} FlushTasks calls", sites, kFlushSites.size());
    }

    // Tasks the loading queue holds now, or -1 before its first FlushTasks: the count FlushTasks itself asks for (vtable +0x80).
    inline int QueueDepth() {
        auto* manager = detail::g_manager.load(std::memory_order_relaxed);
        if (!manager) return -1;
        using Count_t = std::int32_t (*)(void*);
        return (*reinterpret_cast<Count_t**>(manager))[0x80 / sizeof(void*)](manager);
    }

    // For the stretch since the last call: a loading screen, or play between two.
    inline void Log(const char* a_title) {
        using namespace detail;
        if (!g_installed) return;
        auto& f = g_figures;
        const auto u = [](const std::atomic<std::uint64_t>& a_value) { return a_value.load(std::memory_order_relaxed); };
        if (u(f.flushes) != 0 || u(f.filings) != 0) {
            const auto flushS = Seconds(static_cast<std::int64_t>(u(f.flushTicks))), passS = Seconds(static_cast<std::int64_t>(u(f.passTicks)));
            SKSE::log::info("[queue] [{}] FlushTasks {} calls, {:.3f} s: re-filing pass {:.3f} s ({:.3f} s in inserts), waiting {:.3f} s; longest flush {:.3f} s, its pass {:.3f} s; "
                            "flushes waiting under 50 ms after the pass {}, over {} | task inserts: {} filed + {} retried, of them in the pass {} + {}",
                            a_title, u(f.flushes), flushS, passS, Seconds(static_cast<std::int64_t>(u(f.insertTicksInPass))), flushS - passS,
                            Seconds(static_cast<std::int64_t>(u(f.longestFlush))), Seconds(static_cast<std::int64_t>(u(f.passWhenLongest))), u(f.waitShort), u(f.waitLong),
                            u(f.filings), u(f.retries), u(f.filingsInPass), u(f.retriesInPass));
        }
        f.Reset();
    }
}
