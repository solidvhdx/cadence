#pragma once

#include "core/Types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cadence {

class ProfileManager {
public:
    explicit ProfileManager(AppConfig& config);

    void EnsureDefaultProfiles();

    [[nodiscard]] const std::vector<Profile>& Profiles() const noexcept { return m_config.profiles; }
    [[nodiscard]] const Profile* FindById(const std::string& id) const noexcept;
    [[nodiscard]] std::string ActiveProfileName() const;

    // Applies profile to active config (preserving independent settings cleanly)
    bool ActivateProfile(const std::string& id);

    // Creates a new profile capturing current CPS & KPS configs and active mode
    Profile& CreateFromCurrent(const std::string& name);

    // Updates currently active profile with current CPS/KPS state
    bool UpdateActiveProfile();

    bool DeleteProfile(const std::string& id);

    bool ExportProfile(const std::string& id, const std::filesystem::path& destPath) const;
    bool ImportProfile(const std::filesystem::path& srcPath);

private:
    AppConfig& m_config;
};

} // namespace cadence
