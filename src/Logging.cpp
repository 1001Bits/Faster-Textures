#include "Logging.h"

void OpenLog() {
    auto path = SKSE::log::log_directory();
    if (!path) return;

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    *path /= std::format("{}.log", plugin->GetName());

    auto log = std::make_shared<spdlog::logger>("global",
                                                std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true));
    log->set_pattern("[%H:%M:%S.%e] [%l] %v");
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));

    SKSE::log::info("{} {}", plugin->GetName(), plugin->GetVersion().string("."sv));
}
