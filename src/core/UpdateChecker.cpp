#include "core/UpdateChecker.hpp"
#include "core/JsonParser.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <vector>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace cadence {

namespace {

constexpr uint64_t kMinExeBytes = 64 * 1024;
constexpr uint64_t kMaxExeBytes = 32 * 1024 * 1024;

[[nodiscard]] constexpr bool NtOk(NTSTATUS status) noexcept {
    return status >= 0;
}

void EnforceModernTls(HINTERNET session) noexcept {
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        // Older Windows 10 builds reject TLS 1.3 flag -> fall back cleanly to TLS 1.2 only
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
}

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

class WinFileHandle {
public:
    WinFileHandle() = default;
    explicit WinFileHandle(HANDLE h) noexcept : m_h(h) {}
    ~WinFileHandle() { Reset(); }

    WinFileHandle(const WinFileHandle&)            = delete;
    WinFileHandle& operator=(const WinFileHandle&) = delete;

    void Reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
        if (m_h != INVALID_HANDLE_VALUE && m_h != nullptr) {
            CloseHandle(m_h);
        }
        m_h = h;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return m_h; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_h != INVALID_HANDLE_VALUE && m_h != nullptr; }

private:
    HANDLE m_h = INVALID_HANDLE_VALUE;
};

class BcryptAlgHandle {
public:
    BcryptAlgHandle() = default;
    ~BcryptAlgHandle() {
        if (m_h) BCryptCloseAlgorithmProvider(m_h, 0);
    }
    BcryptAlgHandle(const BcryptAlgHandle&)            = delete;
    BcryptAlgHandle& operator=(const BcryptAlgHandle&) = delete;

    BCRYPT_ALG_HANDLE* Put() noexcept { return &m_h; }
    [[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept { return m_h; }

private:
    BCRYPT_ALG_HANDLE m_h = nullptr;
};

class BcryptHashHandle {
public:
    BcryptHashHandle() = default;
    ~BcryptHashHandle() {
        if (m_h) BCryptDestroyHash(m_h);
    }
    BcryptHashHandle(const BcryptHashHandle&)            = delete;
    BcryptHashHandle& operator=(const BcryptHashHandle&) = delete;

    BCRYPT_HASH_HANDLE* Put() noexcept { return &m_h; }
    [[nodiscard]] BCRYPT_HASH_HANDLE Get() const noexcept { return m_h; }

private:
    BCRYPT_HASH_HANDLE m_h = nullptr;
};

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

[[nodiscard]] bool ConstantTimeEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

[[nodiscard]] bool IsRegularSingleLinkFile(HANDLE hFile) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(hFile, &info)) {
        return false;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return false;
    }
    return info.nNumberOfLinks == 1;
}

[[nodiscard]] bool ReadExactFromHandle(HANDLE hFile, void* dst, DWORD byteCount) noexcept {
    auto* out = static_cast<unsigned char*>(dst);
    DWORD total = 0;
    while (total < byteCount) {
        DWORD readNow = 0;
        if (!ReadFile(hFile, out + total, byteCount - total, &readNow, nullptr) || readNow == 0) {
            return false;
        }
        total += readNow;
    }
    return true;
}

[[nodiscard]] bool WriteAllToHandle(HANDLE hFile, const void* src, DWORD byteCount) noexcept {
    const auto* in = static_cast<const unsigned char*>(src);
    DWORD total = 0;
    while (total < byteCount) {
        DWORD written = 0;
        if (!WriteFile(hFile, in + total, byteCount - total, &written, nullptr) || written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> ComputeSha256FromHandle(HANDLE hFile) noexcept {
    try {
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN)) {
            return std::nullopt;
        }

        BcryptAlgHandle alg;
        if (!NtOk(BCryptOpenAlgorithmProvider(alg.Put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return std::nullopt;
        }

        DWORD objLen = 0;
        DWORD cbData = 0;
        if (!NtOk(BCryptGetProperty(alg.Get(), BCRYPT_OBJECT_LENGTH,
                                    reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cbData, 0)) ||
            objLen == 0) {
            return std::nullopt;
        }

        DWORD hashLen = 0;
        if (!NtOk(BCryptGetProperty(alg.Get(), BCRYPT_HASH_LENGTH,
                                    reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cbData, 0)) ||
            hashLen != 32) {
            return std::nullopt;
        }

        std::vector<unsigned char> hashObj(objLen);
        BcryptHashHandle hash;
        if (!NtOk(BCryptCreateHash(alg.Get(), hash.Put(), hashObj.data(), objLen, nullptr, 0, 0))) {
            return std::nullopt;
        }

        std::array<unsigned char, 65536> buf{};
        for (;;) {
            DWORD readNow = 0;
            if (!ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &readNow, nullptr)) {
                return std::nullopt;
            }
            if (readNow == 0) {
                break;
            }
            if (!NtOk(BCryptHashData(hash.Get(), buf.data(), readNow, 0))) {
                return std::nullopt;
            }
        }

        std::array<unsigned char, 32> digest{};
        if (!NtOk(BCryptFinishHash(hash.Get(), digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            return std::nullopt;
        }

        static constexpr char kHex[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(64);
        for (unsigned char b : digest) {
            hex.push_back(kHex[b >> 4]);
            hex.push_back(kHex[b & 0x0F]);
        }
        return hex;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool VerifyPe64FromHandle(HANDLE hFile) noexcept {
    LARGE_INTEGER fileSizeLi{};
    if (!GetFileSizeEx(hFile, &fileSizeLi) || fileSizeLi.QuadPart <= 0) {
        return false;
    }
    const uint64_t fileSize = static_cast<uint64_t>(fileSizeLi.QuadPart);
    if (fileSize < kMinExeBytes || fileSize > kMaxExeBytes) {
        return false;
    }

    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN)) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!ReadExactFromHandle(hFile, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    if (dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }
    const uint64_t peOffset = static_cast<uint64_t>(dos.e_lfanew);
    if (peOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > fileSize) {
        return false;
    }

    LARGE_INTEGER seekPos{};
    seekPos.QuadPart = static_cast<LONGLONG>(peOffset);
    if (!SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN)) {
        return false;
    }

    DWORD peSig = 0;
    IMAGE_FILE_HEADER coff{};
    if (!ReadExactFromHandle(hFile, &peSig, sizeof(peSig)) ||
        !ReadExactFromHandle(hFile, &coff, sizeof(coff))) {
        return false;
    }
    if (peSig != IMAGE_NT_SIGNATURE) {
        return false;
    }
    if (coff.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return false;
    }
    if ((coff.Characteristics & IMAGE_FILE_DLL) != 0 ||
        (coff.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0) {
        return false;
    }
    return true;
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

    EnforceModernTls(session.Get());
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
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &avail)) {
            return false; // Network error (distinct from clean EOF)
        }
        if (avail == 0) {
            break; // Clean EOF
        }
        while (avail > 0) {
            const DWORD toRead = static_cast<DWORD>(std::min<size_t>(avail, chunk.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.Get(), chunk.data(), toRead, &read)) {
                return false;
            }
            if (read == 0) {
                break;
            }
            if (outBody.size() + read > maxBytes) {
                return false;
            }
            outBody.append(chunk.data(), read);
            avail -= read;
        }
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

std::optional<std::string> UpdateChecker::NormalizeSha256Hex(std::string_view hex) noexcept {
    if (hex.size() != 64) return std::nullopt;
    try {
        std::string norm;
        norm.reserve(64);
        for (char c : hex) {
            if (c >= '0' && c <= '9') {
                norm.push_back(c);
            } else if (c >= 'a' && c <= 'f') {
                norm.push_back(c);
            } else if (c >= 'A' && c <= 'F') {
                norm.push_back(static_cast<char>(c - 'A' + 'a'));
            } else {
                return std::nullopt;
            }
        }
        return norm;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> UpdateChecker::ComputeFileSha256(const std::filesystem::path& filePath) noexcept {
    WinFileHandle fh(CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!fh || !IsRegularSingleLinkFile(fh.Get())) {
        return std::nullopt;
    }
    return ComputeSha256FromHandle(fh.Get());
}

bool UpdateChecker::VerifyFileSha256(const std::filesystem::path& filePath, std::string_view expectedHex) noexcept {
    const auto norm = NormalizeSha256Hex(expectedHex);
    if (!norm) return false;
    const auto actual = ComputeFileSha256(filePath);
    if (!actual) return false;
    return ConstantTimeEquals(*actual, *norm);
}

std::filesystem::path UpdateChecker::GenerateRandomTempPath(const std::filesystem::path& targetDir) {
    if (targetDir.empty()) return {};
    static constexpr wchar_t kHexW[] = L"0123456789abcdef";

    for (int attempt = 0; attempt < 8; ++attempt) {
        std::array<unsigned char, 8> rnd{};
        if (!NtOk(BCryptGenRandom(nullptr, rnd.data(), static_cast<ULONG>(rnd.size()),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            return {};
        }
        std::wstring suffix;
        suffix.reserve(16);
        for (unsigned char b : rnd) {
            suffix.push_back(kHexW[b >> 4]);
            suffix.push_back(kHexW[b & 0x0F]);
        }
        std::filesystem::path cand = targetDir / (L"Cadence.upd." + suffix + L".tmp");
        // Reject any pre-existing file, directory, symlink, or reparse point at candidate path
        if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND) {
            return cand;
        }
    }
    return {};
}

std::optional<ReleaseInfo> UpdateChecker::ParseGitHubReleaseJson(std::string_view json) {
    JsonValue root;
    JsonParser parser(json);
    if (!parser.Parse(root) || !root.IsObject()) {
        return std::nullopt;
    }

    const JsonValue* tagVal = root.Find("tag_name");
    if (!tagVal || !tagVal->IsString()) return std::nullopt;

    const auto ver = ParseSemVer(tagVal->AsString());
    if (!ver) return std::nullopt;

    ReleaseInfo info{};
    info.tagName = tagVal->AsString();
    info.version = *ver;

    const JsonValue* htmlVal = root.Find("html_url");
    if (htmlVal && htmlVal->IsString() &&
        htmlVal->AsString().starts_with("https://github.com/solidvhdx/cadence/releases/")) {
        info.releaseUrl = htmlVal->AsString();
    } else {
        info.releaseUrl = "https://github.com/solidvhdx/cadence/releases/latest";
    }

    // Structurally locate the exact "Cadence.exe" asset and require its SHA-256 digest
    const JsonValue* assetsVal = root.Find("assets");
    if (assetsVal && assetsVal->IsArray()) {
        for (const auto& item : assetsVal->AsArray()) {
            if (!item.IsObject()) continue;
            const JsonValue* nameVal = item.Find("name");
            if (!nameVal || !nameVal->IsString() || nameVal->AsString() != "Cadence.exe") {
                continue;
            }

            const JsonValue* dlVal     = item.Find("browser_download_url");
            const JsonValue* digestVal = item.Find("digest");
            if (!dlVal || !dlVal->IsString() || !digestVal || !digestVal->IsString()) {
                break; // Fail closed: never auto-download an asset without a SHA-256 digest
            }

            const std::string& dlUrl = dlVal->AsString();
            const std::string& dig   = digestVal->AsString();
            if (!dlUrl.starts_with("https://github.com/solidvhdx/cadence/releases/download/") ||
                !dlUrl.ends_with("/Cadence.exe") ||
                !dig.starts_with("sha256:")) {
                break;
            }

            const auto normSha = NormalizeSha256Hex(std::string_view(dig).substr(7));
            if (!normSha) {
                break;
            }

            info.downloadUrl = dlUrl;
            info.sha256Hex   = *normSha;

            const JsonValue* sizeVal = item.Find("size");
            if (sizeVal && sizeVal->IsNumber()) {
                const double sz = sizeVal->AsDouble();
                if (std::isfinite(sz) && sz >= static_cast<double>(kMinExeBytes) &&
                    sz <= static_cast<double>(kMaxExeBytes)) {
                    info.assetSize = static_cast<uint64_t>(sz);
                }
            }
            break;
        }
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

bool UpdateChecker::DownloadBinary(const std::string& url,
                                   const std::filesystem::path& destPath,
                                   std::string_view expectedSha256Hex,
                                   const DownloadProgressCallback& onProgress,
                                   const std::atomic<bool>* cancelFlag) {
    auto isCancelled = [&]() noexcept -> bool {
        return cancelFlag && cancelFlag->load(std::memory_order_relaxed);
    };

    const auto expectedSha = NormalizeSha256Hex(expectedSha256Hex);
    if (!expectedSha || isCancelled() || destPath.empty() ||
        !url.starts_with("https://github.com/solidvhdx/cadence/releases/download/") ||
        !url.ends_with("/Cadence.exe")) {
        return false;
    }

    // Refuse pre-existing files or pre-planted symlinks / reparse points
    if (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
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
    if (!session || isCancelled()) return false;

    EnforceModernTls(session.Get());
    WinHttpSetTimeouts(session.Get(), 5000, 6000, 10000, 15000);

    WinHttpHandle connect(WinHttpConnect(session.Get(), host.c_str(), uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect || isCancelled()) return false;

    WinHttpHandle request(WinHttpOpenRequest(connect.Get(), L"GET", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request || isCancelled()) return false;

    // Allow HTTPS -> HTTPS redirects (GitHub redirects release downloads to objects.githubusercontent.com)
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }
    if (isCancelled()) return false;
    if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
        return false;
    }
    if (isCancelled()) return false;

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request.Get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        statusCode != 200) {
        return false;
    }

    // Query optional Content-Length header so truncated downloads are always rejected
    std::optional<uint64_t> expectedContentLength;
    {
        wchar_t lenBuf[32]{};
        DWORD lenBytes = sizeof(lenBuf);
        if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenBytes, WINHTTP_NO_HEADER_INDEX)) {
            uint64_t parsedLen = 0;
            bool okLen = true;
            for (const wchar_t* p = lenBuf; *p != L'\0'; ++p) {
                if (*p < L'0' || *p > L'9') { okLen = false; break; }
                parsedLen = parsedLen * 10 + static_cast<uint64_t>(*p - L'0');
                if (parsedLen > kMaxExeBytes) { okLen = false; break; }
            }
            if (okLen && parsedLen > 0) {
                expectedContentLength = parsedLen;
            }
        }
    }

    const uint64_t totalExpected = expectedContentLength.value_or(0);
    if (onProgress) {
        onProgress(0, totalExpected);
    }

    // Create destination exclusively (CREATE_NEW, share mode 0, never follow reparse points)
    WinFileHandle outFile(CreateFileW(destPath.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr));
    if (!outFile) {
        return false;
    }

    auto cleanupFail = [&]() -> bool {
        outFile.Reset();
        std::error_code ec;
        std::filesystem::remove(destPath, ec);
        return false;
    };

    if (!IsRegularSingleLinkFile(outFile.Get())) {
        return cleanupFail();
    }

    std::array<unsigned char, 16384> buf{};
    uint64_t totalBytes = 0;

    for (;;) {
        if (isCancelled()) {
            return cleanupFail();
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &avail)) {
            return cleanupFail(); // Mid-stream network error: never accept partial binary!
        }
        if (avail == 0) {
            break; // Clean EOF
        }
        while (avail > 0) {
            if (isCancelled()) {
                return cleanupFail();
            }
            const DWORD toRead = static_cast<DWORD>(std::min<size_t>(avail, buf.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.Get(), buf.data(), toRead, &read)) {
                return cleanupFail();
            }
            if (read == 0) {
                break;
            }
            totalBytes += read;
            if (totalBytes > kMaxExeBytes) {
                return cleanupFail();
            }
            if (!WriteAllToHandle(outFile.Get(), buf.data(), read)) {
                return cleanupFail();
            }
            avail -= read;
            if (onProgress) {
                onProgress(totalBytes, totalExpected);
            }
        }
    }

    if (isCancelled() || !FlushFileBuffers(outFile.Get())) {
        return cleanupFail();
    }
    if (expectedContentLength && totalBytes != *expectedContentLength) {
        return cleanupFail();
    }

    // Verify both cryptographic SHA-256 and x64 PE64 structure on the SAME exclusive handle before closing
    const auto actualSha = ComputeSha256FromHandle(outFile.Get());
    if (!actualSha || !ConstantTimeEquals(*actualSha, *expectedSha)) {
        return cleanupFail();
    }
    if (!VerifyPe64FromHandle(outFile.Get())) {
        return cleanupFail();
    }

    outFile.Reset();
    return true;
}

bool UpdateChecker::VerifyPe64Binary(const std::filesystem::path& exePath) noexcept {
    WinFileHandle fh(CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!fh || !IsRegularSingleLinkFile(fh.Get())) {
        return false;
    }
    return VerifyPe64FromHandle(fh.Get());
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

bool UpdateChecker::ApplyUpdateAndRelaunch(const std::filesystem::path& newExePath,
                                           std::string_view expectedSha256Hex) {
    const auto expectedSha = NormalizeSha256Hex(expectedSha256Hex);
    if (!expectedSha) {
        return false;
    }

    auto moveWithRetry = [](const std::filesystem::path& from, const std::filesystem::path& to) -> bool {
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                return true;
            }
            const DWORD err = GetLastError();
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
                return false;
            }
            Sleep(80);
        }
        return false;
    };

    const DWORD newAttrs = GetFileAttributesW(newExePath.c_str());
    if (newAttrs == INVALID_FILE_ATTRIBUTES ||
        (newAttrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return false;
    }

    std::error_code ec;
    const auto curExe = CurrentExePath();
    if (curExe.empty()) {
        return false;
    }

    std::filesystem::path oldExe = curExe;
    oldExe += L".old";

    // Remove any previous .old binary first
    std::filesystem::remove(oldExe, ec);

    // Windows allows renaming a currently running executable in-place!
    if (!moveWithRetry(curExe, oldExe)) {
        return false;
    }

    if (!moveWithRetry(newExePath, curExe)) {
        // Roll back immediately so the user never loses their working binary
        moveWithRetry(oldExe, curExe);
        return false;
    }

    // Zero-TOCTOU lock: open the final binary with GENERIC_READ + FILE_SHARE_READ only (blocks all
    // concurrent writes, renames, and deletes), re-verify SHA-256 + PE64 from this handle, and hold
    // the handle open across CreateProcessW so the image cannot be swapped between check and launch.
    WinFileHandle lockedExe(CreateFileW(curExe.c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr));
    bool verified = false;
    if (lockedExe && IsRegularSingleLinkFile(lockedExe.Get()) && VerifyPe64FromHandle(lockedExe.Get())) {
        const auto finalSha = ComputeSha256FromHandle(lockedExe.Get());
        verified = finalSha && ConstantTimeEquals(*finalSha, *expectedSha);
    }

    if (!verified) {
        lockedExe.Reset();
        std::filesystem::remove(curExe, ec);
        moveWithRetry(oldExe, curExe);
        return false;
    }

    std::wstring cmdLine = L"\"" + curExe.wstring() + L"\" --updated";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(curExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        lockedExe.Reset();
        // Roll back if the new binary failed to launch
        moveWithRetry(curExe, newExePath);
        moveWithRetry(oldExe, curExe);
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    lockedExe.Reset();
    return true;
}

} // namespace cadence
