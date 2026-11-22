#include "core/ConfigManager.hpp"

#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <variant>

namespace cadence {

namespace {

// Minimal self-contained RFC-8259 JSON DOM (zero third-party dependencies)
struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray  = std::vector<JsonValue>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> data{nullptr};

    [[nodiscard]] bool IsObject() const noexcept { return std::holds_alternative<JsonObject>(data); }
    [[nodiscard]] bool IsArray()  const noexcept { return std::holds_alternative<JsonArray>(data); }
    [[nodiscard]] bool IsString() const noexcept { return std::holds_alternative<std::string>(data); }
    [[nodiscard]] bool IsNumber() const noexcept { return std::holds_alternative<double>(data); }
    [[nodiscard]] bool IsBool()   const noexcept { return std::holds_alternative<bool>(data); }

    [[nodiscard]] const JsonObject& AsObject() const { return std::get<JsonObject>(data); }
    [[nodiscard]] const JsonArray&  AsArray()  const { return std::get<JsonArray>(data); }
    [[nodiscard]] const std::string& AsString() const { return std::get<std::string>(data); }
    [[nodiscard]] double AsDouble() const { return std::get<double>(data); }
    [[nodiscard]] bool   AsBool()   const { return std::get<bool>(data); }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : m_src(src) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return m_pos == m_src.size();
    }

private:
    static constexpr int kMaxDepth = 64;

    void SkipWs() noexcept {
        while (m_pos < m_src.size() && (m_src[m_pos] == ' ' || m_src[m_pos] == '\t' || m_src[m_pos] == '\r' || m_src[m_pos] == '\n')) {
            ++m_pos;
        }
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        SkipWs();
        if (m_pos >= m_src.size()) return false;
        const char c = m_src[m_pos];
        if (c == '{') return ParseObject(out, depth + 1);
        if (c == '[') return ParseArray(out, depth + 1);
        if (c == '"') {
            std::string s;
            if (!ParseString(s)) return false;
            out.data = std::move(s);
            return true;
        }
        if (c == 't' && m_src.substr(m_pos, 4) == "true")  { m_pos += 4; out.data = true; return true; }
        if (c == 'f' && m_src.substr(m_pos, 5) == "false") { m_pos += 5; out.data = false; return true; }
        if (c == 'n' && m_src.substr(m_pos, 4) == "null")  { m_pos += 4; out.data = nullptr; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        return false;
    }

    bool ParseObject(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        ++m_pos; // '{'
        JsonObject obj;
        SkipWs();
        if (m_pos < m_src.size() && m_src[m_pos] == '}') {
            ++m_pos;
            out.data = std::move(obj);
            return true;
        }
        while (m_pos < m_src.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (m_pos >= m_src.size() || m_src[m_pos] != ':') return false;
            ++m_pos;
            JsonValue val;
            if (!ParseValue(val, depth)) return false;
            obj.emplace(std::move(key), std::move(val));
            SkipWs();
            if (m_pos >= m_src.size()) return false;
            if (m_src[m_pos] == '}') {
                ++m_pos;
                out.data = std::move(obj);
                return true;
            }
            if (m_src[m_pos] != ',') return false;
            ++m_pos;
        }
        return false;
    }

    bool ParseArray(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return false;
        ++m_pos; // '['
        JsonArray arr;
        SkipWs();
        if (m_pos < m_src.size() && m_src[m_pos] == ']') {
            ++m_pos;
            out.data = std::move(arr);
            return true;
        }
        while (m_pos < m_src.size()) {
            JsonValue val;
            if (!ParseValue(val, depth)) return false;
            arr.push_back(std::move(val));
            SkipWs();
            if (m_pos >= m_src.size()) return false;
            if (m_src[m_pos] == ']') {
                ++m_pos;
                out.data = std::move(arr);
                return true;
            }
            if (m_src[m_pos] != ',') return false;
            ++m_pos;
        }
        return false;
    }

    bool ParseString(std::string& out) {
        if (m_pos >= m_src.size() || m_src[m_pos] != '"') return false;
        ++m_pos;
        out.clear();
        while (m_pos < m_src.size()) {
            const char c = m_src[m_pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (m_pos >= m_src.size()) return false;
                const char esc = m_src[m_pos++];
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (m_pos + 4 > m_src.size()) return false;
                        uint32_t cp = 0;
                        const auto [ptr, ec] = std::from_chars(m_src.data() + m_pos, m_src.data() + m_pos + 4, cp, 16);
                        if (ec != std::errc{} || ptr != m_src.data() + m_pos + 4) return false;
                        m_pos += 4;
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default:   out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool ParseNumber(JsonValue& out) {
        const size_t start = m_pos;
        if (m_src[m_pos] == '-') ++m_pos;
        while (m_pos < m_src.size() && ((m_src[m_pos] >= '0' && m_src[m_pos] <= '9') ||
               m_src[m_pos] == '.' || m_src[m_pos] == 'e' || m_src[m_pos] == 'E' ||
               m_src[m_pos] == '+' || m_src[m_pos] == '-')) {
            ++m_pos;
        }
        double val = 0.0;
        const auto [ptr, ec] = std::from_chars(m_src.data() + start, m_src.data() + m_pos, val);
        if (ec != std::errc{} || ptr != m_src.data() + m_pos || !std::isfinite(val)) return false;
        out.data = val;
        return true;
    }

    std::string_view m_src;
    size_t           m_pos = 0;
};

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
