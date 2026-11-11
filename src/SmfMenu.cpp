#include "SmfMenu.h"

#include "Streamer.h"

#include <cstdarg>
#include <cstring>

// SKSE Menu Framework draws its menu with Dear ImGui and exports the cimgui functions (igCheckbox, ...) beside its own
// (AddSectionItem). The few used here are looked up by name, as in Faster Objects' page: nothing is linked, a missing DLL or
// function only leaves the page out, and no pointer that was not found is ever called. Names and signatures are cimgui's
// (Dear ImGui 1.90), checked against SKSEMenuFramework.dll 3.23. Every flags argument is 0: flag values differ between
// ImGui versions.
//
// The framework calls the page once per frame while it is open, inside its ImGui frame on the game's render thread. So the
// page does no file work: the small-first switches go to Streamer, which applies them at once and saves in the background,
// and the cache location, read once when the page is added, is saved the same way.
namespace SmfMenu {
    namespace {
        using RenderFunction = void(__stdcall*)();
        using TextV = void (*)(const char*, va_list);

        struct Api {
            void (*AddSectionItem)(const char* path, RenderFunction render) = nullptr;
            bool (*Checkbox)(const char* label, bool* value) = nullptr;
            bool (*RadioButton)(const char* label, int* value, int button) = nullptr;
            bool (*InputText)(const char* label, char* buffer, std::size_t size, int flags, void* callback, void* user) = nullptr;
            bool (*IsItemDeactivatedAfterEdit)() = nullptr;
            void (*BeginDisabled)(bool disabled) = nullptr;
            void (*EndDisabled)() = nullptr;
            TextV SetItemTooltipV = nullptr;
            TextV TextWrappedV = nullptr;
            TextV TextDisabledV = nullptr;
            void (*SeparatorText)(const char* label) = nullptr;
            void (*Spacing)() = nullptr;
        };
        Api api;  // complete before the page is added, then only read by it

        template <class F>
        bool Resolve(HMODULE a_module, const char* a_name, F& a_out) {
            a_out = reinterpret_cast<F>(GetProcAddress(a_module, a_name));
            if (!a_out) SKSE::log::info("[menu] SKSE Menu Framework has no {}; no page in its menu", a_name);
            return a_out != nullptr;
        }

        // ImGui's text functions take a printf format: text always goes in as its argument, so a '%' in it is shown as is.
        void CallV(TextV a_function, const char* a_format, ...) {
            va_list arguments;
            va_start(arguments, a_format);
            a_function(a_format, arguments);
            va_end(arguments);
        }

        void Tooltip(const char* a_help) { CallV(api.SetItemTooltipV, "%s", a_help); }

        // The cache location as the page shows it: read when the page is added, then changed only here.
        Streamer::CacheChoice g_cache;
        char g_folder[512]{};
        std::string g_status;  // refreshed every few seconds from Streamer::Status, which only reads atomics
        std::uint32_t g_statusAge = 0;

        void Toggle(bool a_loadingScreens, const char* a_label, const char* a_help) {
            bool on = Streamer::SmallFirst(a_loadingScreens);
            if (api.Checkbox(a_label, &on)) Streamer::SetSmallFirst(a_loadingScreens, on);
            Tooltip(a_help);
        }

        void CacheLocation() {
            int location = g_cache.location;
            bool changed = api.RadioButton("Beside the game##ftcache", &location, 0);
            Tooltip("A folder called FasterTextures Cache beside the game, outside the Data folder. If that cannot be written, local app data.");
            changed |= api.RadioButton("Local app data##ftcache", &location, 1);
            Tooltip("FasterTextures\\Cache in your local app data folder.");
            changed |= api.RadioButton("Folder of your choice##ftcache", &location, 2);
            Tooltip("The folder typed below. Pick one on a disk with room: the cache takes about a seventh of the size of the archives it covers.");
            api.BeginDisabled(location != 2);
            api.InputText("Folder##ftcachefolder", g_folder, sizeof(g_folder), 0, nullptr, nullptr);
            const bool typed = api.IsItemDeactivatedAfterEdit();
            api.EndDisabled();
            Tooltip("A full path, for example D:\\FasterTextures Cache. Saved when you leave the field.");
            if (changed || typed) {
                g_cache.location = location;
                g_cache.folder = g_folder;
                Streamer::SetCacheChoice(g_cache);
            }
            CallV(api.TextDisabledV, "%s", "The cache location applies after you restart the game.");
        }

        void Failed(const char* a_what) {
            static bool logged = false;  // the render thread only
            if (logged) return;
            logged = true;
            SKSE::log::warn("[menu] SKSE Menu Framework page: {}; the rest of the page is left out while this happens", a_what);
        }

        void __stdcall RenderSettings() {
            try {
                static const std::string intro = std::format("Faster Textures {}. Changes are saved to Data\\MCM\\Settings\\FasterTextures.ini, which the MCM menu uses too.",
                                                             SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."));
                CallV(api.TextDisabledV, "%s", intro.c_str());
                api.SeparatorText("Load textures small first");
                Toggle(true, "During loading screens##ftscreens",
                       "Loading screens get a small version of each large texture and end sooner; the full textures replace them right after. "
                       "Off: textures are sharp the moment a loading screen ends, and loading screens take longer. Applies at once.");
                Toggle(false, "During play##ftplay",
                       "Textures that load while you play come in small first, so the game's loading thread gets through its work sooner: less "
                       "stutter from large textures, and new objects can look soft for a moment. Off: they load at full size, as usual. Applies at once.");
                api.SeparatorText("Small mips cache");
                CacheLocation();
                api.SeparatorText("Status");
                if (g_statusAge++ % 120 == 0) g_status = Streamer::Status();
                CallV(api.TextWrappedV, "%s", g_status.c_str());
                api.Spacing();
            } catch (const std::exception& error) {
                Failed(error.what());
            } catch (...) {
                Failed("unknown error");
            }
        }
    }

    void Register() {
        try {
            const auto module = GetModuleHandleW(L"SKSEMenuFramework.dll");
            if (!module) {
                SKSE::log::info("[menu] SKSE Menu Framework is not installed; no page in its menu");
                return;
            }
            Api found;
            const bool complete = Resolve(module, "AddSectionItem", found.AddSectionItem) && Resolve(module, "igCheckbox", found.Checkbox) &&
                                  Resolve(module, "igRadioButton_IntPtr", found.RadioButton) && Resolve(module, "igInputText", found.InputText) &&
                                  Resolve(module, "igIsItemDeactivatedAfterEdit", found.IsItemDeactivatedAfterEdit) &&
                                  Resolve(module, "igBeginDisabled", found.BeginDisabled) && Resolve(module, "igEndDisabled", found.EndDisabled) &&
                                  Resolve(module, "igSetItemTooltipV", found.SetItemTooltipV) && Resolve(module, "igTextWrappedV", found.TextWrappedV) &&
                                  Resolve(module, "igTextDisabledV", found.TextDisabledV) && Resolve(module, "igSeparatorText", found.SeparatorText) &&
                                  Resolve(module, "igSpacing", found.Spacing);
            if (!complete) return;
            api = found;
            g_cache = Streamer::SavedCacheChoice();
            const auto size = std::min(g_cache.folder.size(), sizeof(g_folder) - 1);
            std::memcpy(g_folder, g_cache.folder.data(), size);
            g_folder[size] = '\0';
            // Once, at kPostLoad: nothing renders yet. The framework splits the path at '/' into section and page.
            api.AddSectionItem("Faster Textures/Settings", RenderSettings);
            SKSE::log::info("[menu] Faster Textures page added to SKSE Menu Framework's menu");
        } catch (const std::exception& error) {
            SKSE::log::warn("[menu] SKSE Menu Framework page not added: {}", error.what());
        } catch (...) {
            SKSE::log::warn("[menu] SKSE Menu Framework page not added: unknown error");
        }
    }
}
