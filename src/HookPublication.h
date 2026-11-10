#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace SkyrimHooks {
    // Native callers may enter a vtable callback as soon as the slot changes.
    // Publish its continuation first, then replace only the observed slot.
    // The caller separately gates streaming work until all hooks/workers exist.
    // A page that holds code as well stays executable while it is writable: other
    // threads may be running that code at that very moment.
    template <class Function>
    bool Publish(void** entry, Function hook, std::atomic<Function>& original) {
        static_assert(std::is_pointer_v<Function>);
        if (!entry || !hook || reinterpret_cast<std::uintptr_t>(entry) % alignof(void*)) return false;
        auto* previous = *entry;
        if (!previous || previous == reinterpret_cast<void*>(hook)) return false;
        MEMORY_BASIC_INFORMATION page{};
        if (!VirtualQuery(entry, &page, sizeof(page))) return false;
        constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const DWORD writable = (page.Protect & kExecutable) != 0 ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        DWORD protection{};
        if (!VirtualProtect(entry, sizeof(void*), writable, &protection)) return false;
        original.store(reinterpret_cast<Function>(previous), std::memory_order_release);
        const auto replaced = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(entry), reinterpret_cast<void*>(hook), previous);
        DWORD unused{};
        VirtualProtect(entry, sizeof(void*), protection, &unused);
        return replaced == previous;
    }
}
