#include "core/StringUtil.hpp"

#include <windows.h>

#include <limits>

namespace cadence {

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int srcLen = static_cast<int>(utf8.size());
    const int len    = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), srcLen, nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), srcLen, out.data(), len) != len) {
        return {};
    }
    return out;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty() || wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int srcLen = static_cast<int>(wide.size());
    const int len    = WideCharToMultiByte(CP_UTF8, 0, wide.data(), srcLen, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), srcLen, out.data(), len, nullptr, nullptr) != len) {
        return {};
    }
    return out;
}

} // namespace cadence
