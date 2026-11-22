#include "core/UpdateChecker.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <vector>

namespace cadence {

namespace {

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) noexcept : m_h(h) {}
    ~WinHttpHandle() { Reset(); }

    WinHttpHandle(const WinHttpHandle&)            = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    void Reset(HINTERNET h = nullptr) noexcept {
        if (m_h) WinHttpCloseHandle(m_h);
        m_h = h;
    }

    [[nodiscard]] HINTERNET Get() const noexcept { return m_h; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_h != nullptr; }

private:
    HINTERNET m_h = nullptr;
};

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

// Extracts a JSON string value for a given key starting at offset (handles basic JSON escapes like \/)
std::optional<std::string> ExtractJsonString(std::string_view json, std::string_view key, size_t startPos = 0, size_t* outEndPos = nullptr) {
    std::string pattern;
    pattern.reserve(key.size() + 3);
    pattern.push_back('"');
    pattern.append(key);
    pattern.push_back('"');

    size_t pos = startPos;
    while ((pos = json.find(pattern, pos)) != std::string_view::npos) {
        size_t i = pos + pattern.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
        if (i >= json.size() || json[i] != ':') {
            pos = i;
            continue;
        }
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
        if (i >= json.size() || json[i] != '"') {
            pos = i;
            continue;
        }
        ++i;
        std::string value;
        while (i < json.size()) {
            const char c = json[i++];
            if (c == '"') {
                if (outEndPos) *outEndPos = i;
                return value;
            }
            if (c == '\\' && i < json.size()) {
                const char esc = json[i++];
                switch (esc) {
                    case '"':  value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    case '/':  value.push_back('/'); break;
                    case 'n':  value.push_back('\n'); break;
                    case 'r':  value.push_back('\r'); break;
                    case 't':  value.push_back('\t'); break;
                    default:   value.push_back(esc); break;
                }
            } else {
                value.push_back(c);
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool HttpGetToBuffer(const std::wstring& host,
                     const std::wstring& path,
                     const std::wstring& extraHeaders,
                     std::string& outBody,
                     size_t maxBytes) {
    const std::wstring ua = L"Cadence/" + Utf8ToWide(CADENCE_VERSION_STR);
    WinHttpHandle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;

    WinHttpSetTimeouts(session.Get(), 4000, 4000, 5000, 8000);

    WinHttpHandle connect(WinHttpConnect(session.Get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect) return false;

    WinHttpHandle request(WinHttpOpenRequest(connect.Get(), L"GET", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request) return false;

    const wchar_t* hdrs = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str();
    const DWORD hdrLen  = extraHeaders.empty() ? 0u : static_cast<DWORD>(extraHeaders.size());
    if (!WinHttpSendRequest(request.Get(), hdrs, hdrLen, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }
    if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request.Get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    if (statusCode != 200) {
        return false;
    }

    outBody.clear();
    std::array<char, 8192> chunk{};
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(request.Get(), &avail) && avail > 0) {
        DWORD toRead = static_cast<DWORD>(std::min<size_t>(avail, chunk.size()));
        DWORD read   = 0;
        if (!WinHttpReadData(request.Get(), chunk.data(), toRead, &read) || read == 0) {
            break;
        }
        if (outBody.size() + read > maxBytes) {
            return false;
        }
        outBody.append(chunk.data(), read);
    }
    return !outBody.empty();
}

} // namespace

std::optional<SemVer> UpdateChecker::ParseSemVer(std::string_view tag) noexcept {
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) {
        tag.remove_prefix(1);
    }
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) {
        tag.remove_prefix(1);
    }
    if (tag.empty()) return std::nullopt;

    SemVer v{};
    const char* cur = tag.data();
    const char* end = tag.data() + tag.size();

    auto [p1, ec1] = std::from_chars(cur, end, v.major);
    if (ec1 != std::errc{}) return std::nullopt;

    if (p1 < end && *p1 == '.') {
        auto [p2, ec2] = std::from_chars(p1 + 1, end, v.minor);
        if (ec2 != std::errc{}) return std::nullopt;
        if (p2 < end && *p2 == '.') {
            auto [p3, ec3] = std::from_chars(p2 + 1, end, v.patch);
            if (ec3 != std::errc{}) return std::nullopt;
        }
    }
    return v;
}

bool UpdateChecker::IsNewerVersion(std::string_view candidateTag, SemVer current) noexcept {
    const auto parsed = ParseSemVer(candidateTag);
    if (!parsed) return false;
    return *parsed > current;
}

std::optional<ReleaseInfo> UpdateChecker::ParseGitHubReleaseJson(std::string_view json) {
    const auto tag = ExtractJsonString(json, "tag_name");
    if (!tag) return std::nullopt;

    const auto ver = ParseSemVer(*tag);
    if (!ver) return std::nullopt;

    ReleaseInfo info{};
    info.tagName = *tag;
    info.version = *ver;

    if (const auto htmlUrl = ExtractJsonString(json, "html_url")) {
        if (htmlUrl->starts_with("https://github.com/solidvhdx/cadence/")) {
            info.releaseUrl = *htmlUrl;
        }
    }
    if (info.releaseUrl.empty()) {
        info.releaseUrl = "https://github.com/solidvhdx/cadence/releases/latest";
    }

    // Find the Cadence.exe download URL inside the assets array
    size_t searchPos = 0;
    while (true) {
        size_t endPos = 0;
        const auto dlUrl = ExtractJsonString(json, "browser_download_url", searchPos, &endPos);
        if (!dlUrl) break;
        if (dlUrl->starts_with("https://github.com/solidvhdx/cadence/releases/download/") &&
            dlUrl->ends_with("/Cadence.exe")) {
            info.downloadUrl = *dlUrl;
            break;
        }
        searchPos = endPos;
    }

    return info;
}

std::optional<ReleaseInfo> UpdateChecker::CheckForUpdate() {
    std::string body;
    if (!HttpGetToBuffer(L"api.github.com",
                         L"/repos/solidvhdx/cadence/releases/latest",
                         L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                         body,
                         256 * 1024)) {
        return std::nullopt;
    }

    auto info = ParseGitHubReleaseJson(body);
    if (!info || info->version <= kCurrentVersion) {
        return std::nullopt;
    }
    return info;
}

bool UpdateChecker::DownloadBinary(const std::string& url, const std::filesystem::path& destPath) {
    if (!url.starts_with("https://github.com/solidvhdx/cadence/releases/download/")) {
        return false;
    }

    const std::wstring wideUrl = Utf8ToWide(url);
    URL_COMPONENTSW uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.dwHostNameLength  = static_cast<DWORD>(-1);
    uc.dwUrlPathLength   = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &uc)) {
        return false;
    }

    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo) {
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }

    const std::wstring ua = L"Cadence/" + Utf8ToWide(CADENCE_VERSION_STR);
    WinHttpHandle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;

    WinHttpSetTimeouts(session.Get(), 5000, 6000, 10000, 30000);

    WinHttpHandle connect(WinHttpConnect(session.Get(), host.c_str(), uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect) return false;

    WinHttpHandle request(WinHttpOpenRequest(connect.Get(), L"GET", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request) return false;

    // Allow HTTPS -> HTTPS redirects (GitHub redirects release downloads to objects.githubusercontent.com)
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }
    if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request.Get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        statusCode != 200) {
        return false;
    }

    std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    constexpr size_t kMaxExeBytes = 32 * 1024 * 1024;
    std::array<char, 65536> buf{};
    size_t totalBytes = 0;
    bool validMzHeader = false;

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(request.Get(), &avail) && avail > 0) {
        DWORD toRead = static_cast<DWORD>(std::min<size_t>(avail, buf.size()));
        DWORD read   = 0;
        if (!WinHttpReadData(request.Get(), buf.data(), toRead, &read) || read == 0) {
            break;
        }
        if (totalBytes == 0 && read >= 2) {
            validMzHeader = (buf[0] == 'M' && buf[1] == 'Z');
        }
        totalBytes += read;
        if (totalBytes > kMaxExeBytes) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(destPath, ec);
            return false;
        }
        out.write(buf.data(), static_cast<std::streamsize>(read));
        if (!out.good()) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(destPath, ec);
            return false;
        }
    }
    out.flush();
    out.close();

    // Sanity check: real Windows PE binary (> 64 KB with MZ header)
    if (!validMzHeader || totalBytes < 64 * 1024) {
        std::error_code ec;
        std::filesystem::remove(destPath, ec);
        return false;
    }
    return true;
}

std::filesystem::path UpdateChecker::CurrentExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return {};
        if (len < buf.size() - 1) {
            buf.resize(len);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

void UpdateChecker::CleanupOldBinary() noexcept {
    try {
        const auto cur = CurrentExePath();
        if (cur.empty()) return;
        std::filesystem::path oldPath = cur;
        oldPath += L".old";
        std::error_code ec;
        if (std::filesystem::exists(oldPath, ec)) {
            std::filesystem::remove(oldPath, ec);
        }
    } catch (...) {
    }
}

bool UpdateChecker::ApplyUpdateAndRelaunch(const std::filesystem::path& newExePath) {
    std::error_code ec;
    const auto curExe = CurrentExePath();
    if (curExe.empty() || !std::filesystem::exists(newExePath, ec)) {
        return false;
    }

    std::filesystem::path oldExe = curExe;
    oldExe += L".old";

    // Remove any previous .old binary first
    std::filesystem::remove(oldExe, ec);

    // Windows allows renaming a currently running executable in-place!
    if (!MoveFileExW(curExe.c_str(), oldExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return false;
    }

    if (!MoveFileExW(newExePath.c_str(), curExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Roll back immediately so the user never loses their working binary
        MoveFileExW(oldExe.c_str(), curExe.c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }

    std::wstring cmdLine = L"\"" + curExe.wstring() + L"\" --updated";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(curExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace cadence
