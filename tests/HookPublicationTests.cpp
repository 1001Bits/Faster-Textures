#include "HookPublication.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
    struct Context;
    using Function = unsigned (*)(Context*, unsigned);
    struct Context { std::atomic<Function> original{}; std::atomic<unsigned> calls{}; };
    unsigned Native(Context*, unsigned input) { return input + 17; }
    unsigned Hook(Context* context, unsigned input) {
        ++context->calls;
        const auto previous = context->original.load(std::memory_order_acquire);
        return previous ? previous(context, input) : 0;
    }
}

int main() {
    constexpr unsigned count = 64;
    auto** slots = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!slots) return 1;
    std::array<Context, count> contexts{};
    for (unsigned i = 0; i < count; ++i) slots[i] = reinterpret_cast<void*>(&Native);
    DWORD old{};
    if (!VirtualProtect(slots, 4096, PAGE_READONLY, &old)) return 2;
    std::atomic<bool> stop{};
    std::atomic<unsigned> ready{}, failures{}, visits{};
    std::array<std::jthread, 4> callers;
    for (auto& thread : callers) thread = std::jthread([&] {
        ++ready;
        while (!stop.load()) {
            for (unsigned i = 0; i < count; ++i) {
                // The game's aligned vtable read has the same atomicity.
                const auto function = reinterpret_cast<Function>(
                    std::atomic_ref<void*>(slots[i]).load(std::memory_order_acquire));
                if (function(&contexts[i], i) != i + 17) ++failures;
            }
            ++visits;
        }
    });
    while (ready.load() != callers.size() || visits.load() < 100) std::this_thread::yield();
    for (unsigned i = 0; i < count; ++i) {
        if (!SkyrimHooks::Publish(slots + i, &Hook, contexts[i].original)) ++failures;
        if (SkyrimHooks::Publish(slots + i, &Hook, contexts[i].original)) ++failures;
    }
    const auto after = visits.load();
    while (visits.load() < after + 2000) std::this_thread::yield();
    stop.store(true);
    for (auto& thread : callers) thread.join();
    for (const auto& context : contexts)
        if (!context.calls.load() || context.original.load() != &Native) ++failures;
    MEMORY_BASIC_INFORMATION protection{};
    if (!VirtualQuery(slots, &protection, sizeof(protection)) || protection.Protect != PAGE_READONLY) ++failures;
    std::atomic<Function> empty{};
    if (SkyrimHooks::Publish<Function>(nullptr, &Hook, empty) ||
        SkyrimHooks::Publish<Function>(slots, nullptr, empty) || empty.load()) ++failures;

    // A vtable on a page that also holds code, as in some wrappers: the code keeps
    // running while slots on its page are published. Losing execute rights for a
    // moment would end this process with an access violation.
    auto* page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!page) return 4;
    auto** shared = reinterpret_cast<void**>(page);
    for (unsigned i = 0; i < count; ++i) shared[i] = reinterpret_cast<void*>(&Native);
    const unsigned char code[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};  // mov eax, 7; ret
    std::memcpy(page + 2048, code, sizeof(code));
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old)) return 5;
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    const auto run = reinterpret_cast<unsigned (*)()>(page + 2048);
    std::array<Context, count> sharedContexts{};
    std::atomic<bool> stopCode{};
    std::atomic<unsigned> codeRuns{}, codeReady{};
    std::jthread runner([&] {
        ++codeReady;
        while (!stopCode.load()) {
            if (run() != 7) ++failures;
            ++codeRuns;
        }
    });
    while (!codeReady.load() || codeRuns.load() < 1000) std::this_thread::yield();
    for (unsigned i = 0; i < count; ++i)
        if (!SkyrimHooks::Publish(shared + i, &Hook, sharedContexts[i].original)) ++failures;
    stopCode.store(true);
    runner.join();
    for (unsigned i = 0; i < count; ++i)
        if (reinterpret_cast<Function>(shared[i])(&sharedContexts[i], i) != i + 17 || sharedContexts[i].original.load() != &Native) ++failures;
    if (!VirtualQuery(page, &protection, sizeof(protection)) || protection.Protect != PAGE_EXECUTE_READ) ++failures;
    VirtualFree(page, 0, MEM_RELEASE);

    const auto errors = failures.load();
    std::printf("Concurrent vtable publication: %u calls, %u failures; code on a hooked page ran %u times\n", visits.load() * count, errors, codeRuns.load());
    VirtualFree(slots, 0, MEM_RELEASE);
    return errors ? 3 : 0;
}
