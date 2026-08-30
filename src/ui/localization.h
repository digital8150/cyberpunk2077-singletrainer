#pragma once

namespace Features
{
    enum class Language : unsigned;
}

namespace Loc
{
    // 일반 사용자 대면 문구는 이 ID를 통해서만 UI에 전달한다. 진단 카운터의 라벨은 의도적으로
    // 영어를 유지해 로그/수치와 같은 용어를 쓴다 (widgets.cpp의 런타임 통계 참고).
    enum class Str
    {
        BrandName,

        // 내비게이션
        NavGroupGeneral,
        NavGroupVisuals,
        NavGroupSystem,
        TabAimbot,
        TabEsp,
        TabMisc,
        TabDebug,
        AimbotDescription,
        EspDescription,
        MiscDescription,
        DebugDescription,

        // 상태 표시줄
        BuiltOn,
        FooterHotkeys,
        Korean,
        English,
        Dark,
        Light,

        // 섹션 제목
        SectionGeneral,
        SectionTargeting,
        SectionAiming,
        SectionVisuals,
        SectionFilters,
        SectionVisibility,
        SectionWeapon,
        SectionOverlay,
        SectionDiagnostics,
        SectionExperimental,
        TagUnstable,
        ExperimentalNote,

        // 에임봇
        AimbotEnabled,
        AimMode,
        AimModeClassic,
        AimModeSilent,
        ActivationKey,
        DrawFovCircle,
        FovRadius,
        Smoothing,
        AimDistance,
        TargetEnemies,
        TargetPolice,
        OnlyVisibleTargets,
        RequireHealthPool,
        LimitHealthPool,
        MaxHealthPool,
        ActivationSilentHint,
        ActivationClassicHint,
        VisibleOnlyHint,
        HealthFilterHint,
        SmoothingSilentNote,
        AimbotDisabledNote,

        // ESP
        EspEnabled,
        BoundingBoxes,
        Skeleton,
        HealthBars,
        NativeHighlight,
        Civilians,
        Enemies,
        Police,
        Unclassified,
        HideDeadNpcs,
        VisibilityCheck,
        HideOccludedNpcs,
        MaxDistance,
        VisibilityPerformanceHint,
        EspDisabledNote,

        // 기타
        NoRecoil,
        NoSpread,
        WeaponNote,

        // 디버그
        ShowFps,
        ShowGraph,
        ShowInternalStats,
        ShowInternalStatsHelp,
        DiagnosticLogging,
        DiagnosticLoggingHelp,
        CrashReporting,
        CrashReportingHelp,
        PerformanceProfiling,
        PerformanceProfilingHelp,
        DebuggerOutput,
        DebuggerOutputHelp,
        HeadlessAimbot,
        HeadlessAimbotNote,
        HeadlessAimbotHint,
        RuntimeStatistics,
        SilentAimDiagnostics,

        // 값 포맷
        MetersFormat,
        DegreesFormat,
        HealthFormat,
        DecimalFormat,

        // 시작 안내
        TrainerReady,
        StartupOpenPrefix,
        StartupOpenSuffix,
        InsertKey,

        // 키 이름
        PressKey,
        ReleaseKeys,
        KeyMouseLeft,
        KeyMouseRight,
        KeyMouseMiddle,
        KeyMouseFour,
        KeyMouseFive,
        KeyBackspace,
        KeyTab,
        KeyEnter,
        KeyShift,
        KeyRightShift,
        KeyControl,
        KeyRightControl,
        KeyAlt,
        KeyRightAlt,
        KeyCapsLock,
        KeySpace,
        KeyPageUp,
        KeyPageDown,
        KeyHome,
        KeyLeft,
        KeyUp,
        KeyRight,
        KeyDown,
        KeyDelete,
        KeyNumpadFormat,
        KeyFunctionFormat,
        KeyHexFormat,
    };

    const char* Text(Str id);
    const char* Text(Str id, Features::Language language);
}
