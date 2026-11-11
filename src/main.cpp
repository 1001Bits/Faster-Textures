// Faster Textures for Skyrim (SE / AE / VR)

#include "Logging.h"
#include "SmfMenu.h"
#include "Streamer.h"

namespace {
    void OnMessage(SKSE::MessagingInterface::Message* message) {
        switch (message->type) {
            case SKSE::MessagingInterface::kPostLoad:
                SmfMenu::Register();
                break;
            case SKSE::MessagingInterface::kDataLoaded:
                Streamer::OnDataLoaded();
                break;
            case SKSE::MessagingInterface::kNewGame:
            case SKSE::MessagingInterface::kPostLoadGame:
            case SKSE::MessagingInterface::kSaveGame:
                Streamer::LogSummary();
                break;
            default:
                break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse, false);  // no log of CommonLib's own: OpenLog makes this plugin's
    OpenLog();

    if (REL::Module::IsVR()) SKSE::log::info("Skyrim VR");

    Streamer::Install();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
