#pragma once

// In-process loading-screen profiler for Mode=1/2 with [Streamer] Profile=1:
// 1. 1 ms high-resolution stack & CPU-cycle sampler across all game threads during loading screens,
//    using lock-free pre-cached PE .pdata tables + RtlVirtualUnwind (never touches the loader lock).
// 2. ID3D11Device::CreateBuffer vtable hook: exact counts, bytes, and ms for vertex, index,
//    constant, and other GPU buffers created during loading screens.
// 3. CompressedArchiveStream::DoRead and ArchiveStream::DoRead hooks: exact counts, bytes, and ms
//    for non-texture archive reads during loading screens, classified by file header magic
//    (NIF/BTR meshes, HKX animations, TRI morphs, Audio, DDS outside CreateTexture, PEX, Other).

#include "HookPublication.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace LoadProfiler {
    inline constexpr std::size_t kMaxDepth = 14;

    enum BufKind : int { kBufVertex = 0, kBufIndex = 1, kBufConstant = 2, kBufOther = 3, kBufCount = 4 };
    inline constexpr std::array<const char*, kBufCount> kBufNames{"vertex", "index", "constant", "other"};

    enum FileKind : int {
        kFileNif = 0,
        kFileHkx = 1,
        kFileTri = 2,
        kFileAudio = 3,
        kFileDds = 4,
        kFilePex = 5,
        kFileOther = 6,
        kFileCount = 7
    };
    inline constexpr std::array<const char*, kFileCount> kFileNames{"nif/mesh", "hkx", "tri", "audio", "dds-other", "pex", "other"};

    struct ModuleEntry {
        std::uintptr_t base = 0;
        std::uint32_t size = 0;
        std::uint8_t index = 0;
        const IMAGE_RUNTIME_FUNCTION_ENTRY* pdata = nullptr;
        std::uint32_t pdataCount = 0;
        char name[48]{};
    };

    struct Sample {
        std::uint32_t tid = 0;
        std::uint8_t screen = 0;
        std::uint8_t onCpu = 0;
        std::uint8_t depth = 0;
        std::uint8_t pad = 0;
        std::uint64_t cycles = 0;
        std::uint8_t modIdx[kMaxDepth]{};
        std::uint32_t funcRva[kMaxDepth]{};
    };

    struct CounterSet {
        std::atomic<std::uint64_t> bufCount[kBufCount]{}, bufBytes[kBufCount]{}, bufTicks[kBufCount]{};
        std::atomic<std::uint64_t> fileCount[kFileCount]{}, readCalls[kFileCount]{}, readBytes[kFileCount]{}, readTicks[kFileCount]{};
        std::atomic<std::uint64_t> compReadTicks{0}, storedReadTicks{0}, nonTexReadThreadCount{0};

        void Reset() {
            for (int i = 0; i < kBufCount; ++i) {
                bufCount[i].store(0, std::memory_order_relaxed);
                bufBytes[i].store(0, std::memory_order_relaxed);
                bufTicks[i].store(0, std::memory_order_relaxed);
            }
            for (int i = 0; i < kFileCount; ++i) {
                fileCount[i].store(0, std::memory_order_relaxed);
                readCalls[i].store(0, std::memory_order_relaxed);
                readBytes[i].store(0, std::memory_order_relaxed);
                readTicks[i].store(0, std::memory_order_relaxed);
            }
            compReadTicks.store(0, std::memory_order_relaxed);
            storedReadTicks.store(0, std::memory_order_relaxed);
            nonTexReadThreadCount.store(0, std::memory_order_relaxed);
        }
    };

    namespace detail {
        inline std::atomic<bool> g_enabled{false};
        inline std::atomic<bool> g_active{false};
        inline std::atomic<std::uint32_t> g_screenIndex{0};
        inline std::atomic<std::uint32_t> g_mainTid{0}, g_renderTid{0}, g_texLoaderTid{0};
        inline std::size_t g_startOffsetField = 0x20;
        inline CounterSet g_screenCounters, g_routeCounters;
        inline double g_totalScreenSeconds = 0.0;

        inline std::vector<ModuleEntry> g_modules;
        inline std::vector<Sample> g_samples;
        inline std::mutex g_reportMutex;

        inline thread_local bool t_inTextureCreate = false;
        inline thread_local const void* t_lastStream = nullptr;
        inline thread_local std::uint32_t t_lastStart = 0;
        inline thread_local int t_lastKind = kFileOther;

        inline std::int64_t Qpc() noexcept {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return now.QuadPart;
        }

        inline double QpcSeconds(std::int64_t a_ticks) noexcept {
            static const double freq = [] {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return static_cast<double>(f.QuadPart);
            }();
            return static_cast<double>(a_ticks) / freq;
        }

        inline double MiB(std::uint64_t a_bytes) noexcept {
            return static_cast<double>(a_bytes) / (1024.0 * 1024.0);
        }

        inline const ModuleEntry* FindModule(std::uintptr_t a_pc) noexcept {
            const auto* begin = g_modules.data();
            const auto* end = begin + g_modules.size();
            const auto* it = std::upper_bound(begin, end, a_pc, [](std::uintptr_t pc, const ModuleEntry& m) noexcept {
                return pc < m.base;
            });
            if (it == begin) return nullptr;
            --it;
            return (a_pc - it->base) < it->size ? it : nullptr;
        }

        inline const IMAGE_RUNTIME_FUNCTION_ENTRY* FindFunction(const ModuleEntry& a_mod, std::uint32_t a_rva) noexcept {
            if (!a_mod.pdata || a_mod.pdataCount == 0) return nullptr;
            const auto* begin = a_mod.pdata;
            const auto* end = begin + a_mod.pdataCount;
            const auto* it = std::upper_bound(begin, end, a_rva, [](std::uint32_t rva, const IMAGE_RUNTIME_FUNCTION_ENTRY& f) noexcept {
                return rva < f.BeginAddress;
            });
            if (it == begin) return nullptr;
            --it;
            return a_rva < it->EndAddress ? it : nullptr;
        }

        __declspec(noinline) inline std::uint8_t UnwindStackSEH(CONTEXT* a_ctx, std::uint8_t* a_modIdx, std::uint32_t* a_funcRva) noexcept {
            std::uint8_t depth = 0;
            __try {
                for (; depth < kMaxDepth; ++depth) {
                    const DWORD64 rip = a_ctx->Rip;
                    const DWORD64 rsp = a_ctx->Rsp;
                    if (rip < 0x10000 || rsp < 0x10000 || (rsp & 7) != 0) break;
                    const DWORD64 lookupPc = depth == 0 ? rip : rip - 1;
                    const ModuleEntry* mod = FindModule(static_cast<std::uintptr_t>(lookupPc));
                    if (!mod) break;
                    const auto rva = static_cast<std::uint32_t>(lookupPc - mod->base);
                    const IMAGE_RUNTIME_FUNCTION_ENTRY* fn = FindFunction(*mod, rva);
                    a_modIdx[depth] = mod->index;
                    a_funcRva[depth] = fn ? fn->BeginAddress : rva;
                    if (fn) {
                        PVOID handlerData = nullptr;
                        DWORD64 establisherFrame = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, mod->base, lookupPc,
                                         const_cast<PIMAGE_RUNTIME_FUNCTION_ENTRY>(fn),
                                         a_ctx, &handlerData, &establisherFrame, nullptr);
                    } else if (depth == 0) {
                        a_ctx->Rip = *reinterpret_cast<const DWORD64*>(rsp);
                        a_ctx->Rsp = rsp + 8;
                    } else {
                        break;
                    }
                    if (a_ctx->Rip == 0 || (depth > 0 && a_ctx->Rsp <= rsp)) break;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            return depth;
        }

        // ── Hooks ────────────────────────────────────────────────────────────

        using CreateBuffer_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
        inline std::atomic<CreateBuffer_t> g_origCreateBuffer{nullptr};

        inline HRESULT STDMETHODCALLTYPE Hook_CreateBuffer(ID3D11Device* a_device, const D3D11_BUFFER_DESC* a_desc,
                                                           const D3D11_SUBRESOURCE_DATA* a_initial, ID3D11Buffer** a_out) {
            const auto orig = g_origCreateBuffer.load(std::memory_order_acquire);
            if (!g_active.load(std::memory_order_relaxed)) {
                return orig(a_device, a_desc, a_initial, a_out);
            }
            const auto t0 = Qpc();
            const HRESULT hr = orig(a_device, a_desc, a_initial, a_out);
            const auto ticks = static_cast<std::uint64_t>(Qpc() - t0);
            const UINT bind = a_desc ? a_desc->BindFlags : 0;
            const int kind = (bind & D3D11_BIND_VERTEX_BUFFER)     ? kBufVertex :
                             (bind & D3D11_BIND_INDEX_BUFFER)      ? kBufIndex :
                             (bind & D3D11_BIND_CONSTANT_BUFFER)   ? kBufConstant :
                                                                     kBufOther;
            const std::uint64_t bytes = a_desc ? a_desc->ByteWidth : 0;
            for (auto* c : {&g_screenCounters, &g_routeCounters}) {
                c->bufCount[kind].fetch_add(1, std::memory_order_relaxed);
                c->bufBytes[kind].fetch_add(bytes, std::memory_order_relaxed);
                c->bufTicks[kind].fetch_add(ticks, std::memory_order_relaxed);
            }
            return hr;
        }

        using DoRead_t = RE::BSResource::ErrorCode (*)(const void*, void*, std::uint64_t, std::uint64_t&);
        inline std::atomic<DoRead_t> g_origCompDoRead{nullptr}, g_origStoredDoRead{nullptr};

        inline int ClassifyMagic(const void* a_buffer, std::uint64_t a_read) noexcept {
            if (!a_buffer || a_read < 4) return kFileOther;
            std::uint32_t magic = 0;
            std::memcpy(&magic, a_buffer, 4);
            if (magic == 0x656D6147u || magic == 0x4974654Eu || magic == 0x20525442u || magic == 0x204F5442u) return kFileNif;  // "Game", "NetI", "BTR ", "BTO "
            if (magic == 0x57E0E057u) return kFileHkx;
            if (magic == 0x52545246u) return kFileTri;  // "FRTR"
            if (magic == 0x455A5546u || magic == 0x46464952u || magic == 0x204D5758u) return kFileAudio;  // "FUZE", "RIFF", "XWM "
            if (magic == 0x20534444u) return kFileDds;  // "DDS "
            if (magic == 0xDEC057FAu || magic == 0xFA57C0DEu) return kFilePex;
            return kFileOther;
        }

        inline RE::BSResource::ErrorCode RecordNonTexRead(const void* a_this, void* a_buffer, std::uint64_t a_toRead,
                                                          std::uint64_t& a_read, DoRead_t a_orig, bool a_compressed) {
            if (!g_active.load(std::memory_order_relaxed) || t_inTextureCreate) {
                return a_orig(a_this, a_buffer, a_toRead, a_read);
            }
            const auto t0 = Qpc();
            const auto rc = a_orig(a_this, a_buffer, a_toRead, a_read);
            const auto ticks = static_cast<std::uint64_t>(Qpc() - t0);
            if (rc == RE::BSResource::ErrorCode::kNone && a_read > 0) {
                const auto start = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(a_this) + g_startOffsetField);
                bool newFile = false;
                if (a_this != t_lastStream || start != t_lastStart) {
                    t_lastStream = a_this;
                    t_lastStart = start;
                    t_lastKind = ClassifyMagic(a_buffer, a_read);
                    newFile = true;
                }
                const int kind = t_lastKind;
                for (auto* c : {&g_screenCounters, &g_routeCounters}) {
                    if (newFile) c->fileCount[kind].fetch_add(1, std::memory_order_relaxed);
                    c->readCalls[kind].fetch_add(1, std::memory_order_relaxed);
                    c->readBytes[kind].fetch_add(a_read, std::memory_order_relaxed);
                    c->readTicks[kind].fetch_add(ticks, std::memory_order_relaxed);
                    (a_compressed ? c->compReadTicks : c->storedReadTicks).fetch_add(ticks, std::memory_order_relaxed);
                }
            }
            return rc;
        }

        inline RE::BSResource::ErrorCode Hook_CompDoRead(const void* a_this, void* a_buffer, std::uint64_t a_toRead, std::uint64_t& a_read) {
            return RecordNonTexRead(a_this, a_buffer, a_toRead, a_read, g_origCompDoRead.load(std::memory_order_acquire), true);
        }

        inline RE::BSResource::ErrorCode Hook_StoredDoRead(const void* a_this, void* a_buffer, std::uint64_t a_toRead, std::uint64_t& a_read) {
            return RecordNonTexRead(a_this, a_buffer, a_toRead, a_read, g_origStoredDoRead.load(std::memory_order_acquire), false);
        }

        // ── Module table & symbol resolution ─────────────────────────────────

        inline void SnapshotModules() {
            g_modules.clear();
            HMODULE mods[512]{};
            DWORD needed = 0;
            const HANDLE proc = GetCurrentProcess();
            if (!K32EnumProcessModules(proc, mods, sizeof(mods), &needed)) return;
            const std::size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), 250);
            g_modules.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto base = reinterpret_cast<std::uintptr_t>(mods[i]);
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
                ModuleEntry entry{};
                entry.base = base;
                entry.size = nt->OptionalHeader.SizeOfImage;
                const auto& exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (exc.VirtualAddress != 0 && exc.Size >= sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) {
                    entry.pdata = reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(base + exc.VirtualAddress);
                    entry.pdataCount = exc.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
                }
                K32GetModuleBaseNameA(proc, mods[i], entry.name, sizeof(entry.name) - 1);
                g_modules.push_back(entry);
            }
            std::sort(g_modules.begin(), g_modules.end(), [](const ModuleEntry& a, const ModuleEntry& b) {
                return a.base < b.base;
            });
            for (std::size_t i = 0; i < g_modules.size(); ++i) {
                g_modules[i].index = static_cast<std::uint8_t>(i);
            }
        }

        struct ThreadTrack {
            std::uint32_t tid = 0;
            HANDLE handle = nullptr;
            std::uint64_t lastCycles = 0;
            std::uint64_t totalCycles = 0;
            std::uint64_t activeSamples = 0;
            std::uint64_t waitSamples = 0;
            std::uint64_t user100ns = 0;
            std::uint64_t kernel100ns = 0;
            std::uint64_t screenStartUser = 0;
            std::uint64_t screenStartKernel = 0;
        };

        inline std::uint64_t FileTimeToU64(const FILETIME& a_ft) noexcept {
            return (static_cast<std::uint64_t>(a_ft.dwHighDateTime) << 32) | a_ft.dwLowDateTime;
        }

        inline void RefreshThreads(std::unordered_map<std::uint32_t, ThreadTrack>& a_threads, std::uint32_t a_selfTid) {
            const DWORD pid = GetCurrentProcessId();
            const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == a_selfTid) continue;
                    auto it = a_threads.find(te.th32ThreadID);
                    if (it == a_threads.end()) {
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                        if (!h) continue;
                        ThreadTrack tr{};
                        tr.tid = te.th32ThreadID;
                        tr.handle = h;
                        ULONG64 cyc = 0;
                        QueryThreadCycleTime(h, &cyc);
                        tr.lastCycles = cyc;
                        FILETIME c{}, e{}, k{}, u{};
                        if (GetThreadTimes(h, &c, &e, &k, &u)) {
                            tr.screenStartKernel = FileTimeToU64(k);
                            tr.screenStartUser = FileTimeToU64(u);
                        }
                        a_threads.emplace(tr.tid, tr);
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }

        inline std::string ResolveSymbolName(std::uint8_t a_modIdx, std::uint32_t a_funcRva) {
            if (a_modIdx >= g_modules.size()) return "unknown";
            const auto& mod = g_modules[a_modIdx];
            const DWORD64 addr = mod.base + a_funcRva;
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 250;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym) && sym->NameLen > 0) {
                return std::format("{}!{}+0x{:X} (rva 0x{:X})", mod.name, std::string_view(sym->Name, sym->NameLen), disp, a_funcRva);
            }
            return std::format("{}+0x{:X}", mod.name, a_funcRva);
        }

        inline const char* ThreadRole(std::uint32_t a_tid) noexcept {
            const auto mainTid = g_mainTid.load(std::memory_order_relaxed);
            const auto renderTid = g_renderTid.load(std::memory_order_relaxed);
            const auto texTid = g_texLoaderTid.load(std::memory_order_relaxed);
            if (a_tid == mainTid && a_tid == renderTid) return "Main+Render";
            if (a_tid == mainTid) return "Main";
            if (a_tid == renderTid) return "Render";
            if (a_tid == texTid) return "TexLoader";
            return "Worker";
        }

        inline void WriteProfileReport(const std::unordered_map<std::uint32_t, ThreadTrack>& a_threads) {
            std::scoped_lock lock(g_reportMutex);
            auto dir = SKSE::log::log_directory();
            if (!dir) return;
            const auto outPath = *dir / "FasterTexturesProfile.txt";
            std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
            if (!out) return;

            static bool symInit = false;
            if (!symInit) {
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                SymInitialize(GetCurrentProcess(), nullptr, TRUE);
                symInit = true;
            }

            std::unordered_map<std::uint64_t, std::string> symCache;
            const auto symOf = [&](std::uint8_t modIdx, std::uint32_t rva) -> const std::string& {
                const std::uint64_t key = (static_cast<std::uint64_t>(modIdx) << 32) | rva;
                auto it = symCache.find(key);
                if (it != symCache.end()) return it->second;
                return symCache.emplace(key, ResolveSymbolName(modIdx, rva)).first->second;
            };

            out << std::format("=== Faster Textures Loading Screen Profile ===\n");
            out << std::format("Screens profiled: {}, total loading-screen wall time: {:.2f} s, total stack samples: {}\n\n",
                               g_screenIndex.load(), g_totalScreenSeconds, g_samples.size());

            // 1. D3D11 CreateBuffer summary
            out << "== D3D11 CreateBuffer during loading screens ==\n";
            std::uint64_t totalBufCount = 0, totalBufBytes = 0, totalBufTicks = 0;
            for (int i = 0; i < kBufCount; ++i) {
                const auto cnt = g_routeCounters.bufCount[i].load();
                const auto bytes = g_routeCounters.bufBytes[i].load();
                const auto ticks = g_routeCounters.bufTicks[i].load();
                totalBufCount += cnt;
                totalBufBytes += bytes;
                totalBufTicks += ticks;
                out << std::format("  {:10}: {:6} buffers, {:7.2f} MiB, {:6.3f} s ({:5.2f} % of screen time)\n",
                                   kBufNames[i], cnt, MiB(bytes), QpcSeconds(static_cast<std::int64_t>(ticks)),
                                   g_totalScreenSeconds > 0 ? 100.0 * QpcSeconds(static_cast<std::int64_t>(ticks)) / g_totalScreenSeconds : 0.0);
            }
            out << std::format("  {:10}: {:6} buffers, {:7.2f} MiB, {:6.3f} s ({:5.2f} % of screen time)\n\n",
                               "TOTAL", totalBufCount, MiB(totalBufBytes), QpcSeconds(static_cast<std::int64_t>(totalBufTicks)),
                               g_totalScreenSeconds > 0 ? 100.0 * QpcSeconds(static_cast<std::int64_t>(totalBufTicks)) / g_totalScreenSeconds : 0.0);

            // 2. Non-texture archive DoRead summary
            out << "== Non-texture archive DoRead (read + LZ4/zlib unpack) during loading screens ==\n";
            std::uint64_t totalFiles = 0, totalCalls = 0, totalBytes = 0, totalTicks = 0;
            for (int i = 0; i < kFileCount; ++i) {
                const auto files = g_routeCounters.fileCount[i].load();
                const auto calls = g_routeCounters.readCalls[i].load();
                const auto bytes = g_routeCounters.readBytes[i].load();
                const auto ticks = g_routeCounters.readTicks[i].load();
                totalFiles += files;
                totalCalls += calls;
                totalBytes += bytes;
                totalTicks += ticks;
                out << std::format("  {:10}: {:6} files ({:6} reads), {:7.2f} MiB, {:6.3f} s ({:5.2f} % of screen time)\n",
                                   kFileNames[i], files, calls, MiB(bytes), QpcSeconds(static_cast<std::int64_t>(ticks)),
                                   g_totalScreenSeconds > 0 ? 100.0 * QpcSeconds(static_cast<std::int64_t>(ticks)) / g_totalScreenSeconds : 0.0);
            }
            out << std::format("  {:10}: {:6} files ({:6} reads), {:7.2f} MiB, {:6.3f} s (compressed {:.3f} s, stored {:.3f} s)\n\n",
                               "TOTAL", totalFiles, totalCalls, MiB(totalBytes), QpcSeconds(static_cast<std::int64_t>(totalTicks)),
                               QpcSeconds(static_cast<std::int64_t>(g_routeCounters.compReadTicks.load())),
                               QpcSeconds(static_cast<std::int64_t>(g_routeCounters.storedReadTicks.load())));

            // 3. Thread summary sorted by total CPU time
            out << "== Threads active during loading screens ==\n";
            std::vector<ThreadTrack> sortedThreads;
            for (const auto& [tid, tr] : a_threads) {
                if (tr.activeSamples > 0 || tr.user100ns + tr.kernel100ns > 0 || tid == g_mainTid.load() || tid == g_renderTid.load()) {
                    sortedThreads.push_back(tr);
                }
            }
            std::sort(sortedThreads.begin(), sortedThreads.end(), [](const ThreadTrack& a, const ThreadTrack& b) {
                return (a.user100ns + a.kernel100ns) > (b.user100ns + b.kernel100ns);
            });
            for (const auto& tr : sortedThreads) {
                const double userSec = static_cast<double>(tr.user100ns) / 1e7;
                const double kernSec = static_cast<double>(tr.kernel100ns) / 1e7;
                out << std::format("  tid {:6} ({:11}): CPU {:.3f} s (user {:.3f} s + kernel {:.3f} s), {:.2f} Gcycles, active samples {:5}, idle/wait samples {:5}\n",
                                   tr.tid, ThreadRole(tr.tid), userSec + kernSec, userSec, kernSec,
                                   static_cast<double>(tr.totalCycles) / 1e9, tr.activeSamples, tr.waitSamples);
            }
            out << "\n";

            // 4. Function breakdown: (A) All threads on-CPU by cycles & samples, (B) Main thread on-CPU, (C) Main thread off-CPU wait, (D) TexLoader/IOManager thread on-CPU
            struct FuncStat {
                std::uint64_t exclSamples = 0, inclSamples = 0;
                std::uint64_t exclCycles = 0, inclCycles = 0;
            };

            const auto dumpGroup = [&](const char* a_title, auto a_filter, bool a_byCycles) {
                std::unordered_map<std::uint64_t, FuncStat> stats;
                std::map<std::vector<std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>> stacks;
                std::uint64_t totalSamples = 0, totalCycles = 0;

                for (const auto& s : g_samples) {
                    if (!a_filter(s) || s.depth == 0) continue;
                    ++totalSamples;
                    totalCycles += s.cycles;
                    std::uint64_t seen[kMaxDepth]{};
                    std::size_t seenCount = 0;
                    std::vector<std::uint64_t> stackKey;
                    stackKey.reserve(s.depth);
                    for (std::uint8_t d = 0; d < s.depth; ++d) {
                        const std::uint64_t key = (static_cast<std::uint64_t>(s.modIdx[d]) << 32) | s.funcRva[d];
                        stackKey.push_back(key);
                        if (d == 0) {
                            stats[key].exclSamples += 1;
                            stats[key].exclCycles += s.cycles;
                        }
                        bool dup = false;
                        for (std::size_t k = 0; k < seenCount; ++k) {
                            if (seen[k] == key) { dup = true; break; }
                        }
                        if (!dup) {
                            seen[seenCount++] = key;
                            stats[key].inclSamples += 1;
                            stats[key].inclCycles += s.cycles;
                        }
                    }
                    auto& st = stacks[stackKey];
                    st.first += 1;
                    st.second += s.cycles;
                }

                out << std::format("== {} (samples: {}, Gcycles: {:.2f}) ==\n", a_title, totalSamples, static_cast<double>(totalCycles) / 1e9);
                if (totalSamples == 0) {
                    out << "  (no samples)\n\n";
                    return;
                }

                std::vector<std::pair<std::uint64_t, FuncStat>> rows(stats.begin(), stats.end());
                std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
                    return a_byCycles ? (a.second.inclCycles > b.second.inclCycles) : (a.second.inclSamples > b.second.inclSamples);
                });

                out << "  -- Top Inclusive Functions --\n";
                for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 45); ++i) {
                    const auto key = rows[i].first;
                    const auto& st = rows[i].second;
                    const auto modIdx = static_cast<std::uint8_t>(key >> 32);
                    const auto rva = static_cast<std::uint32_t>(key);
                    const double inclPct = a_byCycles && totalCycles > 0 ? 100.0 * static_cast<double>(st.inclCycles) / static_cast<double>(totalCycles)
                                                                         : 100.0 * static_cast<double>(st.inclSamples) / static_cast<double>(totalSamples);
                    const double exclPct = a_byCycles && totalCycles > 0 ? 100.0 * static_cast<double>(st.exclCycles) / static_cast<double>(totalCycles)
                                                                         : 100.0 * static_cast<double>(st.exclSamples) / static_cast<double>(totalSamples);
                    out << std::format("    incl {:5.1f}% ({:5} smp) | excl {:5.1f}% ({:5} smp) | {}\n",
                                       inclPct, st.inclSamples, exclPct, st.exclSamples, symOf(modIdx, rva));
                }

                std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
                    return a_byCycles ? (a.second.exclCycles > b.second.exclCycles) : (a.second.exclSamples > b.second.exclSamples);
                });
                out << "  -- Top Exclusive (Leaf) Functions --\n";
                for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 30); ++i) {
                    const auto key = rows[i].first;
                    const auto& st = rows[i].second;
                    if (st.exclSamples == 0) break;
                    const auto modIdx = static_cast<std::uint8_t>(key >> 32);
                    const auto rva = static_cast<std::uint32_t>(key);
                    const double exclPct = a_byCycles && totalCycles > 0 ? 100.0 * static_cast<double>(st.exclCycles) / static_cast<double>(totalCycles)
                                                                         : 100.0 * static_cast<double>(st.exclSamples) / static_cast<double>(totalSamples);
                    const double inclPct = a_byCycles && totalCycles > 0 ? 100.0 * static_cast<double>(st.inclCycles) / static_cast<double>(totalCycles)
                                                                         : 100.0 * static_cast<double>(st.inclSamples) / static_cast<double>(totalSamples);
                    out << std::format("    excl {:5.1f}% ({:5} smp) | incl {:5.1f}% ({:5} smp) | {}\n",
                                       exclPct, st.exclSamples, inclPct, st.inclSamples, symOf(modIdx, rva));
                }

                std::vector<std::pair<std::vector<std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>>> topStacks(stacks.begin(), stacks.end());
                std::sort(topStacks.begin(), topStacks.end(), [&](const auto& a, const auto& b) {
                    return a_byCycles ? (a.second.second > b.second.second) : (a.second.first > b.second.first);
                });
                out << "  -- Top Call Stacks (leaf -> caller) --\n";
                for (std::size_t i = 0; i < std::min<std::size_t>(topStacks.size(), 25); ++i) {
                    const auto& [stk, cnt] = topStacks[i];
                    const double pct = a_byCycles && totalCycles > 0 ? 100.0 * static_cast<double>(cnt.second) / static_cast<double>(totalCycles)
                                                                     : 100.0 * static_cast<double>(cnt.first) / static_cast<double>(totalSamples);
                    std::string chain;
                    for (std::size_t d = 0; d < stk.size(); ++d) {
                        const auto modIdx = static_cast<std::uint8_t>(stk[d] >> 32);
                        const auto rva = static_cast<std::uint32_t>(stk[d]);
                        if (d > 0) chain += " <- ";
                        chain += symOf(modIdx, rva);
                    }
                    out << std::format("    {:5.1f}% ({:4} smp) {}\n", pct, cnt.first, chain);
                }
                out << "\n";
            };

            const auto mainTid = g_mainTid.load();
            const auto texTid = g_texLoaderTid.load();
            dumpGroup("ALL THREADS ON-CPU (weighted by CPU cycles)", [](const Sample& s) { return s.onCpu != 0; }, true);
            dumpGroup("MAIN THREAD ON-CPU (weighted by CPU cycles)", [mainTid](const Sample& s) { return s.onCpu != 0 && s.tid == mainTid; }, true);
            dumpGroup("MAIN THREAD OFF-CPU / WAITING (by sample count)", [mainTid](const Sample& s) { return s.onCpu == 0 && s.tid == mainTid; }, false);
            dumpGroup("WORKER / LOADER THREADS ON-CPU (excluding MainThread, weighted by CPU cycles)", [mainTid](const Sample& s) { return s.onCpu != 0 && s.tid != mainTid; }, true);
            if (texTid != 0 && texTid != mainTid) {
                dumpGroup("TEXTURE LOADER THREAD (IOManager) ON-CPU", [texTid](const Sample& s) { return s.onCpu != 0 && s.tid == texTid; }, true);
                dumpGroup("TEXTURE LOADER THREAD (IOManager) OFF-CPU / WAITING", [texTid](const Sample& s) { return s.onCpu == 0 && s.tid == texTid; }, false);
            }
        }

        inline void SamplerThread() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            const std::uint32_t selfTid = GetCurrentThreadId();
            const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
            std::unordered_map<std::uint32_t, ThreadTrack> threads;
            std::uint32_t lastScreenSeen = 0;

            for (;;) {
                while (!g_active.load(std::memory_order_acquire)) {
                    const auto curScreen = g_screenIndex.load(std::memory_order_acquire);
                    if (curScreen != lastScreenSeen) {
                        lastScreenSeen = curScreen;
                        for (auto& [tid, tr] : threads) {
                            FILETIME c{}, e{}, k{}, u{};
                            if (GetThreadTimes(tr.handle, &c, &e, &k, &u)) {
                                const auto kNow = FileTimeToU64(k);
                                const auto uNow = FileTimeToU64(u);
                                if (kNow >= tr.screenStartKernel) tr.kernel100ns += kNow - tr.screenStartKernel;
                                if (uNow >= tr.screenStartUser) tr.user100ns += uNow - tr.screenStartUser;
                                tr.screenStartKernel = kNow;
                                tr.screenStartUser = uNow;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(400));
                        if (!g_active.load(std::memory_order_acquire)) {
                            WriteProfileReport(threads);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }

                const auto screen = static_cast<std::uint8_t>(g_screenIndex.load(std::memory_order_relaxed));
                RefreshThreads(threads, selfTid);
                for (auto& [tid, tr] : threads) {
                    ULONG64 cyc = 0;
                    if (QueryThreadCycleTime(tr.handle, &cyc)) tr.lastCycles = cyc;
                    FILETIME c{}, e{}, k{}, u{};
                    if (GetThreadTimes(tr.handle, &c, &e, &k, &u)) {
                        tr.screenStartKernel = FileTimeToU64(k);
                        tr.screenStartUser = FileTimeToU64(u);
                    }
                }

                std::uint32_t tick = 0;
                while (g_active.load(std::memory_order_acquire)) {
                    if (timer) {
                        LARGE_INTEGER due{};
                        due.QuadPart = -10000LL;  // 1.0 ms relative
                        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                        WaitForSingleObject(timer, 5);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (!g_active.load(std::memory_order_acquire)) break;

                    if (++tick % 250 == 0) {
                        RefreshThreads(threads, selfTid);
                    }

                    const auto mainTid = g_mainTid.load(std::memory_order_relaxed);
                    const auto renderTid = g_renderTid.load(std::memory_order_relaxed);
                    const auto texTid = g_texLoaderTid.load(std::memory_order_relaxed);

                    for (auto& [tid, tr] : threads) {
                        ULONG64 cyc = 0;
                        if (!QueryThreadCycleTime(tr.handle, &cyc)) continue;
                        const std::uint64_t delta = cyc > tr.lastCycles ? cyc - tr.lastCycles : 0;
                        tr.lastCycles = cyc;
                        const bool onCpu = delta > 25000;
                        if (onCpu) {
                            ++tr.activeSamples;
                            tr.totalCycles += delta;
                        } else {
                            ++tr.waitSamples;
                            const bool watchWait = (tid == mainTid || tid == renderTid || tid == texTid) ? (tick % 2 == 0)
                                                                                                         : (tr.activeSamples > 0 && (tick % 8 == 0));
                            if (!watchWait) continue;
                        }

                        if (g_samples.size() >= 400000) continue;
                        if (SuspendThread(tr.handle) == static_cast<DWORD>(-1)) continue;
                        CONTEXT ctx{};
                        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                        Sample s{};
                        s.tid = tid;
                        s.screen = screen;
                        s.onCpu = onCpu ? 1 : 0;
                        s.cycles = onCpu ? delta : 0;
                        if (GetThreadContext(tr.handle, &ctx)) {
                            s.depth = UnwindStackSEH(&ctx, s.modIdx, s.funcRva);
                        }
                        ResumeThread(tr.handle);
                        if (s.depth > 0) {
                            g_samples.push_back(s);
                        }
                    }
                }
            }
        }
    }  // namespace detail

    struct TextureReadScope {
        TextureReadScope() noexcept {
            detail::t_inTextureCreate = true;
            detail::g_texLoaderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        }
        ~TextureReadScope() noexcept {
            detail::t_inTextureCreate = false;
        }
    };

    inline void NoteRenderThread(std::uint32_t a_tid) noexcept {
        detail::g_renderTid.store(a_tid, std::memory_order_relaxed);
    }

    inline void OnScreenStart() noexcept {
        if (!detail::g_enabled.load(std::memory_order_relaxed)) return;
        detail::g_screenCounters.Reset();
        detail::g_screenIndex.fetch_add(1, std::memory_order_acq_rel);
        detail::g_active.store(true, std::memory_order_release);
    }

    inline void OnScreenEnd(double a_seconds) {
        if (!detail::g_enabled.load(std::memory_order_relaxed)) return;
        detail::g_active.store(false, std::memory_order_release);
        detail::g_totalScreenSeconds += a_seconds;

        const auto& c = detail::g_screenCounters;
        const auto u = [](const std::atomic<std::uint64_t>& v) { return v.load(std::memory_order_relaxed); };
        std::uint64_t bufCnt = 0, bufBytes = 0, bufTicks = 0;
        for (int i = 0; i < kBufCount; ++i) {
            bufCnt += u(c.bufCount[i]);
            bufBytes += u(c.bufBytes[i]);
            bufTicks += u(c.bufTicks[i]);
        }
        const auto nifFiles = u(c.fileCount[kFileNif]);
        const auto nifBytes = u(c.readBytes[kFileNif]);
        const auto nifTicks = u(c.readTicks[kFileNif]);
        std::uint64_t otherFiles = 0, otherBytes = 0, otherTicks = 0;
        for (int i = 1; i < kFileCount; ++i) {
            otherFiles += u(c.fileCount[i]);
            otherBytes += u(c.readBytes[i]);
            otherTicks += u(c.readTicks[i]);
        }
        SKSE::log::info("[profile] screen {} ({:.2f} s): CreateBuffer {} ({:.1f} MiB, {:.3f} s: VB {} {:.3f} s, IB {} {:.3f} s, CB {} {:.3f} s) | non-tex archive reads: NIF {} files ({:.1f} MiB, {:.3f} s) + other {} files ({:.1f} MiB, {:.3f} s)",
                        detail::g_screenIndex.load(), a_seconds, bufCnt, detail::MiB(bufBytes), detail::QpcSeconds(static_cast<std::int64_t>(bufTicks)),
                        u(c.bufCount[kBufVertex]), detail::QpcSeconds(static_cast<std::int64_t>(u(c.bufTicks[kBufVertex]))),
                        u(c.bufCount[kBufIndex]), detail::QpcSeconds(static_cast<std::int64_t>(u(c.bufTicks[kBufIndex]))),
                        u(c.bufCount[kBufConstant]), detail::QpcSeconds(static_cast<std::int64_t>(u(c.bufTicks[kBufConstant]))),
                        nifFiles, detail::MiB(nifBytes), detail::QpcSeconds(static_cast<std::int64_t>(nifTicks)),
                        otherFiles, detail::MiB(otherBytes), detail::QpcSeconds(static_cast<std::int64_t>(otherTicks)));
    }

    inline void Install(ID3D11Device* a_device, std::uintptr_t a_compressedVtable, std::uintptr_t a_storedVtable, std::size_t a_startOffsetField) {
        detail::g_mainTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        detail::g_startOffsetField = a_startOffsetField;
        detail::SnapshotModules();
        detail::g_samples.reserve(250000);

        constexpr std::size_t kSlot_CreateBuffer = 3;
        constexpr std::size_t kSlot_DoRead = 6;
        if (a_device) {
            SkyrimHooks::Publish(*reinterpret_cast<void***>(a_device) + kSlot_CreateBuffer, &detail::Hook_CreateBuffer, detail::g_origCreateBuffer);
        }
        if (a_compressedVtable) {
            SkyrimHooks::Publish(reinterpret_cast<void**>(a_compressedVtable) + kSlot_DoRead, &detail::Hook_CompDoRead, detail::g_origCompDoRead);
        }
        if (a_storedVtable) {
            SkyrimHooks::Publish(reinterpret_cast<void**>(a_storedVtable) + kSlot_DoRead, &detail::Hook_StoredDoRead, detail::g_origStoredDoRead);
        }
        detail::g_enabled.store(true, std::memory_order_release);
        std::thread(detail::SamplerThread).detach();
        SKSE::log::info("[profile] loading-screen profiler active ({} modules indexed)", detail::g_modules.size());
    }
}  // namespace LoadProfiler
