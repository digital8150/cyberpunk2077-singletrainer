#include "config.h"
#include "diagnostics.h"
#include "features/features.h"

#include <filesystem>
#include <cstdio>
#include <utility>

namespace Features
{
    Settings& GetSettings() { static Settings settings; return settings; }
}
namespace Diagnostics
{
    void Log(const char*, ...) {}
    RuntimeToggles GetRuntimeToggles() { return {}; }
    void ApplyRuntimeToggles(const RuntimeToggles&) {}
}

int main()
{
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) return 1;
    const auto root = std::filesystem::path(temporary) /
        (L"cbpk-config-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root / L"cbpk");
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    const auto ini = root / L"cbpk" / L"config.ini";
    const auto write = [&](const wchar_t* section, const wchar_t* key, const wchar_t* value) {
        return WritePrivateProfileStringW(section, key, value, ini.c_str()) != FALSE;
    };
    int failures = 0;
    const auto check = [&](bool success, const char* message) {
        if (!success) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    write(L"aimbot", L"activation_key", L"5");
    write(L"aimbot", L"silent_aim", L"1");
    write(L"aimbot", L"enabled", L"1");
    check(Config::Initialize(), "initialize legacy INI");
    auto& s = Features::GetSettings();
    check(s.aimbot.activationKey == 5 && s.aimbot.silentAim, "preserve legacy active profile");
    check(s.alternateAimbot == s.aimbot, "seed second profile from legacy settings");
    s.aimbot.subActivationKey = 6;
    s.aimbot.boneMask = 1 | 4 | 8;
    s.aimbot.leadPrediction = false;
    s.alternateAimbot.activationKey = 2;
    s.alternateAimbot.subActivationKey = 0;
    s.alternateAimbot.nearestBone = true;
    s.alternateAimbot.silentAim = false;
    s.alternateAimbot.smoothing = 17.5f;
    s.esp.showName = false;
    s.esp.showDistance = true;
    s.profileSwitchKey = 0x76;
    const auto first = s.aimbot;
    const auto second = s.alternateAimbot;
    std::swap(s.aimbot, s.alternateAimbot);
    s.activeAimbotProfile = 1;
    Config::Shutdown();
    s = {};
    check(Config::Initialize(), "reload saved profiles");
    check(s.aimbot == second && s.alternateAimbot == first, "round-trip both complete profiles after switching");
    check(s.activeAimbotProfile == 1 && s.profileSwitchKey == 0x76, "round-trip active profile and switch key");
    check(!s.esp.showName && s.esp.showDistance, "independent label toggles");
    // Exercise the production dirty/debounce path, including leadPrediction (previously omitted).
    s.aimbot.leadPrediction = !s.aimbot.leadPrediction;
    s.alternateAimbot.boneMask = 16;
    Config::Update();
    Sleep(550);
    Config::Update();
    check(GetPrivateProfileIntW(L"aimbot", L"lead_prediction", 99, ini.c_str()) == 0, "autosave lead prediction");
    check(GetPrivateProfileIntW(L"aimbot_alternate", L"bone_mask", 0, ini.c_str()) == 16, "autosave alternate profile");
    Config::Shutdown();
    write(L"aimbot", L"bone_mask", L"32");
    write(L"aimbot", L"sub_activation_key", L"999");
    write(L"aimbot_profiles", L"active", L"99");
    s = {};
    check(Config::Initialize(), "load invalid settings");
    check(s.aimbot.boneMask == 1 && s.aimbot.subActivationKey == 0 && s.activeAimbotProfile <= 1,
          "invalid mask/key/profile cannot escape bounds");
    Config::Shutdown();
    std::filesystem::remove(ini);
    std::filesystem::remove(root / L"cbpk");
    std::filesystem::remove(root);
    std::puts(failures ? "Config profile tests failed" : "Config profile tests passed");
    return failures ? 1 : 0;
}
