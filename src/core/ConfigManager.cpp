#include "core/ConfigManager.hpp"
#include "core/JsonParser.hpp"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>

namespace cadence {

namespace {

std::string EscapeJson(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

template <typename T>
T GetNum(const JsonObject& obj, const std::string& key, T fallback) {
    auto it = obj.find(key);
    if (it != obj.end() && it->second.IsNumber()) {
        const double d = it->second.AsDouble();
        if (!std::isfinite(d)) return fallback;
        const double lo = static_cast<double>(std::numeric_limits<T>::lowest());
        const double hi = static_cast<double>(std::numeric_limits<T>::max());
        return static_cast<T>(std::clamp(d, lo, hi));
    }
    return fallback;
}

bool GetBool(const JsonObject& obj, const std::string& key, bool fallback) {
    auto it = obj.find(key);
    if (it != obj.end() && it->second.IsBool()) {
        return it->second.AsBool();
    }
    return fallback;
}

std::string GetStr(const JsonObject& obj, const std::string& key, const std::string& fallback) {
    auto it = obj.find(key);
    if (it != obj.end() && it->second.IsString()) {
        return it->second.AsString();
    }
    return fallback;
}

std::string WriteHotkey(const HotkeyBinding& hk) {
    return std::format(R"({{"source":{},"code":{}}})", static_cast<int>(hk.source), hk.code);
}

HotkeyBinding ReadHotkey(const JsonObject& obj, const std::string& key, HotkeyBinding fallback) {
    auto it = obj.find(key);
    if (it != obj.end() && it->second.IsObject()) {
        const auto& o = it->second.AsObject();
        HotkeyBinding hk{};
        hk.source = static_cast<HotkeySource>(std::clamp(GetNum<int>(o, "source", 0), 0, 1));
        hk.code   = static_cast<uint16_t>(std::clamp(GetNum<int>(o, "code", fallback.code), 1, 254));
        return hk;
    }
    return fallback;
}

std::string WriteCps(const CpsConfig& c) {
    return std::format(
        R"({{"rateCps":{:.2f},"displayAsMs":{},"button":{},"pattern":{},"burstCount":{},"burstCooldownMs":{},"holdDurationMs":{},"jitterEnabled":{},"jitterMs":{:.2f},"activation":{},"hotkey":{},"cursorMode":{},"lockX":{},"lockY":{},"lockedMicroWiggle":{},"limitMode":{},"limitValue":{}}})",
        c.rateCps,
        c.displayAsMs ? "true" : "false",
        static_cast<int>(c.button),
        static_cast<int>(c.pattern),
        c.burstCount,
        c.burstCooldownMs,
        c.holdDurationMs,
        c.jitterEnabled ? "true" : "false",
        c.jitterMs,
        static_cast<int>(c.activation),
        WriteHotkey(c.hotkey),
        static_cast<int>(c.cursorMode),
        c.lockX,
        c.lockY,
        c.lockedMicroWiggle ? "true" : "false",
        static_cast<int>(c.limitMode),
        c.limitValue
    );
}

void ReadCps(const JsonObject& o, CpsConfig& c) {
    c.rateCps           = std::clamp(GetNum<double>(o, "rateCps", c.rateCps), 0.5, 500.0);
    c.displayAsMs       = GetBool(o, "displayAsMs", c.displayAsMs);
    c.button            = static_cast<MouseButton>(std::clamp(GetNum<int>(o, "button", 0), 0, 2));
    c.pattern           = static_cast<ClickPattern>(std::clamp(GetNum<int>(o, "pattern", 0), 0, 3));
    c.burstCount        = std::clamp(GetNum<uint32_t>(o, "burstCount", c.burstCount), 2u, 20u);
    c.burstCooldownMs   = std::clamp(GetNum<uint32_t>(o, "burstCooldownMs", c.burstCooldownMs), 10u, 5000u);
    c.holdDurationMs    = std::clamp(GetNum<uint32_t>(o, "holdDurationMs", c.holdDurationMs), 1u, 250u);
    c.jitterEnabled     = GetBool(o, "jitterEnabled", c.jitterEnabled);
    c.jitterMs          = std::clamp(GetNum<double>(o, "jitterMs", c.jitterMs), 0.5, 50.0);
    c.activation        = static_cast<ActivationMode>(std::clamp(GetNum<int>(o, "activation", 0), 0, 1));
    c.hotkey            = ReadHotkey(o, "hotkey", c.hotkey);
    c.cursorMode        = static_cast<CursorMode>(std::clamp(GetNum<int>(o, "cursorMode", 0), 0, 1));
    c.lockX             = GetNum<int32_t>(o, "lockX", c.lockX);
    c.lockY             = GetNum<int32_t>(o, "lockY", c.lockY);
    c.lockedMicroWiggle = GetBool(o, "lockedMicroWiggle", c.lockedMicroWiggle);
    c.limitMode         = static_cast<RepeatLimitMode>(std::clamp(GetNum<int>(o, "limitMode", 0), 0, 2));
    c.limitValue        = std::clamp(GetNum<uint32_t>(o, "limitValue", c.limitValue), 1u, 1000000u);
}

std::string WriteKps(const KpsConfig& k) {
    std::string seq = "[";
    for (size_t i = 0; i < k.sequenceVks.size(); ++i) {
        if (i > 0) seq += ",";
        seq += std::to_string(k.sequenceVks[i]);
    }
    seq += "]";

    return std::format(
        R"({{"rateKps":{:.2f},"displayAsMs":{},"inputMode":{},"targetVk":{},"modCtrl":{},"modShift":{},"modAlt":{},"sequenceVks":{},"pressDurationMs":{},"jitterEnabled":{},"jitterMs":{:.2f},"dispatchMethod":{},"activation":{},"hotkey":{},"releaseOnStop":{},"limitMode":{},"limitValue":{}}})",
        k.rateKps,
        k.displayAsMs ? "true" : "false",
        static_cast<int>(k.inputMode),
        k.targetVk,
        k.modCtrl ? "true" : "false",
        k.modShift ? "true" : "false",
        k.modAlt ? "true" : "false",
        seq,
        k.pressDurationMs,
        k.jitterEnabled ? "true" : "false",
        k.jitterMs,
        static_cast<int>(k.dispatchMethod),
        static_cast<int>(k.activation),
        WriteHotkey(k.hotkey),
        k.releaseOnStop ? "true" : "false",
        static_cast<int>(k.limitMode),
        k.limitValue
    );
}

void ReadKps(const JsonObject& o, KpsConfig& k) {
    k.rateKps         = std::clamp(GetNum<double>(o, "rateKps", k.rateKps), 0.5, 500.0);
    k.displayAsMs     = GetBool(o, "displayAsMs", k.displayAsMs);
    k.inputMode       = static_cast<KeyInputMode>(std::clamp(GetNum<int>(o, "inputMode", 0), 0, 1));
    k.targetVk        = static_cast<uint16_t>(std::clamp(GetNum<int>(o, "targetVk", k.targetVk), 1, 254));
    k.modCtrl         = GetBool(o, "modCtrl", k.modCtrl);
    k.modShift        = GetBool(o, "modShift", k.modShift);
    k.modAlt          = GetBool(o, "modAlt", k.modAlt);
    if (auto it = o.find("sequenceVks"); it != o.end() && it->second.IsArray()) {
        std::vector<uint16_t> seq;
        for (const auto& item : it->second.AsArray()) {
            if (item.IsNumber()) {
                const int vk = static_cast<int>(item.AsDouble());
                if (vk >= 1 && vk <= 254) seq.push_back(static_cast<uint16_t>(vk));
            }
        }
        if (!seq.empty()) k.sequenceVks = std::move(seq);
    }
    k.pressDurationMs = std::clamp(GetNum<uint32_t>(o, "pressDurationMs", k.pressDurationMs), 1u, 500u);
    k.jitterEnabled   = GetBool(o, "jitterEnabled", k.jitterEnabled);
    k.jitterMs        = std::clamp(GetNum<double>(o, "jitterMs", k.jitterMs), 0.5, 50.0);
    k.dispatchMethod  = static_cast<KeyDispatchMethod>(std::clamp(GetNum<int>(o, "dispatchMethod", 0), 0, 1));
    k.activation      = static_cast<ActivationMode>(std::clamp(GetNum<int>(o, "activation", 1), 0, 1));
    k.hotkey          = ReadHotkey(o, "hotkey", k.hotkey);
    k.releaseOnStop   = GetBool(o, "releaseOnStop", k.releaseOnStop);
    k.limitMode       = static_cast<RepeatLimitMode>(std::clamp(GetNum<int>(o, "limitMode", 0), 0, 2));
    k.limitValue      = std::clamp(GetNum<uint32_t>(o, "limitValue", k.limitValue), 1u, 1000000u);
}

std::string WriteProfileObj(const Profile& p) {
    return std::format(
        R"({{"id":{},"name":{},"mode":{},"cps":{},"kps":{}}})",
        EscapeJson(p.id),
        EscapeJson(p.name),
        static_cast<int>(p.mode),
        WriteCps(p.cps),
        WriteKps(p.kps)
    );
}

bool ReadProfileObj(const JsonObject& o, Profile& p) {
    p.id   = GetStr(o, "id", "");
    p.name = GetStr(o, "name", "Unnamed");
    if (p.id.empty()) p.id = p.name;
    p.mode = static_cast<OperatingMode>(std::clamp(GetNum<int>(o, "mode", 0), 0, 1));
    if (auto it = o.find("cps"); it != o.end() && it->second.IsObject()) ReadCps(it->second.AsObject(), p.cps);
    if (auto it = o.find("kps"); it != o.end() && it->second.IsObject()) ReadKps(it->second.AsObject(), p.kps);
    return true;
}

} // namespace

std::filesystem::path ConfigManager::DefaultConfigPath() {
    PWSTR appData = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        base = appData;
        CoTaskMemFree(appData);
    } else {
        base = std::filesystem::current_path();
    }
    return base / L"Cadence" / L"config.json";
}

std::string ConfigManager::Serialize(const AppConfig& config) {
    const auto& s = config.settings;
    std::string settingsJson = std::format(
        R"({{"hybridQpcTimer":{},"disablePowerThrottle":{},"mmcssPriority":{},"stopOnSelfFocus":{},"targetWindowLock":{},"targetProcessName":{},"minimizeToTray":{},"alwaysOnTop":{},"checkForUpdates":{},"panicHotkey":{}}})",
        s.hybridQpcTimer ? "true" : "false",
        s.disablePowerThrottle ? "true" : "false",
        s.mmcssPriority ? "true" : "false",
        s.stopOnSelfFocus ? "true" : "false",
        s.targetWindowLock ? "true" : "false",
        EscapeJson(s.targetProcessName),
        s.minimizeToTray ? "true" : "false",
        s.alwaysOnTop ? "true" : "false",
        s.checkForUpdates ? "true" : "false",
        WriteHotkey(s.panicHotkey)
    );

    std::string profilesJson = "[";
    for (size_t i = 0; i < config.profiles.size(); ++i) {
        if (i > 0) profilesJson += ",\n    ";
        profilesJson += WriteProfileObj(config.profiles[i]);
    }
    profilesJson += "]";

    return std::format(
        "{{\n  \"version\": 1,\n  \"activeMode\": {},\n  \"activeProfileId\": {},\n  \"cps\": {},\n  \"kps\": {},\n  \"settings\": {},\n  \"profiles\": [\n    {}\n  ]\n}}\n",
        static_cast<int>(config.activeMode),
        EscapeJson(config.activeProfileId),
        WriteCps(config.cps),
        WriteKps(config.kps),
        settingsJson,
        profilesJson.substr(1, profilesJson.size() - 2)
    );
}

bool ConfigManager::Deserialize(const std::string& jsonText, AppConfig& outConfig) {
    JsonParser parser(jsonText);
    JsonValue root;
    if (!parser.Parse(root) || !root.IsObject()) {
        return false;
    }
    const auto& o = root.AsObject();
    outConfig.activeMode      = static_cast<OperatingMode>(std::clamp(GetNum<int>(o, "activeMode", 0), 0, 1));
    outConfig.activeProfileId = GetStr(o, "activeProfileId", "default");

    if (auto it = o.find("cps"); it != o.end() && it->second.IsObject()) {
        ReadCps(it->second.AsObject(), outConfig.cps);
    }
    if (auto it = o.find("kps"); it != o.end() && it->second.IsObject()) {
        ReadKps(it->second.AsObject(), outConfig.kps);
    }
    if (auto it = o.find("settings"); it != o.end() && it->second.IsObject()) {
        const auto& so = it->second.AsObject();
        outConfig.settings.hybridQpcTimer       = GetBool(so, "hybridQpcTimer", true);
        outConfig.settings.disablePowerThrottle = GetBool(so, "disablePowerThrottle", true);
        outConfig.settings.mmcssPriority        = GetBool(so, "mmcssPriority", true);
        outConfig.settings.stopOnSelfFocus      = GetBool(so, "stopOnSelfFocus", true);
        outConfig.settings.targetWindowLock     = GetBool(so, "targetWindowLock", false);
        outConfig.settings.targetProcessName    = GetStr(so, "targetProcessName", "");
        outConfig.settings.minimizeToTray       = GetBool(so, "minimizeToTray", true);
        outConfig.settings.alwaysOnTop          = GetBool(so, "alwaysOnTop", false);
        outConfig.settings.checkForUpdates      = GetBool(so, "checkForUpdates", true);
        outConfig.settings.panicHotkey          = ReadHotkey(so, "panicHotkey", outConfig.settings.panicHotkey);
    }
    if (auto it = o.find("profiles"); it != o.end() && it->second.IsArray()) {
        outConfig.profiles.clear();
        for (const auto& pv : it->second.AsArray()) {
            if (pv.IsObject()) {
                Profile p{};
                if (ReadProfileObj(pv.AsObject(), p)) {
                    outConfig.profiles.push_back(std::move(p));
                }
            }
        }
    }
    return true;
}

std::string ConfigManager::SerializeProfile(const Profile& profile) {
    return WriteProfileObj(profile);
}

bool ConfigManager::DeserializeProfile(const std::string& jsonText, Profile& outProfile) {
    JsonParser parser(jsonText);
    JsonValue root;
    if (!parser.Parse(root) || !root.IsObject()) {
        return false;
    }
    return ReadProfileObj(root.AsObject(), outProfile);
}

bool ConfigManager::LoadFromFile(const std::filesystem::path& path, AppConfig& outConfig) {
    constexpr uintmax_t kMaxConfigBytes = 2 * 1024 * 1024; // 2 MB cap against memory DoS
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz == 0 || sz > kMaxConfigBytes) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string buf(static_cast<size_t>(sz), '\0');
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!in && !in.eof()) return false;
    buf.resize(static_cast<size_t>(in.gcount()));
    return Deserialize(buf, outConfig);
}

bool ConfigManager::SaveToFile(const std::filesystem::path& path, const AppConfig& config) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const auto tmpPath = path.wstring() + L".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string payload = Serialize(config);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }
    if (MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
    return true;
}

} // namespace cadence
