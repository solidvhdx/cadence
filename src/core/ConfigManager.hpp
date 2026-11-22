#pragma once

#include "core/Types.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace cadence {

class ConfigManager {
public:
    static std::filesystem::path DefaultConfigPath();

    static bool LoadFromFile(const std::filesystem::path& path, AppConfig& outConfig);
    static bool SaveToFile(const std::filesystem::path& path, const AppConfig& config);

    static std::string Serialize(const AppConfig& config);
    static bool        Deserialize(const std::string& jsonText, AppConfig& outConfig);

    static std::string SerializeProfile(const Profile& profile);
    static bool        DeserializeProfile(const std::string& jsonText, Profile& outProfile);
};

} // namespace cadence
