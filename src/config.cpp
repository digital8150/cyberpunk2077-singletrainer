#include "config.h"
#include "diagnostics.h"
#include "features/features.h"
#include "framework.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

namespace
{
    constexpr ULONGLONG kSaveDebounceMilliseconds = 500;

    wchar_t g_configPath[MAX_PATH]{};
    Features::Settings g_lastObserved;
    Features::Settings g_lastSaved;
    ULONGLONG g_dirtySince = 0;
    bool g_initialized = false;

    bool SameSettings(const Features::Settings& lhs, const Features::Settings& rhs)
    {
        return lhs.ui.language == rhs.ui.language &&
               lhs.ui.theme == rhs.ui.theme &&
               lhs.esp.enabled == rhs.esp.enabled &&
               lhs.esp.boundingBoxes == rhs.esp.boundingBoxes &&
               lhs.esp.skeleton == rhs.esp.skeleton &&
               lhs.esp.healthBars == rhs.esp.healthBars &&
               lhs.esp.nativeHighlight == rhs.esp.nativeHighlight &&
               lhs.esp.hideDead == rhs.esp.hideDead &&
               lhs.esp.visibilityCheck == rhs.esp.visibilityCheck &&
               lhs.esp.hideOccluded == rhs.esp.hideOccluded &&
               lhs.esp.showCivilians == rhs.esp.showCivilians &&
               lhs.esp.showEnemies == rhs.esp.showEnemies &&
               lhs.esp.showPolice == rhs.esp.showPolice &&
               lhs.esp.showUnclassified == rhs.esp.showUnclassified &&
               lhs.esp.maxDistanceMeters == rhs.esp.maxDistanceMeters &&
               lhs.aimbot == rhs.aimbot &&
               lhs.alternateAimbot == rhs.alternateAimbot &&
               lhs.activeAimbotProfile == rhs.activeAimbotProfile &&
               lhs.profileSwitchKey == rhs.profileSwitchKey &&
               lhs.esp.showName == rhs.esp.showName &&
               lhs.esp.showDistance == rhs.esp.showDistance &&
               lhs.misc.noRecoil == rhs.misc.noRecoil &&
               lhs.misc.noSpread == rhs.misc.noSpread &&
               lhs.debug.showFps == rhs.debug.showFps &&
               lhs.debug.showGraph == rhs.debug.showGraph &&
               lhs.debug.graphOpacityPercent == rhs.debug.graphOpacityPercent &&
               lhs.debug.graphPositionX == rhs.debug.graphPositionX &&
               lhs.debug.graphPositionY == rhs.debug.graphPositionY &&
               lhs.debug.showInternalStats == rhs.debug.showInternalStats &&
               lhs.debug.headlessAimbot == rhs.debug.headlessAimbot &&
               lhs.debug.diagnosticLogging == rhs.debug.diagnosticLogging &&
               lhs.debug.crashReporting == rhs.debug.crashReporting &&
               lhs.debug.performanceProfiling == rhs.debug.performanceProfiling &&
               lhs.debug.debuggerOutput == rhs.debug.debuggerOutput;
    }

    bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback)
    {
        return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, g_configPath) != 0;
    }

    // v1은 show_fps와 no_recoil을 [trainer]에 두었다. M2에서 둘이 [debug]/[misc]로 옮겨갔으므로, 새 키가
    // 아직 없는 파일에서는 옛 자리를 한 번 읽어 이어받는다. 그러지 않으면 기존 사용자의 no_recoil=1이
    // 조용히 기본값(꺼짐)으로 되돌아간다. 새 키가 한 번 쓰이고 나면 이 경로는 다시 타지 않는다.
    bool ReadBoolMigrated(const wchar_t* section, const wchar_t* key, const wchar_t* legacySection,
                          const wchar_t* legacyKey, bool fallback)
    {
        constexpr UINT kMissing = 0xFFFF;
        const UINT stored = GetPrivateProfileIntW(section, key, kMissing, g_configPath);
        if (stored != kMissing)
            return stored != 0;
        return GetPrivateProfileIntW(legacySection, legacyKey, fallback ? 1 : 0, g_configPath) != 0;
    }

    unsigned int ReadKey(const wchar_t* section, const wchar_t* key, unsigned int fallback)
    {
        const UINT value = GetPrivateProfileIntW(section, key, static_cast<INT>(fallback), g_configPath);
        // 0x00과 0xFF는 가상 키 코드가 아니다. 범위를 벗어나면 조용히 기본값으로 돌린다.
        return value >= 0x01 && value <= 0xFE ? static_cast<unsigned int>(value) : fallback;
    }

    float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback, float minimum, float maximum)
    {
        wchar_t fallbackText[32]{};
        swprintf_s(fallbackText, L"%.4f", fallback);
        wchar_t text[64]{};
        GetPrivateProfileStringW(section, key, fallbackText, text, static_cast<DWORD>(std::size(text)),
                                 g_configPath);

        wchar_t* end = nullptr;
        const float value = wcstof(text, &end);
        if (end == text || !std::isfinite(value))
            return fallback;
        return std::clamp(value, minimum, maximum);
    }

    bool WriteValue(const wchar_t* section, const wchar_t* key, const wchar_t* value)
    {
        return WritePrivateProfileStringW(section, key, value, g_configPath) != FALSE;
    }

    bool WriteBool(const wchar_t* section, const wchar_t* key, bool value)
    {
        return WriteValue(section, key, value ? L"1" : L"0");
    }

    bool WriteKey(const wchar_t* section, const wchar_t* key, unsigned int value)
    {
        wchar_t text[16]{};
        swprintf_s(text, L"%u", value);
        return WriteValue(section, key, text);
    }

    bool WriteFloat(const wchar_t* section, const wchar_t* key, float value)
    {
        wchar_t text[32]{};
        swprintf_s(text, L"%.4f", value);
        return WriteValue(section, key, text);
    }

    bool DeleteValue(const wchar_t* section, const wchar_t* key)
    {
        return WritePrivateProfileStringW(section, key, nullptr, g_configPath) != FALSE;
    }

    void LoadAimbot(const wchar_t* section, Features::AimbotSettings& aimbot)
    {
        aimbot.enabled = ReadBool(section, L"enabled", aimbot.enabled);
        aimbot.silentAim = ReadBool(section, L"silent_aim", aimbot.silentAim);
        aimbot.activationKey = ReadKey(section, L"activation_key", aimbot.activationKey);
        aimbot.drawFovCircle =
            ReadBool(section, L"draw_fov_circle", aimbot.drawFovCircle);
        aimbot.targetEnemies = ReadBool(section, L"target_enemies", aimbot.targetEnemies);
        aimbot.targetPolice = ReadBool(section, L"target_police", aimbot.targetPolice);
        aimbot.visibleOnly = ReadBool(section, L"visible_only", aimbot.visibleOnly);
        aimbot.requireHealthPool =
            ReadBool(section, L"require_health_pool", aimbot.requireHealthPool);
        aimbot.limitHealthPool =
            ReadBool(section, L"limit_health_pool", aimbot.limitHealthPool);
        aimbot.maxHealthPool =
            ReadFloat(section, L"max_health_pool", aimbot.maxHealthPool, 500.0f, 6000.0f);
        aimbot.fovRadiusDegrees =
            ReadFloat(section, L"fov_radius_degrees", aimbot.fovRadiusDegrees, 1.0f, 60.0f);
        aimbot.smoothing = ReadFloat(section, L"smoothing", aimbot.smoothing, 0.0f, 30.0f);
        aimbot.maxDistanceMeters =
            ReadFloat(section, L"max_distance_meters", aimbot.maxDistanceMeters, 10.0f, 300.0f);

        const UINT subKey = GetPrivateProfileIntW(section, L"sub_activation_key", 0, g_configPath);
        aimbot.subActivationKey = subKey < 0xFF ? subKey : 0;
        aimbot.boneMask = GetPrivateProfileIntW(section, L"bone_mask", 1, g_configPath) & 31u;
        if (!aimbot.boneMask) aimbot.boneMask = 1;
        aimbot.nearestBone = ReadBool(section, L"nearest_bone", aimbot.nearestBone);
        aimbot.leadPrediction = ReadBool(section, L"lead_prediction", aimbot.leadPrediction);
    }

    bool SaveAimbot(const wchar_t* section, const Features::AimbotSettings& aimbot)
    {
        bool ok = true;
        ok &= WriteBool(section, L"enabled", aimbot.enabled);
        ok &= WriteBool(section, L"silent_aim", aimbot.silentAim);
        ok &= WriteKey(section, L"activation_key", aimbot.activationKey);
        ok &= WriteBool(section, L"draw_fov_circle", aimbot.drawFovCircle);
        ok &= WriteBool(section, L"target_enemies", aimbot.targetEnemies);
        ok &= WriteBool(section, L"target_police", aimbot.targetPolice);
        ok &= WriteBool(section, L"visible_only", aimbot.visibleOnly);
        ok &= WriteBool(section, L"require_health_pool", aimbot.requireHealthPool);
        ok &= WriteBool(section, L"limit_health_pool", aimbot.limitHealthPool);
        ok &= WriteFloat(section, L"max_health_pool", aimbot.maxHealthPool);
        ok &= WriteFloat(section, L"fov_radius_degrees", aimbot.fovRadiusDegrees);
        ok &= WriteFloat(section, L"smoothing", aimbot.smoothing);
        ok &= WriteFloat(section, L"max_distance_meters", aimbot.maxDistanceMeters);

        ok &= WriteKey(section, L"sub_activation_key", aimbot.subActivationKey);
        ok &= WriteKey(section, L"bone_mask", aimbot.boneMask);
        ok &= WriteBool(section, L"nearest_bone", aimbot.nearestBone);
        ok &= WriteBool(section, L"lead_prediction", aimbot.leadPrediction);
        return ok;
    }

    bool Save(const Features::Settings& settings)
    {
        bool ok = true;
        ok &= WriteValue(L"trainer", L"version", L"2");

        ok &= WriteKey(L"ui", L"language", static_cast<unsigned int>(settings.ui.language));
        ok &= WriteKey(L"ui", L"theme", static_cast<unsigned int>(settings.ui.theme));

        ok &= WriteBool(L"esp", L"enabled", settings.esp.enabled);
        ok &= WriteBool(L"esp", L"bounding_boxes", settings.esp.boundingBoxes);
        ok &= WriteBool(L"esp", L"skeleton", settings.esp.skeleton);
        ok &= WriteBool(L"esp", L"health_bars", settings.esp.healthBars);
        ok &= WriteBool(L"esp", L"native_highlight", settings.esp.nativeHighlight);
        ok &= WriteBool(L"esp", L"hide_dead", settings.esp.hideDead);
        ok &= WriteBool(L"esp", L"visibility_check", settings.esp.visibilityCheck);
        ok &= WriteBool(L"esp", L"hide_occluded", settings.esp.hideOccluded);
        ok &= WriteBool(L"esp", L"show_civilians", settings.esp.showCivilians);
        ok &= WriteBool(L"esp", L"show_enemies", settings.esp.showEnemies);
        ok &= WriteBool(L"esp", L"show_police", settings.esp.showPolice);
        ok &= WriteBool(L"esp", L"show_unclassified", settings.esp.showUnclassified);
        ok &= WriteFloat(L"esp", L"max_distance_meters", settings.esp.maxDistanceMeters);

        ok &= SaveAimbot(L"aimbot", settings.aimbot);
        ok &= SaveAimbot(L"aimbot_alternate", settings.alternateAimbot);
        ok &= WriteKey(L"aimbot_profiles", L"active", settings.activeAimbotProfile);
        ok &= WriteKey(L"aimbot_profiles", L"switch_key", settings.profileSwitchKey);
        ok &= WriteBool(L"esp", L"show_name", settings.esp.showName);
        ok &= WriteBool(L"esp", L"show_distance", settings.esp.showDistance);

        ok &= WriteBool(L"misc", L"no_recoil", settings.misc.noRecoil);
        ok &= WriteBool(L"misc", L"no_spread", settings.misc.noSpread);
        // M2의 미완성/불안정 기능은 제거됐다. 기존 config에 남은 키도 한 번의 저장으로 정리해
        // 다음 세션에서 죽은 설정이 다시 살아 있는 것처럼 보이지 않게 한다.
        ok &= DeleteValue(L"misc", L"auto_pistol");
        ok &= DeleteValue(L"misc", L"infinite_health");
        ok &= DeleteValue(L"misc", L"infinite_stamina");

        ok &= WriteBool(L"debug", L"show_fps", settings.debug.showFps);
        ok &= WriteBool(L"debug", L"show_graph", settings.debug.showGraph);
        ok &= WriteFloat(L"debug", L"graph_opacity", settings.debug.graphOpacityPercent);
        ok &= WriteFloat(L"debug", L"graph_position_x", settings.debug.graphPositionX);
        ok &= WriteFloat(L"debug", L"graph_position_y", settings.debug.graphPositionY);
        ok &= WriteBool(L"debug", L"show_internal_stats", settings.debug.showInternalStats);
        ok &= WriteBool(L"debug", L"headless_aimbot", settings.debug.headlessAimbot);

        // 진단 스위치의 정본은 계속 [diagnostics] 섹션이다. Diagnostics::Initialize가 Config보다 먼저
        // 돌면서 환경 변수 > ini > 기본값 순으로 이 키들을 읽으므로, 오버레이에서 바꾼 값도 같은 키에
        // 되돌려 써야 다음 세션에 이어진다. fatal_log와 veh는 UI에서 "크래시 기록" 하나로 합쳐져 있다.
        ok &= WriteBool(L"diagnostics", L"logging", settings.debug.diagnosticLogging);
        ok &= WriteBool(L"diagnostics", L"profiling", settings.debug.performanceProfiling);
        ok &= WriteBool(L"diagnostics", L"debug_output", settings.debug.debuggerOutput);
        ok &= WriteBool(L"diagnostics", L"fatal_log", settings.debug.crashReporting);
        ok &= WriteBool(L"diagnostics", L"veh", settings.debug.crashReporting);

        if (!ok)
            Diagnostics::Log("config save failed: error=%lu", GetLastError());
        else
            Diagnostics::Log("config saved");
        return ok;
    }
}

namespace Config
{
    bool Initialize()
    {
        wchar_t localAppData[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                                     static_cast<DWORD>(std::size(localAppData)));
        if (length == 0 || length >= std::size(localAppData))
        {
            Diagnostics::Log("config unavailable: LOCALAPPDATA is missing or too long");
            return false;
        }

        wchar_t directory[MAX_PATH]{};
        if (swprintf_s(directory, L"%s\\cbpk", localAppData) < 0 ||
            swprintf_s(g_configPath, L"%s\\config.ini", directory) < 0)
        {
            Diagnostics::Log("config unavailable: path exceeds MAX_PATH");
            g_configPath[0] = L'\0';
            return false;
        }

        if (!CreateDirectoryW(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            Diagnostics::Log("config directory creation failed: error=%lu", GetLastError());
            g_configPath[0] = L'\0';
            return false;
        }

        Features::Settings& settings = Features::GetSettings();

        settings.ui.language = static_cast<Features::Language>(
            std::clamp<unsigned int>(GetPrivateProfileIntW(L"ui", L"language",
                                                           static_cast<INT>(settings.ui.language), g_configPath),
                                     0u, 1u));
        settings.ui.theme = static_cast<Features::Theme>(
            std::clamp<unsigned int>(GetPrivateProfileIntW(L"ui", L"theme",
                                                           static_cast<INT>(settings.ui.theme), g_configPath),
                                     0u, 1u));

        settings.esp.enabled = ReadBool(L"esp", L"enabled", settings.esp.enabled);
        settings.esp.boundingBoxes = ReadBool(L"esp", L"bounding_boxes", settings.esp.boundingBoxes);
        settings.esp.skeleton = ReadBool(L"esp", L"skeleton", settings.esp.skeleton);
        settings.esp.healthBars = ReadBool(L"esp", L"health_bars", settings.esp.healthBars);
        settings.esp.nativeHighlight = ReadBool(L"esp", L"native_highlight", settings.esp.nativeHighlight);
        settings.esp.hideDead = ReadBool(L"esp", L"hide_dead", settings.esp.hideDead);
        settings.esp.visibilityCheck = ReadBool(L"esp", L"visibility_check", settings.esp.visibilityCheck);
        settings.esp.hideOccluded = ReadBool(L"esp", L"hide_occluded", settings.esp.hideOccluded);
        settings.esp.showCivilians = ReadBool(L"esp", L"show_civilians", settings.esp.showCivilians);
        settings.esp.showEnemies = ReadBool(L"esp", L"show_enemies", settings.esp.showEnemies);
        settings.esp.showPolice = ReadBool(L"esp", L"show_police", settings.esp.showPolice);
        settings.esp.showUnclassified = ReadBool(L"esp", L"show_unclassified", settings.esp.showUnclassified);
        settings.esp.maxDistanceMeters =
            ReadFloat(L"esp", L"max_distance_meters", settings.esp.maxDistanceMeters, 10.0f, 300.0f);

        LoadAimbot(L"aimbot", settings.aimbot);
        settings.alternateAimbot = settings.aimbot;
        LoadAimbot(L"aimbot_alternate", settings.alternateAimbot);
        settings.activeAimbotProfile = (std::min)(1u, GetPrivateProfileIntW(L"aimbot_profiles", L"active", 0, g_configPath));
        settings.profileSwitchKey = ReadKey(L"aimbot_profiles", L"switch_key", settings.profileSwitchKey);
        settings.esp.showName = ReadBool(L"esp", L"show_name", settings.esp.showName);
        settings.esp.showDistance = ReadBool(L"esp", L"show_distance", settings.esp.showDistance);

        settings.misc.noRecoil =
            ReadBoolMigrated(L"misc", L"no_recoil", L"trainer", L"no_recoil", settings.misc.noRecoil);
        settings.misc.noSpread = ReadBool(L"misc", L"no_spread", settings.misc.noSpread);

        settings.debug.showFps =
            ReadBoolMigrated(L"debug", L"show_fps", L"trainer", L"show_fps", settings.debug.showFps);
        settings.debug.showGraph = ReadBool(L"debug", L"show_graph", settings.debug.showGraph);
        settings.debug.graphOpacityPercent =
            ReadFloat(L"debug", L"graph_opacity", settings.debug.graphOpacityPercent, 35.0f, 100.0f);
        settings.debug.graphPositionX =
            ReadFloat(L"debug", L"graph_position_x", settings.debug.graphPositionX, -1.0f, 16384.0f);
        settings.debug.graphPositionY =
            ReadFloat(L"debug", L"graph_position_y", settings.debug.graphPositionY, -1.0f, 16384.0f);
        settings.debug.showInternalStats =
            ReadBool(L"debug", L"show_internal_stats", settings.debug.showInternalStats);
        settings.debug.headlessAimbot = ReadBoolMigrated(L"debug", L"headless_aimbot", L"aimbot",
                                                         L"headless_diagnostics", settings.debug.headlessAimbot);

        // 진단 토글은 ini에서 직접 읽지 않는다. Diagnostics::Initialize가 이미 환경 변수 > ini >
        // 기본값 순으로 결정했고, 그 결정이 이 세션의 진실이다 (환경 변수로 덮어쓴 세션에서 ini 값을
        // 다시 읽어오면 UI가 실제 상태와 다른 것을 보여주게 된다).
        const Diagnostics::RuntimeToggles toggles = Diagnostics::GetRuntimeToggles();
        settings.debug.diagnosticLogging = toggles.diagnosticLogging;
        settings.debug.crashReporting = toggles.crashReporting;
        settings.debug.performanceProfiling = toggles.performanceProfiling;
        settings.debug.debuggerOutput = toggles.debuggerOutput;

        g_lastObserved = settings;
        g_lastSaved = settings;
        g_dirtySince = 0;
        g_initialized = true;

        const bool existed = GetFileAttributesW(g_configPath) != INVALID_FILE_ATTRIBUTES;
        Diagnostics::Log("config %s: path=%ls", existed ? "loaded" : "initialized", g_configPath);
        if (!existed)
            Save(settings);
        return true;
    }

    void Update()
    {
        if (!g_initialized)
            return;

        const ULONGLONG now = GetTickCount64();
        const Features::Settings& settings = Features::GetSettings();
        if (!SameSettings(settings, g_lastObserved))
        {
            // 진단 스위치는 디바운스를 기다리지 않고 바로 반영한다. 끄는 쪽이 목적인 토글이라
            // 500 ms를 더 기록/계측한 뒤에 꺼지면 사용자가 켜고 끈 효과를 관측할 수 없다.
            if (g_lastObserved.debug.diagnosticLogging != settings.debug.diagnosticLogging ||
                g_lastObserved.debug.crashReporting != settings.debug.crashReporting ||
                g_lastObserved.debug.performanceProfiling != settings.debug.performanceProfiling ||
                g_lastObserved.debug.debuggerOutput != settings.debug.debuggerOutput)
            {
                Diagnostics::RuntimeToggles toggles;
                toggles.diagnosticLogging = settings.debug.diagnosticLogging;
                toggles.crashReporting = settings.debug.crashReporting;
                toggles.performanceProfiling = settings.debug.performanceProfiling;
                toggles.debuggerOutput = settings.debug.debuggerOutput;
                Diagnostics::ApplyRuntimeToggles(toggles);
            }
            g_lastObserved = settings;
            g_dirtySince = now;
        }

        if (g_dirtySince != 0 && now - g_dirtySince >= kSaveDebounceMilliseconds &&
            !SameSettings(settings, g_lastSaved))
        {
            if (Save(settings))
                g_lastSaved = settings;
            g_dirtySince = 0;
        }
    }

    void Shutdown()
    {
        if (!g_initialized)
            return;
        const Features::Settings& settings = Features::GetSettings();
        if (!SameSettings(settings, g_lastSaved))
            Save(settings);
        g_initialized = false;
        g_dirtySince = 0;
    }

    const wchar_t* Path()
    {
        return g_configPath;
    }
}
