#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cadence {

// Minimal self-contained RFC-8259 JSON DOM (zero third-party dependencies).
// Enforces a strict recursion depth limit (64) and rejects non-finite numbers.
struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray  = std::vector<JsonValue>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> data{nullptr};

    [[nodiscard]] bool IsNull()   const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
    [[nodiscard]] bool IsObject() const noexcept { return std::holds_alternative<JsonObject>(data); }
    [[nodiscard]] bool IsArray()  const noexcept { return std::holds_alternative<JsonArray>(data); }
    [[nodiscard]] bool IsString() const noexcept { return std::holds_alternative<std::string>(data); }
    [[nodiscard]] bool IsNumber() const noexcept { return std::holds_alternative<double>(data); }
    [[nodiscard]] bool IsBool()   const noexcept { return std::holds_alternative<bool>(data); }

    [[nodiscard]] const JsonObject&  AsObject() const { return std::get<JsonObject>(data); }
    [[nodiscard]] const JsonArray&   AsArray()  const { return std::get<JsonArray>(data); }
    [[nodiscard]] const std::string& AsString() const { return std::get<std::string>(data); }
    [[nodiscard]] double             AsDouble() const { return std::get<double>(data); }
    [[nodiscard]] bool               AsBool()   const { return std::get<bool>(data); }

    [[nodiscard]] const JsonValue* Find(const std::string& key) const noexcept {
        if (!IsObject()) return nullptr;
        const auto& obj = AsObject();
        const auto it = obj.find(key);
        return (it != obj.end()) ? &it->second : nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) noexcept : m_src(src) {}

    [[nodiscard]] bool Parse(JsonValue& out);

private:
    static constexpr int kMaxDepth = 64;

    void SkipWs() noexcept;
    bool ParseValue(JsonValue& out, int depth);
    bool ParseObject(JsonValue& out, int depth);
    bool ParseArray(JsonValue& out, int depth);
    bool ParseString(std::string& out);
    bool ParseNumber(JsonValue& out);

    std::string_view m_src;
    size_t           m_pos = 0;
};

} // namespace cadence
