#pragma once

// SKSE Menu Framework (optional, Nexus SE 120352): at kPostLoad adds the page Faster Textures/Settings to its menu (F1 by
// default) when SKSEMenuFramework.dll is loaded and has every function the page uses; otherwise nothing happens (one log
// line). The MCM stays: both menus keep their choices in the same file, see Streamer::SetSmallFirst.
namespace SmfMenu {
    void Register();
}
