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
        return lhs.showFps == rhs.showFps &&
               lhs.esp.enabled == rhs.esp.enabled &&
               lhs.esp.boundingBoxes == rhs.esp.boundingBoxes &&
               lhs.esp.skeleton == rhs.esp.skeleton &&
               lhs.esp.healthBars == rhs.esp.healthBars &&
               lhs.esp.nativeHighlight == rhs.esp.nativeHighlight &&
               lhs.esp.hideDead == rhs.esp.hideDead &&
               lhs.esp.showCivilians == rhs.esp.showCivilians &&
               lhs.esp.showEnemies == rhs.esp.showEnemies &&
               lhs.esp.showPolice == rhs.esp.showPolice &&
               lhs.esp.showUnclassified == rhs.esp.showUnclassified &&
               lhs.esp.maxDistanceMeters == rhs.esp.maxDistanceMeters &&
               lhs.aimbot.enabled == rhs.aimbot.enabled &&
               lhs.aimbot.drawFovCircle == rhs.aimbot.drawFovCircle &&
               lhs.aimbot.targetEnemies == rhs.aimbot.targetEnemies &&
               lhs.aimbot.targetPolice == rhs.aimbot.targetPolice &&
               lhs.aimbot.fovRadiusPixels == rhs.aimbot.fovRadiusPixels &&
               lhs.aimbot.smoothing == rhs.aimbot.smoothing &&
               lhs.aimbot.maxDistanceMeters == rhs.aimbot.maxDistanceMeters;
    }

    bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback)
    {
        return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, g_configPath) != 0;
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

    bool WriteFloat(const wchar_t* section, const wchar_t* key, float value)
    {
        wchar_t text[32]{};
        swprintf_s(text, L"%.4f", value);
        return WriteValue(section, key, text);
    }

    bool Save(const Features::Settings& settings)
    {
        bool ok = true;
        ok &= WriteValue(L"trainer", L"version", L"1");
        ok &= WriteBool(L"trainer", L"show_fps", settings.showFps);

        ok &= WriteBool(L"esp", L"enabled", settings.esp.enabled);
        ok &= WriteBool(L"esp", L"bounding_boxes", settings.esp.boundingBoxes);
        ok &= WriteBool(L"esp", L"skeleton", settings.esp.skeleton);
        ok &= WriteBool(L"esp", L"health_bars", settings.esp.healthBars);
        ok &= WriteBool(L"esp", L"native_highlight", settings.esp.nativeHighlight);
        ok &= WriteBool(L"esp", L"hide_dead", settings.esp.hideDead);
        ok &= WriteBool(L"esp", L"show_civilians", settings.esp.showCivilians);
        ok &= WriteBool(L"esp", L"show_enemies", settings.esp.showEnemies);
        ok &= WriteBool(L"esp", L"show_police", settings.esp.showPolice);
        ok &= WriteBool(L"esp", L"show_unclassified", settings.esp.showUnclassified);
        ok &= WriteFloat(L"esp", L"max_distance_meters", settings.esp.maxDistanceMeters);

        ok &= WriteBool(L"aimbot", L"enabled", settings.aimbot.enabled);
        ok &= WriteBool(L"aimbot", L"draw_fov_circle", settings.aimbot.drawFovCircle);
        ok &= WriteBool(L"aimbot", L"target_enemies", settings.aimbot.targetEnemies);
        ok &= WriteBool(L"aimbot", L"target_police", settings.aimbot.targetPolice);
        ok &= WriteFloat(L"aimbot", L"fov_radius_pixels", settings.aimbot.fovRadiusPixels);
        ok &= WriteFloat(L"aimbot", L"smoothing", settings.aimbot.smoothing);
        ok &= WriteFloat(L"aimbot", L"max_distance_meters", settings.aimbot.maxDistanceMeters);

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
        settings.showFps = ReadBool(L"trainer", L"show_fps", settings.showFps);

        settings.esp.enabled = ReadBool(L"esp", L"enabled", settings.esp.enabled);
        settings.esp.boundingBoxes = ReadBool(L"esp", L"bounding_boxes", settings.esp.boundingBoxes);
        settings.esp.skeleton = ReadBool(L"esp", L"skeleton", settings.esp.skeleton);
        settings.esp.healthBars = ReadBool(L"esp", L"health_bars", settings.esp.healthBars);
        settings.esp.nativeHighlight = ReadBool(L"esp", L"native_highlight", settings.esp.nativeHighlight);
        settings.esp.hideDead = ReadBool(L"esp", L"hide_dead", settings.esp.hideDead);
        settings.esp.showCivilians = ReadBool(L"esp", L"show_civilians", settings.esp.showCivilians);
        settings.esp.showEnemies = ReadBool(L"esp", L"show_enemies", settings.esp.showEnemies);
        settings.esp.showPolice = ReadBool(L"esp", L"show_police", settings.esp.showPolice);
        settings.esp.showUnclassified = ReadBool(L"esp", L"show_unclassified", settings.esp.showUnclassified);
        settings.esp.maxDistanceMeters =
            ReadFloat(L"esp", L"max_distance_meters", settings.esp.maxDistanceMeters, 10.0f, 300.0f);

        settings.aimbot.enabled = ReadBool(L"aimbot", L"enabled", settings.aimbot.enabled);
        settings.aimbot.drawFovCircle =
            ReadBool(L"aimbot", L"draw_fov_circle", settings.aimbot.drawFovCircle);
        settings.aimbot.targetEnemies = ReadBool(L"aimbot", L"target_enemies", settings.aimbot.targetEnemies);
        settings.aimbot.targetPolice = ReadBool(L"aimbot", L"target_police", settings.aimbot.targetPolice);
        settings.aimbot.fovRadiusPixels =
            ReadFloat(L"aimbot", L"fov_radius_pixels", settings.aimbot.fovRadiusPixels, 40.0f, 600.0f);
        settings.aimbot.smoothing = ReadFloat(L"aimbot", L"smoothing", settings.aimbot.smoothing, 1.0f, 30.0f);
        settings.aimbot.maxDistanceMeters =
            ReadFloat(L"aimbot", L"max_distance_meters", settings.aimbot.maxDistanceMeters, 10.0f, 300.0f);

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
