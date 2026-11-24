#include "core/ProfileManager.hpp"
#include "core/ConfigManager.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>

namespace cadence {

ProfileManager::ProfileManager(AppConfig& config) : m_config(config) {
    EnsureDefaultProfiles();
}

void ProfileManager::EnsureDefaultProfiles() {
    if (!m_config.profiles.empty()) {
        if (!FindById(m_config.activeProfileId)) {
            m_config.activeProfileId = m_config.profiles.front().id;
        }
        return;
    }

    Profile def{};
    def.id   = "default";
    def.name = "Default";
    def.mode = OperatingMode::CPS;
    def.cps  = m_config.cps;
    def.kps  = m_config.kps;

    Profile fastCps = def;
    fastCps.id              = "butterfly-18";
    fastCps.name            = "Butterfly 18";
    fastCps.mode            = OperatingMode::CPS;
    fastCps.cps.rateCps     = 18.0;
    fastCps.cps.jitterMs    = 4.0;
    fastCps.cps.hotkey      = {HotkeySource::Mouse, VK_XBUTTON2};

    Profile mmoKps = def;
    mmoKps.id                 = "skill-spam-e";
    mmoKps.name               = "Skill Spam E";
    mmoKps.mode               = OperatingMode::KPS;
    mmoKps.kps.rateKps        = 10.0;
    mmoKps.kps.targetVk       = 'E';
    mmoKps.kps.activation     = ActivationMode::Hold;
    mmoKps.kps.hotkey         = {HotkeySource::Keyboard, VK_F7};

    m_config.profiles.push_back(std::move(def));
    m_config.profiles.push_back(std::move(fastCps));
    m_config.profiles.push_back(std::move(mmoKps));
    m_config.activeProfileId = "default";
}

const Profile* ProfileManager::FindById(const std::string& id) const noexcept {
    for (const auto& p : m_config.profiles) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::string ProfileManager::ActiveProfileName() const {
    if (const Profile* p = FindById(m_config.activeProfileId)) {
        return p->name;
    }
    return "Default";
}

bool ProfileManager::ActivateProfile(const std::string& id) {
    const Profile* p = FindById(id);
    if (!p) return false;
    m_config.activeProfileId = p->id;
    m_config.activeMode      = p->mode;
    if (p->mode == OperatingMode::CPS) {
        m_config.cps = p->cps;
    } else {
        m_config.kps = p->kps;
    }
    return true;
}

std::string ProfileManager::CreateFromCurrent(const std::string& name) {
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    Profile p{};
    p.id   = std::format("prof-{:x}", static_cast<uint64_t>(ts));
    p.name = name.empty() ? "Custom Preset" : name;
    p.mode = m_config.activeMode;
    p.cps  = m_config.cps;
    p.kps  = m_config.kps;

    m_config.profiles.push_back(std::move(p));
    m_config.activeProfileId = m_config.profiles.back().id;
    return m_config.activeProfileId;
}

bool ProfileManager::UpdateActiveProfile() {
    for (auto& p : m_config.profiles) {
        if (p.id == m_config.activeProfileId) {
            p.mode = m_config.activeMode;
            p.cps  = m_config.cps;
            p.kps  = m_config.kps;
            return true;
        }
    }
    return false;
}

bool ProfileManager::DeleteProfile(std::string id) {
    if (m_config.profiles.size() <= 1) {
        return false; // Always keep at least one profile
    }
    auto it = std::find_if(m_config.profiles.begin(), m_config.profiles.end(),
                           [&](const Profile& p) { return p.id == id; });
    if (it == m_config.profiles.end()) return false;
    m_config.profiles.erase(it);
    if (m_config.activeProfileId == id) {
        m_config.activeProfileId = m_config.profiles.front().id;
    }
    return true;
}

bool ProfileManager::ExportProfile(const std::string& id, const std::filesystem::path& destPath) const {
    const Profile* p = FindById(id);
    if (!p) return false;
    std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string data = ConfigManager::SerializeProfile(*p);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

bool ProfileManager::ReadProfileFile(const std::filesystem::path& srcPath, Profile& outProfile) {
    constexpr uintmax_t kMaxProfileBytes = 64 * 1024; // A single profile is well under 2 KB
    std::error_code ec;
    const auto sz = std::filesystem::file_size(srcPath, ec);
    if (ec || sz == 0 || sz > kMaxProfileBytes) return false;

    std::ifstream in(srcPath, std::ios::binary);
    if (!in) return false;
    std::string buf(static_cast<size_t>(sz), '\0');
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!in && !in.eof()) return false;
    buf.resize(static_cast<size_t>(in.gcount()));

    Profile parsed{};
    if (!ConfigManager::DeserializeProfile(buf, parsed)) {
        return false;
    }
    outProfile = std::move(parsed);
    return true;
}

std::string ProfileManager::AddImportedProfile(Profile imported) {
    // Avoid ID collision (and never keep an empty ID)
    if (imported.id.empty() || FindById(imported.id)) {
        const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
        imported.id = std::format("imp-{:x}", static_cast<uint64_t>(ts));
    }
    m_config.profiles.push_back(std::move(imported));
    return m_config.profiles.back().id;
}

bool ProfileManager::ImportProfile(const std::filesystem::path& srcPath) {
    Profile imported{};
    if (!ReadProfileFile(srcPath, imported)) {
        return false;
    }
    ActivateProfile(AddImportedProfile(std::move(imported)));
    return true;
}

} // namespace cadence
