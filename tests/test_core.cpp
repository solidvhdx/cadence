#include "TestHarness.hpp"

#include "core/ConfigManager.hpp"
#include "core/PerformanceMonitor.hpp"
#include "core/ProfileManager.hpp"
#include "core/StringUtil.hpp"
#include "core/TimingEngine.hpp"
#include "core/UpdateChecker.hpp"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace cadence;

namespace {

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Fresh, empty directory under %TEMP% for one test
std::filesystem::path TempDir(const wchar_t* name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

const std::string kSampleDigest      = "37F0F4B7201C021CCB9F0ADB28AD408177668327E1DDB8E8231EEC97E12DCE06";
const std::string kSampleDigestLower = "37f0f4b7201c021ccb9f0adb28ad408177668327e1ddb8e8231eec97e12dce06";
const std::string kReleaseExeUrl     = "https://github.com/solidvhdx/cadence/releases/download/v1.0.4/Cadence.exe";

} // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

CADENCE_TEST(ConfigRoundTrip) {
    AppConfig orig{};
    orig.activeMode            = OperatingMode::KPS;
    orig.cps.rateCps           = 17.5;
    orig.cps.button            = MouseButton::Right;
    orig.cps.pattern           = ClickPattern::CustomBurst;
    orig.cps.burstCount        = 4;
    orig.cps.cursorMode        = CursorMode::LockedPoint;
    orig.cps.lockX             = 1234;
    orig.cps.lockY             = 567;
    orig.cps.hotkey            = {HotkeySource::Mouse, VK_XBUTTON2};
    orig.cps.lockedMicroWiggle = false;

    orig.kps.rateKps         = 22.0;
    orig.kps.targetVk        = 'Q';
    orig.kps.modCtrl         = true;
    orig.kps.modShift        = true;
    orig.kps.inputMode       = KeyInputMode::Sequence;
    orig.kps.sequenceVks     = {'1', '2', '3', '4', 'E'};
    orig.kps.pressDurationMs = 45;
    orig.kps.dispatchMethod  = KeyDispatchMethod::VirtualKey;
    orig.kps.hotkey          = {HotkeySource::Keyboard, VK_F8};

    orig.settings.stopOnSelfFocus   = true;
    orig.settings.hybridQpcTimer    = true;
    orig.settings.targetWindowLock  = true;
    orig.settings.targetProcessName = "RobloxPlayerBeta.exe";
    orig.settings.checkForUpdates   = false;

    ProfileManager pm(orig);
    pm.CreateFromCurrent("Unit Test Preset");

    const auto dir  = TempDir(L"cadence_test_roundtrip");
    const auto path = dir / L"config.json";
    CADENCE_REQUIRE(ConfigManager::SaveToFile(path, orig), "Atomic save config to disk");
    CADENCE_CHECK(!std::filesystem::exists(dir / L"config.json.tmp"), "Temporary file is renamed away after saving");

    AppConfig loaded{};
    CADENCE_REQUIRE(ConfigManager::LoadFromFile(path, loaded).has_value(), "Load config from disk");

    CADENCE_CHECK(loaded.activeMode == OperatingMode::KPS, "Active mode preserved");
    CADENCE_CHECK(std::abs(loaded.cps.rateCps - 17.5) < 0.01, "CPS rate preserved independently");
    CADENCE_CHECK(loaded.cps.button == MouseButton::Right, "CPS mouse button preserved");
    CADENCE_CHECK(loaded.cps.pattern == ClickPattern::CustomBurst, "CPS pattern preserved");
    CADENCE_CHECK(loaded.cps.lockX == 1234 && loaded.cps.lockY == 567, "CPS locked coords preserved");
    CADENCE_CHECK(!loaded.cps.lockedMicroWiggle, "CPS micro-wiggle flag preserved");
    CADENCE_CHECK(loaded.cps.hotkey == orig.cps.hotkey, "CPS mouse X2 hotkey preserved");
    CADENCE_CHECK(std::abs(loaded.kps.rateKps - 22.0) < 0.01, "KPS rate preserved independently");
    CADENCE_CHECK(loaded.kps.targetVk == 'Q' && loaded.kps.modCtrl && loaded.kps.modShift, "KPS chord preserved");
    CADENCE_CHECK(loaded.kps.sequenceVks.size() == 5 && loaded.kps.sequenceVks[4] == 'E', "KPS sequence preserved");
    CADENCE_CHECK(loaded.settings.stopOnSelfFocus, "Self-focus safety setting preserved");
    CADENCE_CHECK(loaded.settings.targetWindowLock && loaded.settings.targetProcessName == "RobloxPlayerBeta.exe",
                  "Target window lock preserved");
    CADENCE_CHECK(!loaded.settings.checkForUpdates, "checkForUpdates setting preserved");
    CADENCE_CHECK(loaded.profiles.size() == orig.profiles.size(), "All profiles preserved");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

CADENCE_TEST(ConfigParserHardening) {
    // RFC-8259 \uXXXX Unicode escape decoding
    Profile unicodeProf{};
    CADENCE_CHECK(ConfigManager::DeserializeProfile(R"({"id":"u1","name":"Präzision","mode":0})", unicodeProf),
                  "Unicode JSON parse");
    CADENCE_CHECK(unicodeProf.name == "Pr\xC3\xA4zision", "Unicode \\u00e4 decoded to UTF-8");

    // Recursion depth cap & non-finite numbers
    AppConfig malformed{};
    const std::string deepJson = std::string(100, '[') + std::string(100, ']');
    CADENCE_CHECK(!ConfigManager::Deserialize(deepJson, malformed), "Deeply nested JSON (>64 levels) rejected");
    CADENCE_CHECK(!ConfigManager::Deserialize(R"({"activeMode": 1e9999})", malformed), "Non-finite JSON number rejected");

    AppConfig clamped{};
    CADENCE_CHECK(ConfigManager::Deserialize(R"({"activeMode": -999, "cps": {"burstCount": 999999999999999, "rateCps": -50}})", clamped),
                  "Out-of-range finite JSON numbers parsed safely");
    CADENCE_CHECK(clamped.cps.rateCps >= 0.5 && clamped.cps.rateCps <= 500.0, "Out-of-range CPS rate clamped");
    CADENCE_CHECK(clamped.cps.burstCount >= 2 && clamped.cps.burstCount <= 20, "Out-of-range burstCount clamped");
}

CADENCE_TEST(ConfigSequenceAndCoordsCapped) {
    std::string seq = "[1e300";
    for (int i = 0; i < 40; ++i) {
        seq += "," + std::to_string(65 + (i % 26));
    }
    seq += "]";

    AppConfig cfg{};
    CADENCE_REQUIRE(ConfigManager::Deserialize(R"({"kps":{"sequenceVks":)" + seq +
                                                   R"(},"cps":{"lockX":2147483647,"lockY":-2147483648}})", cfg),
                    "Config with oversized values parses");
    CADENCE_CHECK(cfg.kps.sequenceVks.size() == kMaxSequenceKeys, "KPS sequence capped to the UI limit");
    CADENCE_CHECK(!cfg.kps.sequenceVks.empty() && cfg.kps.sequenceVks.front() == 65, "Out-of-range sequence value skipped");
    CADENCE_CHECK(cfg.cps.lockX == 1'000'000 && cfg.cps.lockY == -1'000'000, "Locked coordinates clamped");
}

CADENCE_TEST(ConfigLoadErrorsAndBackup) {
    const auto dir  = TempDir(L"cadence_test_load_errors");
    const auto path = dir / L"config.json";

    AppConfig cfg{};
    const auto missing = ConfigManager::LoadFromFile(path, cfg);
    CADENCE_CHECK(!missing && missing.error() == ConfigLoadError::NotFound, "Missing file reports NotFound");

    const std::string damaged = "{ \"activeMode\": 1, this is not json";
    WriteTextFile(path, damaged);
    cfg.cps.rateCps = 33.0;
    const auto broken = ConfigManager::LoadFromFile(path, cfg);
    CADENCE_CHECK(!broken && broken.error() == ConfigLoadError::ParseFailed, "Damaged file reports ParseFailed");
    CADENCE_CHECK(std::abs(cfg.cps.rateCps - 33.0) < 0.01, "Failed load leaves the config untouched");

    const auto backup = ConfigManager::BackupFile(path);
    CADENCE_REQUIRE(backup.has_value(), "Backup of damaged config created");
    CADENCE_CHECK(*backup == dir / L"config.json.bak", "Backup named config.json.bak");
    CADENCE_CHECK(ReadTextFile(*backup) == damaged, "Backup holds the original bytes");

    WriteTextFile(path, "");
    const auto empty = ConfigManager::LoadFromFile(path, cfg);
    CADENCE_CHECK(!empty && empty.error() == ConfigLoadError::ParseFailed, "Empty file reports ParseFailed");
    CADENCE_CHECK(!DescribeConfigLoadError(ConfigLoadError::ParseFailed).empty(), "Load errors have a description");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

CADENCE_TEST(ConfigVersioning) {
    const std::string json = ConfigManager::Serialize(AppConfig{});
    CADENCE_CHECK(json.find("\"version\": " + std::to_string(kConfigVersion)) != std::string::npos,
                  "Serialized config carries the current format version");

    AppConfig current{};
    CADENCE_CHECK(ConfigManager::Deserialize(json, current) && current.loadedVersion == kConfigVersion,
                  "Current version read back");

    AppConfig legacy{};
    CADENCE_CHECK(ConfigManager::Deserialize(R"({"activeMode":0})", legacy) && legacy.loadedVersion == 1,
                  "Config without version field treated as v1");

    AppConfig future{};
    CADENCE_CHECK(ConfigManager::Deserialize(R"({"version": 99})", future) && future.loadedVersion == 99,
                  "Newer format version is detected");
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

CADENCE_TEST(ProfileActivationIsolation) {
    AppConfig cfg{};
    cfg.cps.rateCps = 19.5;
    cfg.kps.rateKps = 13.0;
    ProfileManager pm(cfg);

    // Switching to a KPS profile must NOT wipe the user's current CPS rate
    CADENCE_CHECK(pm.ActivateProfile("skill-spam-e"), "Activate built-in KPS profile");
    CADENCE_CHECK(cfg.activeMode == OperatingMode::KPS, "Switched to KPS mode");
    CADENCE_CHECK(std::abs(cfg.cps.rateCps - 19.5) < 0.01, "CPS config preserved when activating KPS profile");

    // Deleting last profile must be prevented
    while (cfg.profiles.size() > 1) {
        CADENCE_REQUIRE(pm.DeleteProfile(cfg.profiles.back().id), "Delete extra profile");
    }
    CADENCE_CHECK(!pm.DeleteProfile(cfg.profiles.front().id), "Prevent deleting final remaining profile");
}

CADENCE_TEST(ProfileImportPreviewAndAdd) {
    AppConfig cfg{};
    ProfileManager pm(cfg);
    const std::string activeBefore = cfg.activeProfileId;

    Profile shared{};
    shared.id   = "default"; // Collides with the built-in profile
    shared.name = "Imported";
    shared.mode = OperatingMode::KPS;

    const auto dir  = TempDir(L"cadence_test_import");
    const auto path = dir / L"profile.json";
    WriteTextFile(path, ConfigManager::SerializeProfile(shared));

    Profile preview{};
    CADENCE_REQUIRE(ProfileManager::ReadProfileFile(path, preview), "Profile file readable for preview");
    CADENCE_CHECK(preview.name == "Imported", "Preview shows the file's profile");
    CADENCE_CHECK(cfg.activeProfileId == activeBefore, "Reading a file changes nothing");

    const std::string newId = pm.AddImportedProfile(preview);
    CADENCE_CHECK(newId != "default", "Colliding profile id renamed");
    CADENCE_CHECK(pm.FindById(newId) != nullptr, "Imported profile added");
    CADENCE_CHECK(cfg.activeProfileId == activeBefore, "Imported profile is not activated automatically");

    WriteTextFile(path, std::string(70 * 1024, ' '));
    Profile oversized{};
    CADENCE_CHECK(!ProfileManager::ReadProfileFile(path, oversized), "Oversized profile file rejected");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Timing & telemetry
// ---------------------------------------------------------------------------

CADENCE_TEST(TimingUnitConversions) {
    TimingEngine timing;
    const int64_t oneMs = timing.ToQpcTicks(Millis{1.0});
    CADENCE_CHECK(oneMs > 0, "1 ms maps to a positive tick count");
    CADENCE_CHECK(std::abs((timing.QpcToMillis(oneMs) - Millis{1.0}).count()) < 0.01, "ms -> ticks -> ms round trip");
    CADENCE_CHECK(std::abs(timing.QpcToMicros(timing.QpcFrequency()).count() - 1'000'000.0) < 1.0,
                  "One QPC second equals 1,000,000 us");
    CADENCE_CHECK(timing.SpinGuard() >= Micros{320.0} && timing.SpinGuard() <= Micros{620.0},
                  "Calibrated spin guard within bounds");
}

CADENCE_TEST(TimingPrecisionAndJitter) {
    TimingEngine timing;
    CADENCE_CHECK(timing.IsHighResTimerAvailable(), "Windows high-resolution waitable timer available");
    CADENCE_CHECK(TimingEngine::SetPowerThrottlingDisabled(true), "Power throttling override succeeds");

    // Jitter bounds over 2000 samples
    double sum = 0.0;
    bool inBounds = true;
    for (int i = 0; i < 2000; ++i) {
        const Millis j = timing.SampleJitter(Millis{5.0});
        inBounds = inBounds && (j >= Millis{-5.0} && j <= Millis{5.0});
        sum += j.count();
    }
    CADENCE_CHECK(inBounds, "Jitter strictly clamped within [-max, +max]");
    CADENCE_CHECK(std::abs(sum / 2000.0) < 0.4, "Gaussian jitter centered near 0");

    // Cancellation must wake in < 10 ms even on a 5000 ms wait
    std::atomic<bool> abort{false};
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const int64_t t0 = timing.NowQpc();
    abort.store(true);
    SetEvent(ev);
    const bool completed = timing.WaitFor(Millis{5000.0}, ev, abort, true);
    const Millis cancelTime = timing.QpcToMillis(timing.NowQpc() - t0);
    CloseHandle(ev);
    CADENCE_CHECK(!completed, "Cancelled wait returns false");
    CADENCE_CHECK(cancelTime < Millis{10.0}, "Cancellation wakes immediately");

    // Real 20 ms precision wait
    abort.store(false);
    HANDLE ev2 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const int64_t s0 = timing.NowQpc();
    CADENCE_CHECK(timing.WaitFor(Millis{20.0}, ev2, abort, true), "20 ms hybrid wait completes");
    const Millis actual20 = timing.QpcToMillis(timing.NowQpc() - s0);
    CloseHandle(ev2);
#if defined(__SANITIZE_ADDRESS__)
    const Millis upperBound{30.0}; // Instrumented builds run slower
#else
    const Millis upperBound{22.0};
#endif
    CADENCE_CHECK(actual20 >= Millis{19.0} && actual20 <= upperBound, "20 ms hybrid wait accuracy within tight bounds");
    std::cout << "    20 ms target -> " << actual20.count() << " ms\n";
}

CADENCE_TEST(PerformanceMonitorRates) {
    TimingEngine timing;
    PerformanceMonitor pm;
    const int64_t freq = timing.QpcFrequency();
    const auto initSnap = pm.ComputeSnapshot(timing.NowQpc(), freq, false);
    CADENCE_CHECK(initSnap.memoryMb > 0.0, "Initial memory usage sampled immediately on construction");

    int64_t t = timing.NowQpc() - freq; // 1 second of 20 CPS events (50 ms each)
    for (int i = 0; i < 20; ++i) {
        t += freq / 20;
        pm.RecordEvent(t, Millis{50.0}, Millis{50.0});
    }
    const auto snap = pm.ComputeSnapshot(t, freq, true);
    CADENCE_CHECK(snap.sessionCount == 20, "Session count matches recorded events");
    CADENCE_CHECK(std::abs(snap.avgIntervalMs - 50.0) < 0.01, "Average interval exact");
    CADENCE_CHECK(snap.actualRate >= 19.8 && snap.actualRate <= 20.2, "Measured CPS matches 20.0 CPS");

    // ResetSession isolates the next session; fractional 14.5 CPS measured immediately
    pm.ResetSession();
    const int64_t step145 = static_cast<int64_t>(static_cast<double>(freq) / 14.5);
    int64_t t2 = timing.NowQpc() - step145 * 4;
    for (int i = 0; i < 4; ++i) {
        t2 += step145;
        pm.RecordEvent(t2, Millis{1000.0 / 14.5}, Millis{1000.0 / 14.5});
    }
    const auto snap2 = pm.ComputeSnapshot(t2, freq, true);
    CADENCE_CHECK(snap2.sessionCount == 4, "ResetSession isolates session count");
    CADENCE_CHECK(std::abs(snap2.actualRate - 14.5) < 0.15, "Fractional 14.5 CPS measured accurately");
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

CADENCE_TEST(StringUtilRoundTrip) {
    const std::string  utf8 = "Pr\xC3\xA4zision \xF0\x9F\x9A\x80";
    const std::wstring wide = Utf8ToWide(utf8);
    CADENCE_CHECK(wide == L"Präzision \U0001F680", "UTF-8 -> UTF-16 incl. surrogate pair");
    CADENCE_CHECK(WideToUtf8(wide) == utf8, "UTF-16 -> UTF-8 round trip");
    CADENCE_CHECK(Utf8ToWide("").empty() && WideToUtf8(L"").empty(), "Empty strings stay empty");
}

// ---------------------------------------------------------------------------
// Updater
// ---------------------------------------------------------------------------

CADENCE_TEST(UpdateSemVer) {
    const auto v1 = UpdateChecker::ParseSemVer("v1.2.3");
    CADENCE_CHECK(v1 && v1->major == 1 && v1->minor == 2 && v1->patch == 3, "ParseSemVer v1.2.3");
    CADENCE_CHECK(UpdateChecker::IsNewerVersion("v1.0.2", SemVer{1, 0, 1}), "v1.0.2 > 1.0.1");
    CADENCE_CHECK(UpdateChecker::IsNewerVersion("v2.0.0", SemVer{1, 9, 9}), "v2.0.0 > 1.9.9");
    CADENCE_CHECK(!UpdateChecker::IsNewerVersion("v1.0.1", SemVer{1, 0, 1}), "v1.0.1 == 1.0.1 not newer");
    CADENCE_CHECK(!UpdateChecker::IsNewerVersion("v1.0.0", SemVer{1, 0, 1}), "v1.0.0 < 1.0.1 not newer");
}

CADENCE_TEST(UpdateReleaseJson) {
    const std::string sampleJson = R"({
        "body": "Decoy text containing \"tag_name\": \"v9.9.9\" and \"browser_download_url\": \"https://evil.example/Cadence.exe\" 🚀",
        "author": {
            "login": "solidvhdx",
            "html_url": "https://github.com/solidvhdx"
        },
        "tag_name": "v1.2.0",
        "html_url": "https://github.com/solidvhdx/cadence/releases/tag/v1.2.0",
        "assets": [
            {"name": "SHA256SUMS.txt", "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000", "browser_download_url": "https://github.com/solidvhdx/cadence/releases/download/v1.2.0/SHA256SUMS.txt"},
            {"name": "Cadence.exe", "size": 738816, "digest": "sha256:)" + kSampleDigest + R"(", "browser_download_url": "https://github.com/solidvhdx/cadence/releases/download/v1.2.0/Cadence.exe"}
        ]
    })";
    const auto rel = UpdateChecker::ParseGitHubReleaseJson(sampleJson);
    CADENCE_REQUIRE(rel.has_value(), "ParseGitHubReleaseJson succeeds with structured parser & surrogate escapes");
    CADENCE_CHECK(rel->tagName == "v1.2.0", "Parsed top-level tag_name (ignores body decoy)");
    CADENCE_CHECK(rel->releaseUrl == "https://github.com/solidvhdx/cadence/releases/tag/v1.2.0", "Release html_url ignores author.html_url");
    CADENCE_CHECK(rel->downloadUrl == "https://github.com/solidvhdx/cadence/releases/download/v1.2.0/Cadence.exe", "Extracted Cadence.exe URL");
    CADENCE_CHECK(rel->sha256Hex == kSampleDigestLower, "Extracted & normalized SHA-256 digest");
    CADENCE_CHECK(rel->assetSize == 738816, "Extracted Cadence.exe asset size");
    CADENCE_CHECK(rel->releaseNotes.starts_with("Decoy text") && rel->releaseNotes.ends_with("\xF0\x9F\x9A\x80"),
                  "Release notes extracted with decoded emoji");

    // Fail-closed: asset without SHA-256 digest leaves downloadUrl empty
    const auto noDigest = UpdateChecker::ParseGitHubReleaseJson(R"({
        "tag_name": "v1.3.0",
        "html_url": "https://github.com/solidvhdx/cadence/releases/tag/v1.3.0",
        "assets": [{"name": "Cadence.exe", "browser_download_url": "https://github.com/solidvhdx/cadence/releases/download/v1.3.0/Cadence.exe"}]
    })");
    CADENCE_CHECK(noDigest.has_value() && noDigest->downloadUrl.empty() && noDigest->sha256Hex.empty(),
                  "Missing asset digest fails closed");

    // Release notes are sanitized and capped
    const auto ctrl = UpdateChecker::ParseGitHubReleaseJson(R"({"tag_name":"v1.4.0","body":"line1\r\nline2\u0007\tend"})");
    CADENCE_CHECK(ctrl && ctrl->releaseNotes == "line1\nline2 end", "CRLF, BEL and tab normalized in release notes");
    const auto big = UpdateChecker::ParseGitHubReleaseJson(R"({"tag_name":"v1.5.0","body":")" + std::string(5000, 'a') + R"("})");
    CADENCE_CHECK(big && big->releaseNotes.size() == 4096, "Release notes capped at 4 KB");
}

CADENCE_TEST(UpdateSha256AndPeChecks) {
    const auto shaTestPath  = UpdateChecker::GenerateRandomTempPath(std::filesystem::temp_directory_path());
    const auto shaTestPath2 = UpdateChecker::GenerateRandomTempPath(std::filesystem::temp_directory_path());
    CADENCE_REQUIRE(!shaTestPath.empty() && !shaTestPath2.empty() && shaTestPath != shaTestPath2,
                    "GenerateRandomTempPath produces distinct CSPRNG paths");
    WriteTextFile(shaTestPath, "abc");
    const std::string kNistAbcSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto computedSha = UpdateChecker::ComputeFileSha256(shaTestPath);
    CADENCE_CHECK(computedSha && *computedSha == kNistAbcSha256, "CNG SHA-256 matches NIST 'abc' vector");
    CADENCE_CHECK(UpdateChecker::VerifyFileSha256(shaTestPath, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"),
                  "VerifyFileSha256 normalizes uppercase hex");
    CADENCE_CHECK(!UpdateChecker::VerifyFileSha256(shaTestPath, kSampleDigestLower), "VerifyFileSha256 rejects mismatched digest");
    std::error_code rmEc;
    std::filesystem::remove(shaTestPath, rmEc);

    // PE64 validation accepts a real x64 exe and rejects a fake one
    const auto selfExe        = UpdateChecker::CurrentExePath();
    const auto siblingCadence = selfExe.parent_path() / L"Cadence.exe";
    const auto validExe       = std::filesystem::exists(siblingCadence) ? siblingCadence : selfExe;
    CADENCE_CHECK(UpdateChecker::VerifyPe64Binary(validExe), "VerifyPe64Binary accepts genuine x64 PE executable");

    // FILE_SHARE_READ lock blocks concurrent writers
    {
        HANDLE hLock = CreateFileW(validExe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        CADENCE_REQUIRE(hLock != INVALID_HANDLE_VALUE, "Open validExe with FILE_SHARE_READ lock");
        HANDLE hWrite = CreateFileW(validExe.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD writeErr = GetLastError();
        CADENCE_CHECK(hWrite == INVALID_HANDLE_VALUE && writeErr == ERROR_SHARING_VIOLATION,
                      "Concurrent writer blocked with ERROR_SHARING_VIOLATION while lock held");
        if (hWrite != INVALID_HANDLE_VALUE) CloseHandle(hWrite);
        CloseHandle(hLock);
    }
    // CreateProcessW succeeds while holding the FILE_SHARE_READ lock on the target executable
    {
        wchar_t sysDir[MAX_PATH]{};
        const UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
        CADENCE_REQUIRE(sysLen > 0, "GetSystemDirectoryW succeeds");
        const std::filesystem::path cmdExe = std::filesystem::path(sysDir) / L"cmd.exe";
        HANDLE hLock = CreateFileW(cmdExe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        CADENCE_REQUIRE(hLock != INVALID_HANDLE_VALUE, "Open cmd.exe with FILE_SHARE_READ lock");

        std::wstring cmdLine = L"\"" + cmdExe.wstring() + L"\" /c exit 0";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL spawned = CreateProcessW(cmdExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CADENCE_CHECK(spawned != FALSE, "CreateProcessW succeeds while holding FILE_SHARE_READ lock");
        if (spawned) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        CloseHandle(hLock);
    }

    const auto fakePePath = std::filesystem::temp_directory_path() / L"cadence_unit_test_fake.exe";
    {
        std::string bogus(70 * 1024, '\0');
        bogus[0] = 'M';
        bogus[1] = 'Z';
        WriteTextFile(fakePePath, bogus);
    }
    CADENCE_CHECK(!UpdateChecker::VerifyPe64Binary(fakePePath), "VerifyPe64Binary rejects fake MZ file");
    std::filesystem::remove(fakePePath, rmEc);
}

CADENCE_TEST(UpdateErrorReasons) {
    const auto dest = UpdateChecker::GenerateRandomTempPath(std::filesystem::temp_directory_path());
    CADENCE_REQUIRE(!dest.empty(), "Temp path for download tests");

    std::atomic<bool> preCancel{true};
    const auto cancelled = UpdateChecker::DownloadBinary(kReleaseExeUrl, dest, kSampleDigestLower, nullptr, &preCancel);
    CADENCE_CHECK(!cancelled && cancelled.error() == UpdateError::Cancelled, "Pre-cancelled download reports Cancelled");

    const auto badHash = UpdateChecker::DownloadBinary(kReleaseExeUrl, dest, "not-a-valid-sha256", nullptr, nullptr);
    CADENCE_CHECK(!badHash && badHash.error() == UpdateError::InvalidRequest, "Invalid digest rejected before network");

    const auto foreign = UpdateChecker::DownloadBinary("https://evil.example/Cadence.exe", dest, kSampleDigestLower, nullptr, nullptr);
    CADENCE_CHECK(!foreign && foreign.error() == UpdateError::InvalidRequest, "Download URL outside the repo rejected");
    CADENCE_CHECK(!std::filesystem::exists(dest), "Failed downloads leave no file behind");

    const auto apply = UpdateChecker::ApplyUpdateAndRelaunch(dest, "not-a-valid-sha256");
    CADENCE_CHECK(!apply && apply.error() == UpdateError::InvalidRequest, "Apply refuses an invalid digest before touching files");

    CADENCE_CHECK(!DescribeUpdateError(UpdateError::HashMismatch).empty() &&
                  !DescribeUpdateError(UpdateError::ReplaceFailed).empty(),
                  "Every update error has a user-facing description");
}

int main(int argc, char** argv) {
    std::cout << "=== Cadence - Core Verification Tests ===\n";
    return cadence::test::RunTests(argc, argv);
}
