#pragma once

// A texture's small mips kept aside, for archive entries they cannot be read
// from: Bethesda's frames are linked or hold one block as large as the file, so
// reaching the small mips means unpacking everything before them. Repacking solves
// that but takes a second copy of every archive; these are about a sixteenth of it.
//
// One file of records and one of tails. A record is only written after its tail,
// and every read is checked against the tail's hash, so a crash or a full disk
// loses entries rather than serving wrong ones. Keys change when an archive does.
// The caller compares nothing else: the file's real header is read on every load,
// and what a tail must match comes from it. Game-independent for the tests.

#include "ArchiveTail.h"

#include <lz4.h>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TailCache {
    struct Key {
        std::uint64_t archive{};
        std::uint64_t offset{};
        bool operator==(const Key&) const = default;
    };

    struct KeyHash {
        std::size_t operator()(const Key& a_key) const noexcept { return static_cast<std::size_t>(a_key.archive ^ (a_key.offset * 0x9E3779B97F4A7C15ull)); }
    };

    // What a tail must match: a file with other headers, or a plan that keeps other mips, is another tail.
    struct What {
        std::uint64_t headHash{};
        std::uint32_t original{}, tailOffset{}, tailBytes{};
        bool operator==(const What&) const = default;
    };

    // An archive's identity: its name, size and time of writing. Zero when unknown.
    [[nodiscard]] inline std::uint64_t ArchiveId(HANDLE a_handle, std::string_view a_name) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(a_handle, &info)) return 0;
        std::string lower(a_name);
        for (auto& c : lower) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        const std::array<std::uint32_t, 4> facts{info.nFileSizeHigh, info.nFileSizeLow, info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime};
        return XXH64(facts.data(), sizeof(facts), XXH64(lower.data(), lower.size(), 0));
    }

    [[nodiscard]] inline What WhatOf(const ArchiveTail::Head& a_head, const MipTail::Plan& a_plan) {
        return {XXH64(a_head.dds.data(), a_head.dds.size(), 0), a_head.original, static_cast<std::uint32_t>(a_plan.tailOffset), static_cast<std::uint32_t>(a_plan.tailBytes)};
    }

    class Store {
    public:
        struct Figures {
            std::uint64_t entries{}, bytes{}, maxBytes{};
            bool full{};
        };

        Store() = default;
        Store(const Store&) = delete;
        Store& operator=(const Store&) = delete;
        ~Store() { Close(); }

        // Opens what is there, or starts anew. a_live are the archives in use: when
        // over a quarter of the file is tails of other archives or replaced ones, it
        // starts anew too, as nothing else ever reclaims them. A full disk stops it
        // growing at the first write that fails. False leaves no store: a folder that
        // cannot be written, or another running game holding it.
        [[nodiscard]] bool Open(const std::filesystem::path& a_folder, std::uint64_t a_maxBytes, const std::vector<std::uint64_t>& a_live) {
            std::error_code error;
            std::filesystem::create_directories(a_folder, error);
            const auto records = a_folder / "tails.idx";
            const auto data = a_folder / "tails.dat";
            records_ = CreateFileW(records.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            write_ = CreateFileW(data.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            read_ = CreateFileW(data.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, nullptr);
            if (records_ == INVALID_HANDLE_VALUE || write_ == INVALID_HANDLE_VALUE || read_ == INVALID_HANDLE_VALUE) {
                CloseHandles();
                return false;
            }
            if (!Load(a_live) && !Reset()) {
                CloseHandles();
                return false;
            }
            maxBytes_ = a_maxBytes;
            stop_ = false;
            writer_ = std::thread([this] { Writing(); });
            return true;
        }

        void Close() {
            if (writer_.joinable()) {
                {
                    std::scoped_lock lock(queueLock_);
                    stop_ = true;
                }
                queueWake_.notify_all();
                writer_.join();
            }
            CloseHandles();
        }

        [[nodiscard]] bool Has(const Key& a_key, const What& a_what) const {
            std::shared_lock lock(slotsLock_);
            const auto it = slots_.find(a_key);
            return it != slots_.end() && it->second.what == a_what;
        }

        // The tail into a_dst, of a_what.tailBytes. False on anything but exactly that tail.
        [[nodiscard]] bool Read(const Key& a_key, const What& a_what, std::uint8_t* a_dst) {
            Slot slot;
            {
                std::shared_lock lock(slotsLock_);
                const auto it = slots_.find(a_key);
                if (it == slots_.end() || !(it->second.what == a_what)) return false;
                slot = it->second;
            }
            if (ArchiveIndex::ReadAt(read_, slot.at, a_dst, a_what.tailBytes) && XXH64(a_dst, a_what.tailBytes, 0) == slot.hash) return true;
            std::unique_lock lock(slotsLock_);
            slots_.erase(a_key);
            return false;
        }

        // Queues a copy of the tail for writing. False when it is not needed, or there
        // is no room, in which case it only means this texture stays out of the store.
        bool Add(const Key& a_key, const What& a_what, const std::uint8_t* a_tail) noexcept {
            if (a_what.tailBytes == 0 || failed_.load(std::memory_order_relaxed)) return false;
            try {
                {
                    // Both under the queue's lock: the writer stores a tail before it leaves the queue.
                    std::scoped_lock lock(queueLock_);
                    {
                        std::shared_lock slots(slotsLock_);
                        const auto it = slots_.find(a_key);
                        if (it != slots_.end() && it->second.what == a_what) return false;
                    }
                    if (used_ + queuedBytes_ + a_what.tailBytes > maxBytes_) {
                        full_ = true;
                        return false;
                    }
                    if (queuedBytes_ + a_what.tailBytes > kMaxQueued || queuedKeys_.contains(a_key)) return false;
                    queuedKeys_.insert(a_key);
                    queuedBytes_ += a_what.tailBytes;
                }
                std::vector<std::uint8_t> tail;
                try {
                    tail.assign(a_tail, a_tail + a_what.tailBytes);
                } catch (...) {
                    std::scoped_lock lock(queueLock_);
                    queuedKeys_.erase(a_key);
                    queuedBytes_ -= a_what.tailBytes;
                    return false;
                }
                {
                    std::scoped_lock lock(queueLock_);
                    queue_.push_back({a_key, a_what, std::move(tail)});
                }
                queueWake_.notify_one();
                return true;
            } catch (...) {
                return false;
            }
        }

        // Whether a tail of this size could still be kept.
        [[nodiscard]] bool HasRoom(std::uint64_t a_bytes) {
            std::scoped_lock lock(queueLock_);
            if (used_ + queuedBytes_ + a_bytes <= maxBytes_) return true;
            full_ = true;
            return false;
        }

        // Waits, for one that writes faster than the disk takes it.
        void WaitForQueueBelow(std::uint64_t a_bytes) {
            std::unique_lock lock(queueLock_);
            drained_.wait(lock, [&] { return queuedBytes_ < a_bytes; });
        }

        // Until everything queued is written.
        void Drain() { WaitForQueueBelow(1); }

        [[nodiscard]] Figures Current() {
            Figures figures;
            {
                std::shared_lock lock(slotsLock_);
                figures.entries = slots_.size();
            }
            std::scoped_lock lock(queueLock_);
            figures.bytes = used_;
            figures.maxBytes = maxBytes_;
            figures.full = full_;
            return figures;
        }

        [[nodiscard]] bool WriteFailed() const { return failed_.load(std::memory_order_relaxed); }

    private:
        struct Slot {
            What what;
            std::uint64_t at{}, hash{};
        };

        struct Record {
            std::uint64_t archive{}, offset{}, at{}, hash{}, headHash{};
            std::uint32_t original{}, tailOffset{}, tailBytes{}, check{};
        };
        static_assert(sizeof(Record) == 56);

        struct Job {
            Key key;
            What what;
            std::vector<std::uint8_t> tail;
        };

        static constexpr std::array<char, 8> kMagic{'F', 'T', 'X', 'T', 'A', 'I', 'L', '1'};
        static constexpr std::uint64_t kMaxQueued = 64ull << 20;

        static std::uint32_t CheckOf(const Record& a_record) { return XXH32(&a_record, offsetof(Record, check), 0); }

        static bool WriteAt(HANDLE a_handle, std::uint64_t a_at, const void* a_data, std::uint32_t a_bytes) {
            OVERLAPPED where{};
            where.Offset = static_cast<DWORD>(a_at);
            where.OffsetHigh = static_cast<DWORD>(a_at >> 32);
            DWORD written = 0;
            return WriteFile(a_handle, a_data, a_bytes, &written, &where) && written == a_bytes;
        }

        static bool Truncate(HANDLE a_handle, std::uint64_t a_bytes) {
            LARGE_INTEGER at;
            at.QuadPart = static_cast<LONGLONG>(a_bytes);
            return SetFilePointerEx(a_handle, at, nullptr, FILE_BEGIN) && SetEndOfFile(a_handle);
        }

        void CloseHandles() {
            for (auto* handle : {&records_, &write_, &read_}) {
                if (*handle != INVALID_HANDLE_VALUE) CloseHandle(*handle);
                *handle = INVALID_HANDLE_VALUE;
            }
        }

        bool Reset() {
            slots_.clear();
            used_ = 0;
            recordsAt_ = kMagic.size();
            return Truncate(write_, 0) && Truncate(records_, 0) && WriteAt(records_, 0, kMagic.data(), static_cast<std::uint32_t>(kMagic.size()));
        }

        // False to start anew.
        bool Load(const std::vector<std::uint64_t>& a_live) {
            LARGE_INTEGER recordBytes{}, dataBytes{};
            if (!GetFileSizeEx(records_, &recordBytes) || !GetFileSizeEx(write_, &dataBytes)) return false;
            if (recordBytes.QuadPart < static_cast<LONGLONG>(kMagic.size()) || recordBytes.QuadPart > (1ll << 30)) return false;

            std::vector<std::uint8_t> raw(static_cast<std::size_t>(recordBytes.QuadPart));
            DWORD read = 0;
            OVERLAPPED start{};
            if (!ReadFile(records_, raw.data(), static_cast<DWORD>(raw.size()), &read, &start) || read != raw.size()) return false;
            if (std::memcmp(raw.data(), kMagic.data(), kMagic.size()) != 0) return false;

            const std::unordered_set<std::uint64_t> live(a_live.begin(), a_live.end());
            std::uint64_t at = kMagic.size(), end = 0, stale = 0;
            for (; at + sizeof(Record) <= raw.size(); at += sizeof(Record)) {
                Record record;
                std::memcpy(&record, raw.data() + at, sizeof(record));
                if (record.check != CheckOf(record) || record.at + record.tailBytes > static_cast<std::uint64_t>(dataBytes.QuadPart)) break;
                end = std::max(end, record.at + record.tailBytes);
                if (!live.contains(record.archive)) {
                    stale += record.tailBytes;
                    continue;
                }
                const Key key{record.archive, record.offset};
                if (const auto old = slots_.find(key); old != slots_.end()) stale += old->second.what.tailBytes;
                slots_[key] = {{record.headHash, record.original, record.tailOffset, record.tailBytes}, record.at, record.hash};
            }
            // Whatever came after the last sound record was never finished.
            if (at != raw.size() && !Truncate(records_, at)) return false;
            if (end != static_cast<std::uint64_t>(dataBytes.QuadPart) && !Truncate(write_, end)) return false;
            recordsAt_ = at;
            used_ = end;
            return stale <= end / 4;
        }

        void Writing() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(queueLock_);
                    queueWake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                    if (queue_.empty()) return;
                    job = std::move(queue_.front());
                    queue_.pop_front();
                }
                const auto at = used_;
                Record record{job.key.archive, job.key.offset, at, XXH64(job.tail.data(), job.tail.size(), 0), job.what.headHash, job.what.original, job.what.tailOffset, job.what.tailBytes, 0};
                record.check = CheckOf(record);
                const bool ok = !failed_.load(std::memory_order_relaxed) && WriteAt(write_, at, job.tail.data(), job.what.tailBytes) && WriteAt(records_, recordsAt_, &record, sizeof(record));
                if (ok) {
                    recordsAt_ += sizeof(record);
                    std::unique_lock lock(slotsLock_);
                    slots_[job.key] = {job.what, at, record.hash};
                } else {
                    failed_.store(true, std::memory_order_relaxed);  // a full disk, most likely: no more tries this session
                }
                {
                    std::scoped_lock lock(queueLock_);
                    if (ok) used_ += job.what.tailBytes;
                    queuedKeys_.erase(job.key);
                    queuedBytes_ -= job.what.tailBytes;
                }
                drained_.notify_all();
            }
        }

        HANDLE records_ = INVALID_HANDLE_VALUE, write_ = INVALID_HANDLE_VALUE, read_ = INVALID_HANDLE_VALUE;
        std::uint64_t maxBytes_ = 0, recordsAt_ = 0;

        mutable std::shared_mutex slotsLock_;
        std::unordered_map<Key, Slot, KeyHash> slots_;

        std::mutex queueLock_;
        std::condition_variable queueWake_, drained_;
        std::deque<Job> queue_;
        std::unordered_set<Key, KeyHash> queuedKeys_;
        std::uint64_t queuedBytes_ = 0, used_ = 0;
        bool stop_ = false, full_ = false;
        std::atomic<bool> failed_{false};
        std::thread writer_;
    };

    // ── Filling from an archive, ahead of the first load ─────────────────────

    // Decodes a whole frame of any block layout into a_dst, a_total bytes.
    [[nodiscard]] inline bool DecodeFrame(const std::uint8_t* a_frame, std::size_t a_size, const Lz4Blocks::Header& a_h, std::uint8_t* a_dst, std::uint64_t a_total) {
        constexpr std::uint64_t kWindow = 64u << 10;
        std::size_t at = a_h.size;
        std::uint64_t out = 0;
        for (;;) {
            if (at + 4 > a_size) return false;
            std::uint32_t word{};
            std::memcpy(&word, a_frame + at, 4);
            at += 4;
            if (word == 0) return out == a_total;
            const std::uint32_t size = word & 0x7FFFFFFFu;
            if (size == 0 || size > a_h.blockMax || at + size > a_size || out >= a_total) return false;
            const auto room = static_cast<int>(std::min<std::uint64_t>(a_h.blockMax, a_total - out));
            const auto* src = reinterpret_cast<const char*>(a_frame + at);
            auto* dst = reinterpret_cast<char*>(a_dst + out);
            int decoded;
            if (word & 0x80000000u) {
                if (static_cast<int>(size) > room) return false;
                std::memcpy(dst, src, size);
                decoded = static_cast<int>(size);
            } else if (a_h.independent) {
                decoded = LZ4_decompress_safe(src, dst, static_cast<int>(size), room);
            } else {
                const auto window = std::min(out, kWindow);
                decoded = LZ4_decompress_safe_usingDict(src, dst, static_cast<int>(size), room, dst - window, static_cast<int>(window));
            }
            if (decoded < 0) return false;
            out += static_cast<std::uint64_t>(decoded);
            at += size + (a_h.blockChecksum ? 4 : 0);
        }
    }

    struct FillBuffers {
        std::vector<std::uint8_t> raw, decoded;
    };

    enum class Filled { kAdded, kHad, kNotNeeded, kFailed, kFull };

    inline constexpr std::uint32_t kMaxFillFile = 512u << 20;

    // Puts one archive entry's tail in the store, if it needs one. a_planOf says
    // what the game would load small: (const SkyrimSource::Info&) -> optional<Plan>.
    template <class PlanOf>
    [[nodiscard]] Filled FillEntry(Store& a_store, const ArchiveTail::Located& a_entry, std::uint64_t a_archive, const PlanOf& a_planOf, FillBuffers& a_buffers) {
        const auto head = ArchiveTail::ReadHead(a_entry);
        if (!head) return Filled::kNotNeeded;
        const auto file = SkyrimSource::Parse(head->dds, head->original);
        const auto plan = file ? a_planOf(*file) : std::nullopt;
        if (!plan || MipTail::CanSkip(head->frame, plan->tailOffset) || head->original > kMaxFillFile) return Filled::kNotNeeded;

        const auto what = WhatOf(*head, *plan);
        const Key key{a_archive, a_entry.offset};
        if (a_store.Has(key, what)) return Filled::kHad;
        if (!a_store.HasRoom(what.tailBytes)) return Filled::kFull;

        a_buffers.raw.resize(a_entry.bytes);
        a_buffers.decoded.resize(head->original);
        if (!ArchiveIndex::ReadAt(a_entry.handle, a_entry.offset, a_buffers.raw.data(), a_entry.bytes) ||
            !DecodeFrame(a_buffers.raw.data() + 4, a_buffers.raw.size() - 4, head->frame, a_buffers.decoded.data(), head->original))
            return Filled::kFailed;
        a_store.WaitForQueueBelow(32ull << 20);
        return a_store.Add(key, what, a_buffers.decoded.data() + plan->tailOffset) ? Filled::kAdded : Filled::kHad;
    }
}
