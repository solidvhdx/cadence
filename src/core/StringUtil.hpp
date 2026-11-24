#pragma once

#include <string>
#include <string_view>

namespace cadence {

// UTF-8 <-> UTF-16 conversion for Win32 APIs. Invalid input yields an empty string.
[[nodiscard]] std::wstring Utf8ToWide(std::string_view utf8);
[[nodiscard]] std::string  WideToUtf8(std::wstring_view wide);

} // namespace cadence
