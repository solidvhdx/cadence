#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
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
    std::string tagName;        // e.g. "v1.0.4"
    SemVer      version{};
    std::string downloadUrl;    // Direct HTTPS link to Cadence.exe asset (empty if digest missing/invalid)
    std::string sha256Hex;      // 64-char lowercase hex SHA-256 digest from GitHub API asset metadata
    uint64_t    assetSize = 0;  // Optional expected byte size from GitHub API asset metadata
    std::string releaseUrl;     // GitHub release page URL (fallback)
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

    // Structurally parses GitHub's releases/latest JSON using Cadence's hardened JsonParser
    [[nodiscard]] static std::optional<ReleaseInfo> ParseGitHubReleaseJson(std::string_view json);

    // Queries GitHub Releases API synchronously (intended for background thread). Returns newer release if available.
    [[nodiscard]] static std::optional<ReleaseInfo> CheckForUpdate();

    // Validates and normalizes a 64-char hex SHA-256 string (returns lowercase hex or nullopt)
    [[nodiscard]] static std::optional<std::string> NormalizeSha256Hex(std::string_view hex) noexcept;

    // Computes SHA-256 hex digest of a file using Windows CNG (BCrypt)
    [[nodiscard]] static std::optional<std::string> ComputeFileSha256(const std::filesystem::path& filePath) noexcept;
    [[nodiscard]] static bool VerifyFileSha256(const std::filesystem::path& filePath, std::string_view expectedHex) noexcept;

    // Generates a CSPRNG-randomized temporary download path inside targetDir
    [[nodiscard]] static std::filesystem::path GenerateRandomTempPath(const std::filesystem::path& targetDir);

    using DownloadProgressCallback = std::function<void(uint64_t downloadedBytes, uint64_t totalBytes)>;

    // Downloads Cadence.exe over HTTPS into an exclusively-created non-reparse file, verifying SHA-256 and x64 PE64
    [[nodiscard]] static bool DownloadBinary(const std::string& url,
                                             const std::filesystem::path& destPath,
                                             std::string_view expectedSha256Hex,
                                             const DownloadProgressCallback& onProgress = nullptr,
                                             const std::atomic<bool>* cancelFlag = nullptr);

    // Validates that a file on disk is a genuine, non-truncated x64 Windows PE executable (not a DLL)
    [[nodiscard]] static bool VerifyPe64Binary(const std::filesystem::path& exePath) noexcept;

    // Swaps running exe -> .old, moves newExePath -> current exe, holds a FILE_SHARE_READ kernel lock while
    // re-verifying SHA-256 + PE64 on the final path, and spawns the updated process before releasing the lock.
    [[nodiscard]] static bool ApplyUpdateAndRelaunch(const std::filesystem::path& newExePath,
                                                     std::string_view expectedSha256Hex);

    // Removes leftover .old binary from a previous self-update (safe to call on every startup)
    static void CleanupOldBinary() noexcept;

    [[nodiscard]] static std::filesystem::path CurrentExePath();
};

} // namespace cadence
