// libFuzzer target for everything that reads untrusted JSON: config.json, imported profiles
// and the GitHub release API response.
// Run: fuzz_config.exe -max_total_time=60

#include "core/ConfigManager.hpp"
#include "core/UpdateChecker.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);

    cadence::AppConfig config{};
    if (cadence::ConfigManager::Deserialize(text, config)) {
        // Whatever was accepted must serialize and parse again
        cadence::AppConfig again{};
        if (!cadence::ConfigManager::Deserialize(cadence::ConfigManager::Serialize(config), again)) {
            std::abort(); // Reported by libFuzzer as a crash with the offending input
        }
    }

    cadence::Profile profile{};
    (void)cadence::ConfigManager::DeserializeProfile(text, profile);

    (void)cadence::UpdateChecker::ParseGitHubReleaseJson(text);
    return 0;
}
