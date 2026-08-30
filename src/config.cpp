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
               lhs.aimbot.enabled == rhs.aimbot.enabled &&
               lhs.aimbot.silentAim == rhs.aimbot.silentAim &&
               lhs.aimbot.activationKey == rhs.aimbot.activationKey &&
               lhs.aimbot.drawFovCircle == rhs.aimbot.drawFovCircle &&
               lhs.aimbot.targetEnemies == rhs.aimbot.targetEnemies &&
               lhs.aimbot.targetPolice == rhs.aimbot.targetPolice &&
               lhs.aimbot.visibleOnly == rhs.aimbot.visibleOnly &&
               lhs.aimbot.requireHealthPool == rhs.aimbot.requireHealthPool &&
               lhs.aimbot.limitHealthPool == rhs.aimbot.limitHealthPool &&
               lhs.aimbot.maxHealthPool == rhs.aimbot.maxHealthPool &&
               lhs.aimbot.fovRadiusDegrees == rhs.aimbot.fovRadiusDegrees &&
               lhs.aimbot.smoothing == rhs.aimbot.smoothing &&
               lhs.aimbot.maxDistanceMeters == rhs.aimbot.maxDistanceMeters &&
               lhs.misc.noRecoil == rhs.misc.noRecoil &&
               lhs.misc.noSpread == rhs.misc.noSpread &&
               lhs.misc.autoPistol == rhs.misc.autoPistol &&
               lhs.misc.infiniteHealth == rhs.misc.infiniteHealth &&
               lhs.misc.infiniteStamina == rhs.misc.infiniteStamina &&
               lhs.debug.showFps == rhs.debug.showFps &&
               lhs.debug.showGraph == rhs.debug.showGraph &&
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

        ok &= WriteBool(L"aimbot", L"enabled", settings.aimbot.enabled);
        ok &= WriteBool(L"aimbot", L"silent_aim", settings.aimbot.silentAim);
        ok &= WriteKey(L"aimbot", L"activation_key", settings.aimbot.activationKey);
        ok &= WriteBool(L"aimbot", L"draw_fov_circle", settings.aimbot.drawFovCircle);
        ok &= WriteBool(L"aimbot", L"target_enemies", settings.aimbot.targetEnemies);
        ok &= WriteBool(L"aimbot", L"target_police", settings.aimbot.targetPolice);
        ok &= WriteBool(L"aimbot", L"visible_only", settings.aimbot.visibleOnly);
        ok &= WriteBool(L"aimbot", L"require_health_pool", settings.aimbot.requireHealthPool);
        ok &= WriteBool(L"aimbot", L"limit_health_pool", settings.aimbot.limitHealthPool);
        ok &= WriteFloat(L"aimbot", L"max_health_pool", settings.aimbot.maxHealthPool);
        ok &= WriteFloat(L"aimbot", L"fov_radius_degrees", settings.aimbot.fovRadiusDegrees);
        ok &= WriteFloat(L"aimbot", L"smoothing", settings.aimbot.smoothing);
        ok &= WriteFloat(L"aimbot", L"max_distance_meters", settings.aimbot.maxDistanceMeters);

        ok &= WriteBool(L"misc", L"no_recoil", settings.misc.noRecoil);
        ok &= WriteBool(L"misc", L"no_spread", settings.misc.noSpread);
        ok &= WriteBool(L"misc", L"auto_pistol", settings.misc.autoPistol);
        ok &= WriteBool(L"misc", L"infinite_health", settings.misc.infiniteHealth);
        ok &= WriteBool(L"misc", L"infinite_stamina", settings.misc.infiniteStamina);

        ok &= WriteBool(L"debug", L"show_fps", settings.debug.showFps);
        ok &= WriteBool(L"debug", L"show_graph", settings.debug.showGraph);
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

        settings.aimbot.enabled = ReadBool(L"aimbot", L"enabled", settings.aimbot.enabled);
        settings.aimbot.silentAim = ReadBool(L"aimbot", L"silent_aim", settings.aimbot.silentAim);
        settings.aimbot.activationKey = ReadKey(L"aimbot", L"activation_key", settings.aimbot.activationKey);
        settings.aimbot.drawFovCircle =
            ReadBool(L"aimbot", L"draw_fov_circle", settings.aimbot.drawFovCircle);
        settings.aimbot.targetEnemies = ReadBool(L"aimbot", L"target_enemies", settings.aimbot.targetEnemies);
        settings.aimbot.targetPolice = ReadBool(L"aimbot", L"target_police", settings.aimbot.targetPolice);
        settings.aimbot.visibleOnly = ReadBool(L"aimbot", L"visible_only", settings.aimbot.visibleOnly);
        settings.aimbot.requireHealthPool =
            ReadBool(L"aimbot", L"require_health_pool", settings.aimbot.requireHealthPool);
        settings.aimbot.limitHealthPool =
            ReadBool(L"aimbot", L"limit_health_pool", settings.aimbot.limitHealthPool);
        settings.aimbot.maxHealthPool =
            ReadFloat(L"aimbot", L"max_health_pool", settings.aimbot.maxHealthPool, 500.0f, 6000.0f);
        settings.aimbot.fovRadiusDegrees =
            ReadFloat(L"aimbot", L"fov_radius_degrees", settings.aimbot.fovRadiusDegrees, 1.0f, 60.0f);
        settings.aimbot.smoothing = ReadFloat(L"aimbot", L"smoothing", settings.aimbot.smoothing, 0.0f, 30.0f);
        settings.aimbot.maxDistanceMeters =
            ReadFloat(L"aimbot", L"max_distance_meters", settings.aimbot.maxDistanceMeters, 10.0f, 300.0f);

        settings.misc.noRecoil = ReadBool(L"misc", L"no_recoil", settings.misc.noRecoil);
        settings.misc.noSpread = ReadBool(L"misc", L"no_spread", settings.misc.noSpread);
        settings.misc.autoPistol = ReadBool(L"misc", L"auto_pistol", settings.misc.autoPistol);
        settings.misc.infiniteHealth = ReadBool(L"misc", L"infinite_health", settings.misc.infiniteHealth);
        settings.misc.infiniteStamina = ReadBool(L"misc", L"infinite_stamina", settings.misc.infiniteStamina);

        settings.debug.showFps = ReadBool(L"debug", L"show_fps", settings.debug.showFps);
        settings.debug.showGraph = ReadBool(L"debug", L"show_graph", settings.debug.showGraph);
        settings.debug.showInternalStats =
            ReadBool(L"debug", L"show_internal_stats", settings.debug.showInternalStats);
        settings.debug.headlessAimbot = ReadBool(L"debug", L"headless_aimbot", settings.debug.headlessAimbot);

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
