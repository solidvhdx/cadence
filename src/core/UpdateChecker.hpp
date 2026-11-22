#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cadence {

struct SemVer {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;

    [[nodiscard]] auto operator<=>(const SemVer&) const noexcept = default;
};

struct ReleaseInfo {
    std::string tagName;     // e.g. "v1.0.2"
    SemVer      version{};
    std::string downloadUrl; // Direct HTTPS link to Cadence.exe asset
    std::string releaseUrl;  // GitHub release page URL (fallback)
};

class UpdateChecker {
public:
    static constexpr SemVer kCurrentVersion{
        CADENCE_VERSION_MAJOR,
        CADENCE_VERSION_MINOR,
        CADENCE_VERSION_PATCH
    };

    // Pure version parsing & comparison (strip optional leading 'v'/'V', ignore suffixes like -beta)
    [[nodiscard]] static std::optional<SemVer> ParseSemVer(std::string_view tag) noexcept;
    [[nodiscard]] static bool IsNewerVersion(std::string_view candidateTag, SemVer current = kCurrentVersion) noexcept;

    // Extracts tag_name, html_url, and Cadence.exe browser_download_url from GitHub's releases/latest JSON
    [[nodiscard]] static std::optional<ReleaseInfo> ParseGitHubReleaseJson(std::string_view json);

    // Queries GitHub Releases API synchronously (intended for background thread). Returns newer release if available.
    [[nodiscard]] static std::optional<ReleaseInfo> CheckForUpdate();

    // Downloads a verified Cadence.exe binary over HTTPS to destPath
    [[nodiscard]] static bool DownloadBinary(const std::string& url, const std::filesystem::path& destPath);

    // Validates that a file on disk is a genuine, non-truncated x64 Windows PE executable (not a DLL)
    [[nodiscard]] static bool VerifyPe64Binary(const std::filesystem::path& exePath) noexcept;

    // Renames running exe -> .old, moves newExePath -> current exe, and spawns updated process with --updated
    [[nodiscard]] static bool ApplyUpdateAndRelaunch(const std::filesystem::path& newExePath);

    // Removes leftover .old binary from a previous self-update (safe to call on every startup)
    static void CleanupOldBinary() noexcept;

    [[nodiscard]] static std::filesystem::path CurrentExePath();
};

} // namespace cadence
