#include "core/JsonParser.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace cadence {

namespace {

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

bool JsonParser::Parse(JsonValue& out) {
    SkipWs();
    if (!ParseValue(out, 0)) return false;
    SkipWs();
    return m_pos == m_src.size();
}

void JsonParser::SkipWs() noexcept {
    while (m_pos < m_src.size() &&
           (m_src[m_pos] == ' ' || m_src[m_pos] == '\t' || m_src[m_pos] == '\r' || m_src[m_pos] == '\n')) {
        ++m_pos;
    }
}

bool JsonParser::ParseValue(JsonValue& out, int depth) {
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

bool JsonParser::ParseObject(JsonValue& out, int depth) {
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
        obj.insert_or_assign(std::move(key), std::move(val));
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

bool JsonParser::ParseArray(JsonValue& out, int depth) {
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

bool JsonParser::ParseString(std::string& out) {
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
                    // Decode UTF-16 surrogate pairs (\uD800..\uDBFF followed by \uDC00..\uDFFF)
                    if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_src.size() &&
                        m_src[m_pos] == '\\' && m_src[m_pos + 1] == 'u') {
                        uint32_t low = 0;
                        const auto [lptr, lec] = std::from_chars(m_src.data() + m_pos + 2, m_src.data() + m_pos + 6, low, 16);
                        if (lec == std::errc{} && lptr == m_src.data() + m_pos + 6 && low >= 0xDC00 && low <= 0xDFFF) {
                            m_pos += 6;
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: out.push_back(esc); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool JsonParser::ParseNumber(JsonValue& out) {
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

} // namespace cadence
