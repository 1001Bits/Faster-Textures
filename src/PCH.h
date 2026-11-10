#pragma once

// CommonLib first: it refuses to build after the Windows headers.
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <d3d11.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>

using namespace std::literals;
