#pragma once

#include "core/Types.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cadence {

// Version of the config.json format written by this build. Bump it together with a migration step
// in ConfigManager::Deserialize whenever the meaning of an existing field changes.
inline constexpr int kConfigVersion = 1;

enum class ConfigLoadError : uint8_t {
    NotFound,    // No config yet (first start) - defaults are fine
    TooLarge,    // Larger than the 2 MB safety cap
    ReadFailed,  // File exists but could not be read
    ParseFailed  // File content is not a valid Cadence config
};

[[nodiscard]] std::wstring_view DescribeConfigLoadError(ConfigLoadError error) noexcept;

class ConfigManager {
public:
    static std::filesystem::path DefaultConfigPath();

    [[nodiscard]] static std::expected<void, ConfigLoadError> LoadFromFile(const std::filesystem::path& path, AppConfig& outConfig);
    [[nodiscard]] static bool SaveToFile(const std::filesystem::path& path, const AppConfig& config);

    // Copies a config that could not be loaded to "<name>.bak" before it gets overwritten by defaults.
    // Returns the backup path, or nullopt if the copy failed.
    [[nodiscard]] static std::optional<std::filesystem::path> BackupFile(const std::filesystem::path& path);

    [[nodiscard]] static std::string Serialize(const AppConfig& config);
    [[nodiscard]] static bool        Deserialize(const std::string& jsonText, AppConfig& outConfig);

    [[nodiscard]] static std::string SerializeProfile(const Profile& profile);
    [[nodiscard]] static bool        DeserializeProfile(const std::string& jsonText, Profile& outProfile);
};

} // namespace cadence
