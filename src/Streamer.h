#pragma once

#include <cstdint>

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
    // loaded. Stands down beside Texture Downscaler. Beside Skyrim Performance
    // Solution's texture streaming, what SPS can manage is handed to it after loading.
    void OnDataLoaded();

    void LogSummary();
}
