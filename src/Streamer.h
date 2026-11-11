#pragma once

#include <cstdint>
#include <string>

// Loads textures small first and fetches the rest in the background.
//
// The game reads a whole .dds into memory for every texture, even when told to
// create it smaller, and only then uploads it. Each mip holds a quarter of the
// one above it, so the top two are about 94% of a file. While a texture is
// created, its stream is replaced by one that shows the game a smaller DDS made
// of the small mips only, so the rest of the game's loader runs as it always
// does. What that saves depends on how the file is stored:
//
// - Loose files and stored archive entries: reads, unpacking (none) and upload.
// - Compressed entries in independent blocks smaller than the file, which is what
//   Faster Archives repacks large textures into: disk reads, unpacking and upload.
// - Other compressed entries must be unpacked from their start, so their small mips
//   come from a cache on disk, filled in the background (TailCache.h).
//
// A worker then reads each file whole and builds the full texture, what the game
// has drawn first. Full textures replace the small ones at Present. It defers
// work rather than removing it: textures look soft until their detail arrives.
//
// The same stand-in stream measures loading: Mode=1 changes nothing and logs how
// long each loading screen spends reading and creating textures.
namespace Streamer {
    // Reads the settings and hooks texture creation. Call when the plugin loads.
    void Install();

    // Starts the background work, once the renderer exists and every plugin has
    // loaded. Beside Texture Downscaler, full textures arrive at the sizes it gives
    // them. Beside Skyrim Performance Solution's texture streaming, what SPS can
    // manage is handed to it after loading.
    void OnDataLoaded();

    void LogSummary();

    // The in-game menus. SkyUI's MCM (through MCM Helper) and SKSE Menu Framework's
    // page both keep the player's choices in Data/MCM/Settings/FasterTextures.ini,
    // which wins over the SKSE ini.
    bool SmallFirst(bool a_loadingScreens);                // what applies now
    void SetSmallFirst(bool a_loadingScreens, bool a_on);  // applies at once, saved in the background

    struct CacheChoice {
        int location = 0;  // 0 beside the game, 1 local app data, 2 the folder below
        std::string folder;
    };
    CacheChoice SavedCacheChoice();              // reads the files: not on the render thread
    void SetCacheChoice(const CacheChoice& a_choice);  // saved in the background, applies after a restart

    std::string Status();  // what runs this session, in a line or two
}
