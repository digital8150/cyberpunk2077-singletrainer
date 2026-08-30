#include "widgets.h"

#include "localization.h"
#include "theme.h"
#include "ui_kit.h"
#include "../features/aimbot.h"
#include "../features/features.h"
#include "../game/entity_tracker.h"
#include "../game/player_modifiers.h"
#include "../game/silent_aim.h"
#include "../game/visibility.h"
#include "../framework.h"  // GetTickCount64

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace Widgets
{
    namespace
    {
        using UiKit::Font;
        using UiKit::Icon;
        namespace M = UiKit::Metrics;

        enum class Page
        {
            Aimbot,
            Esp,
            Misc,
            Debug,
        };

        Page g_activePage = Page::Aimbot;
        bool g_silentDiagnosticsOpen = false;

        float EaseInOutCubic(float value)
        {
            const float t = std::clamp(value, 0.0f, 1.0f);
            return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        }

        // 통계 문자열은 한 프레임 안에서 여러 개가 동시에 살아 있어야 한다 (MetricRow가 값을 바로
        // 그리긴 하지만, 호출자가 여러 개를 연달아 만든다). 회전 버퍼로 충돌을 피한다.
        const char* Format(const char* format, ...)
        {
            static char buffers[12][256];
            static int next = 0;
            char* target = buffers[next];
            next = (next + 1) % 12;
            va_list args;
            va_start(args, format);
            vsnprintf(target, sizeof(buffers[0]), format, args);
            va_end(args);
            return target;
        }

        const char* Count(unsigned long long value)
        {
            return Format("%llu", value);
        }

        // ── 사이드바 ─────────────────────────────────────────────────────────
        void NavGroupLabel(Loc::Str label)
        {
            const ImVec2 position = ImGui::GetCursorScreenPos();
            UiKit::PaintText(ImGui::GetWindowDrawList(), Font::Micro, ImVec2(position.x + 2.0f, position.y + 5.0f),
                             UiTheme::Current().textDisabled, Loc::Text(label));
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, UiKit::FontSize(Font::Micro) + 11.0f));
        }

        void NavItem(const char* id, Icon icon, Loc::Str label, Page page)
        {
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            constexpr float kHeight = 30.0f;

            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(width, kHeight));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const bool selected = g_activePage == page;
            const UiTheme::Palette& palette = UiTheme::Current();
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // 선택 표시는 옅은 면 + 왼쪽 액센트 바까지다. 항목 전체를 진한 파란 블록으로 채우면
            // 화면에서 가장 강한 색이 "지금 어느 페이지인지"라는 가장 덜 중요한 정보에 붙는다.
            if (selected)
            {
                drawList->AddRectFilled(minimum, maximum, palette.accentSoft, 6.0f);
                drawList->AddRectFilled(ImVec2(minimum.x, minimum.y + 7.0f),
                                        ImVec2(minimum.x + 2.0f, maximum.y - 7.0f), palette.accent, 1.0f);
            }
            else if (hovered)
            {
                drawList->AddRectFilled(minimum, maximum, palette.surfaceHovered, 6.0f);
            }

            const float centerY = (minimum.y + maximum.y) * 0.5f;
            const ImU32 tint = selected ? palette.accent : palette.textSecondary;
            UiKit::DrawIcon(drawList, icon, ImVec2(minimum.x + 17.0f, centerY), 14.0f, tint);
            const ImVec2 textSize = UiKit::MeasureText(Font::Body, Loc::Text(label));
            UiKit::PaintText(drawList, Font::Body, ImVec2(minimum.x + 32.0f, centerY - textSize.y * 0.5f),
                             selected ? palette.text : palette.textSecondary, Loc::Text(label));

            if (pressed)
                g_activePage = page;
        }

        void DrawSidebar()
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);

            // 애플리케이션 식별자. 채워진 배너가 아니라 조용한 필드 하나다.
            constexpr float kBrandHeight = 30.0f;
            const ImVec2 brandMin = ImGui::GetCursorScreenPos();
            const ImVec2 brandMax(brandMin.x + width, brandMin.y + kBrandHeight);
            drawList->AddRectFilled(brandMin, brandMax, palette.surface, 6.0f);
            drawList->AddRect(brandMin, brandMax, palette.border, 6.0f, 1.0f);
            const ImVec2 markMin(brandMin.x + 8.0f, brandMin.y + 8.0f);
            drawList->AddRectFilled(markMin, ImVec2(markMin.x + 14.0f, markMin.y + 14.0f), palette.accent, 4.0f);
            UiKit::DrawIcon(drawList, Icon::Crosshair, ImVec2(markMin.x + 7.0f, markMin.y + 7.0f), 9.0f,
                            palette.knob);
            const ImVec2 brandSize = UiKit::MeasureText(Font::Section, Loc::Text(Loc::Str::BrandName));
            UiKit::PaintText(drawList, Font::Section,
                             ImVec2(markMin.x + 21.0f, (brandMin.y + brandMax.y) * 0.5f - brandSize.y * 0.5f),
                             palette.text, Loc::Text(Loc::Str::BrandName));
            ImGui::Dummy(ImVec2(width, kBrandHeight + 10.0f));

            NavGroupLabel(Loc::Str::NavGroupGeneral);
            NavItem("nav_aimbot", Icon::Crosshair, Loc::Str::TabAimbot, Page::Aimbot);

            NavGroupLabel(Loc::Str::NavGroupVisuals);
            NavItem("nav_esp", Icon::Eye, Loc::Str::TabEsp, Page::Esp);

            NavGroupLabel(Loc::Str::NavGroupSystem);
            NavItem("nav_misc", Icon::Sliders, Loc::Str::TabMisc, Page::Misc);
            NavItem("nav_debug", Icon::Terminal, Loc::Str::TabDebug, Page::Debug);

            // 빌드 표시는 기둥 맨 아래에 고정한다. 내비게이션 흐름에 섞이면 항목 하나처럼 읽힌다.
            const float stampHeight = UiKit::FontSize(Font::Micro) + 2.0f;
            const float bottom = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - stampHeight;
            if (bottom > ImGui::GetCursorPosY())
                ImGui::SetCursorPosY(bottom);
            const ImVec2 stampPosition = ImGui::GetCursorScreenPos();
            UiKit::PaintText(drawList, Font::Micro, ImVec2(stampPosition.x + 2.0f, stampPosition.y),
                             palette.textDisabled,
                             Format("%s  %s", Loc::Text(Loc::Str::BuiltOn), __DATE__));
            // SetCursorPosY로 커서만 옮기고 끝내면 ImGui가 "경계를 넓히기만 하고 항목을 넣지
            // 않았다"고 보고한다. 실제로 차지한 만큼 항목으로 잡아 준다.
            ImGui::Dummy(ImVec2(width, stampHeight));
        }

        // ── 상태 표시줄 ──────────────────────────────────────────────────────
        bool StatusPicker(const char* id, const char* value)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const ImVec2 textSize = UiKit::MeasureText(Font::Small, value);
            const ImVec2 size(textSize.x + 26.0f, 20.0f);

            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton("##picker", size);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (hovered)
                drawList->AddRectFilled(minimum, maximum, palette.surfaceHovered, 4.0f);
            UiKit::PaintText(drawList, Font::Small,
                             ImVec2(minimum.x + 7.0f, (minimum.y + maximum.y) * 0.5f - textSize.y * 0.5f),
                             hovered ? palette.text : palette.textSecondary, value);
            const float caretX = maximum.x - 12.0f;
            const float caretY = (minimum.y + maximum.y) * 0.5f - 1.0f;
            drawList->AddTriangleFilled(ImVec2(caretX - 3.5f, caretY), ImVec2(caretX + 3.5f, caretY),
                                        ImVec2(caretX, caretY + 3.5f), palette.textDisabled);
            return pressed;
        }

        void DrawStatusBar(Features::Settings& settings)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (M::kFooterHeight - 20.0f) * 0.5f);

            const bool light = settings.ui.theme == Features::Theme::Light;
            if (StatusPicker("theme", Loc::Text(light ? Loc::Str::Light : Loc::Str::Dark)))
                settings.ui.theme = light ? Features::Theme::Dark : Features::Theme::Light;

            ImGui::SameLine(0.0f, 2.0f);
            const bool korean = settings.ui.language == Features::Language::Korean;
            ImGui::PushID("language");
            if (StatusPicker("picker", Loc::Text(korean ? Loc::Str::Korean : Loc::Str::English)))
                ImGui::OpenPopup("##language_popup");
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
            UiKit::PushFont(Font::Body);
            if (ImGui::BeginPopup("##language_popup"))
            {
                if (ImGui::Selectable(Loc::Text(Loc::Str::Korean), korean, 0, ImVec2(96.0f, 22.0f)))
                    settings.ui.language = Features::Language::Korean;
                if (ImGui::Selectable(Loc::Text(Loc::Str::English), !korean, 0, ImVec2(96.0f, 22.0f)))
                    settings.ui.language = Features::Language::English;
                ImGui::EndPopup();
            }
            UiKit::PopFont();
            ImGui::PopStyleVar();
            ImGui::PopID();

            const char* hotkeys = Loc::Text(Loc::Str::FooterHotkeys);
            const ImVec2 hotkeySize = UiKit::MeasureText(Font::Micro, hotkeys);
            UiKit::PaintText(ImGui::GetWindowDrawList(), Font::Micro,
                             ImVec2(origin.x + width - hotkeySize.x,
                                    origin.y + (M::kFooterHeight - hotkeySize.y) * 0.5f),
                             palette.textDisabled, hotkeys);
        }

        // ── 페이지 ──────────────────────────────────────────────────────────
        void DrawAimbotPage(Features::Settings& settings)
        {
            Features::AimbotSettings& aimbot = settings.aimbot;
            const float width = UiKit::ContentWidth();
            UiKit::ColumnsBegin(UiKit::ResolveColumnCount(2, width), width);

            UiKit::Column();
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionGeneral));
            UiKit::ToggleRow("aimbot_enabled", Loc::Text(Loc::Str::AimbotEnabled), &aimbot.enabled,
                             aimbot.enabled ? nullptr : Loc::Text(Loc::Str::AimbotDisabledNote));
            UiKit::BeginDisabled(!aimbot.enabled);
            {
                int mode = aimbot.silentAim ? 1 : 0;
                const char* modes[] = {Loc::Text(Loc::Str::AimModeClassic), Loc::Text(Loc::Str::AimModeSilent)};
                if (UiKit::ComboRow("aim_mode", Loc::Text(Loc::Str::AimMode), &mode, modes, 2))
                    aimbot.silentAim = mode == 1;

                UiKit::KeybindRow("activation_key", Loc::Text(Loc::Str::ActivationKey), &aimbot.activationKey);
                UiKit::HelperText(Format(aimbot.silentAim ? Loc::Text(Loc::Str::ActivationSilentHint)
                                                          : Loc::Text(Loc::Str::ActivationClassicHint),
                                         UiKit::KeyName(aimbot.activationKey)));
            }
            UiKit::EndDisabled();
            UiKit::SectionEnd();

            UiKit::BeginDisabled(!aimbot.enabled);
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionAiming));
            UiKit::ToggleRow("fov_circle", Loc::Text(Loc::Str::DrawFovCircle), &aimbot.drawFovCircle);
            UiKit::SliderRow("fov_radius", Loc::Text(Loc::Str::FovRadius), &aimbot.fovRadiusDegrees, 1.0f, 60.0f,
                             Loc::Text(Loc::Str::DegreesFormat));
            UiKit::BeginDisabled(aimbot.silentAim);
            UiKit::SliderRow("smoothing", Loc::Text(Loc::Str::Smoothing), &aimbot.smoothing, 0.0f, 30.0f,
                             Loc::Text(Loc::Str::DecimalFormat),
                             aimbot.silentAim ? Loc::Text(Loc::Str::SmoothingSilentNote) : nullptr);
            UiKit::EndDisabled();
            UiKit::SliderRow("aim_distance", Loc::Text(Loc::Str::AimDistance), &aimbot.maxDistanceMeters, 10.0f,
                             300.0f, Loc::Text(Loc::Str::MetersFormat));
            UiKit::SectionEnd();
            UiKit::EndDisabled();

            UiKit::Column();
            UiKit::BeginDisabled(!aimbot.enabled);
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionTargeting));
            UiKit::CheckRow("target_enemies", Loc::Text(Loc::Str::TargetEnemies), &aimbot.targetEnemies);
            UiKit::CheckRow("target_police", Loc::Text(Loc::Str::TargetPolice), &aimbot.targetPolice);
            UiKit::CheckRow("visible_only", Loc::Text(Loc::Str::OnlyVisibleTargets), &aimbot.visibleOnly,
                            aimbot.visibleOnly ? Loc::Text(Loc::Str::VisibleOnlyHint) : nullptr);
            UiKit::CheckRow("require_health", Loc::Text(Loc::Str::RequireHealthPool), &aimbot.requireHealthPool);
            UiKit::CheckRow("limit_health", Loc::Text(Loc::Str::LimitHealthPool), &aimbot.limitHealthPool);
            UiKit::BeginDisabled(!aimbot.limitHealthPool);
            UiKit::SliderRow("max_health", Loc::Text(Loc::Str::MaxHealthPool), &aimbot.maxHealthPool, 500.0f,
                             6000.0f, Loc::Text(Loc::Str::HealthFormat));
            UiKit::EndDisabled();
            if (aimbot.requireHealthPool || aimbot.limitHealthPool)
                UiKit::HelperText(Loc::Text(Loc::Str::HealthFilterHint));
            UiKit::SectionEnd();
            UiKit::EndDisabled();

            UiKit::ColumnsEnd();
        }

        void DrawEspPage(Features::Settings& settings)
        {
            Features::EspSettings& esp = settings.esp;
            const float width = UiKit::ContentWidth();
            UiKit::ColumnsBegin(UiKit::ResolveColumnCount(2, width), width);

            UiKit::Column();
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionGeneral));
            UiKit::ToggleRow("esp_enabled", Loc::Text(Loc::Str::EspEnabled), &esp.enabled,
                             esp.enabled ? nullptr : Loc::Text(Loc::Str::EspDisabledNote));
            UiKit::SectionEnd();

            UiKit::BeginDisabled(!esp.enabled);
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionVisuals));
            UiKit::ToggleRow("boxes", Loc::Text(Loc::Str::BoundingBoxes), &esp.boundingBoxes);
            UiKit::ToggleRow("skeleton", Loc::Text(Loc::Str::Skeleton), &esp.skeleton);
            UiKit::ToggleRow("health_bars", Loc::Text(Loc::Str::HealthBars), &esp.healthBars);
            UiKit::ToggleRow("native_highlight", Loc::Text(Loc::Str::NativeHighlight), &esp.nativeHighlight);
            UiKit::SectionEnd();
            UiKit::EndDisabled();

            UiKit::Column();
            UiKit::BeginDisabled(!esp.enabled);
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionFilters));
            UiKit::CheckRow("civilians", Loc::Text(Loc::Str::Civilians), &esp.showCivilians);
            UiKit::CheckRow("enemies", Loc::Text(Loc::Str::Enemies), &esp.showEnemies);
            UiKit::CheckRow("police", Loc::Text(Loc::Str::Police), &esp.showPolice);
            UiKit::CheckRow("unclassified", Loc::Text(Loc::Str::Unclassified), &esp.showUnclassified);
            UiKit::SectionEnd();

            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionVisibility));
            UiKit::CheckRow("hide_dead", Loc::Text(Loc::Str::HideDeadNpcs), &esp.hideDead);
            UiKit::CheckRow("visibility_check", Loc::Text(Loc::Str::VisibilityCheck), &esp.visibilityCheck);
            if (esp.visibilityCheck)
                UiKit::WarningText(Loc::Text(Loc::Str::VisibilityPerformanceHint));
            UiKit::BeginDisabled(!esp.visibilityCheck);
            UiKit::CheckRow("hide_occluded", Loc::Text(Loc::Str::HideOccludedNpcs), &esp.hideOccluded);
            UiKit::EndDisabled();
            UiKit::SliderRow("max_distance", Loc::Text(Loc::Str::MaxDistance), &esp.maxDistanceMeters, 10.0f,
                             300.0f, Loc::Text(Loc::Str::MetersFormat));
            UiKit::SectionEnd();
            UiKit::EndDisabled();

            UiKit::ColumnsEnd();
        }

        void DrawMiscPage(Features::Settings& settings)
        {
            // 설정이 둘뿐이라 2열로 나눌 것이 없다. 열 하나는 UiKit이 최대 폭으로 잘라 준다.
            UiKit::ColumnsBegin(1, UiKit::ContentWidth(), false);
            UiKit::Column();
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionWeapon));
            UiKit::ToggleRow("no_recoil", Loc::Text(Loc::Str::NoRecoil), &settings.misc.noRecoil);
            UiKit::ToggleRow("no_spread", Loc::Text(Loc::Str::NoSpread), &settings.misc.noSpread);
            UiKit::HelperText(Loc::Text(Loc::Str::WeaponNote));
            UiKit::SectionEnd();
            UiKit::ColumnsEnd();
        }

        void DrawRuntimeStatistics()
        {
            const Game::EntityTracker::Stats entity = Game::EntityTracker::GetStats();
            const Game::PlayerModifiers::Stats modifiers = Game::PlayerModifiers::GetStats();
            const Aimbot::Stats aim = Aimbot::GetStats();
            const Game::Visibility::Stats visibility = Game::Visibility::GetStats();
            const Game::SilentAim::DiagnosticsSnapshot silent = Game::SilentAim::GetDiagnostics();
            const UiTheme::Palette& palette = UiTheme::Current();

            const float width = UiKit::ContentWidth();
            UiKit::ColumnsBegin(UiKit::ResolveColumnCount(2, width), width);

            // 카운터 이름은 로그와 같은 용어를 써야 대조가 되므로 영어를 유지한다.
            UiKit::Column();
            UiKit::SectionBegin(Loc::Text(Loc::Str::RuntimeStatistics));
            UiKit::MetricGroup("Entity feed", entity.hookCreated ? "hooked" : "unavailable",
                               entity.hookCreated ? palette.success : palette.warning);
            UiKit::MetricRow("registered", Count(entity.registered));
            UiKit::MetricRow("positioned", Count(entity.positioned));
            UiKit::MetricRow("NPCs", Count(entity.puppets));
            UiKit::MetricRow("live", Count(entity.trackedPuppets));

            UiKit::MetricGroup("Classified live", nullptr, 0);
            UiKit::MetricRow("civilian", Count(entity.trackedCivilians));
            UiKit::MetricRow("enemy", Count(entity.trackedEnemies));
            UiKit::MetricRow("police", Count(entity.trackedPolice));
            UiKit::MetricRow("hostile", Count(entity.trackedHostile));

            UiKit::MetricGroup("Attitude and health", nullptr, 0);
            UiKit::MetricRow("attitude resolved", Count(entity.attitudeValid));
            UiKit::MetricRow("attitude unavailable", Count(entity.attitudeInvalid));
            UiKit::MetricRow("health valid", Count(entity.healthValid));
            UiKit::MetricRow("health fallback", Count(entity.healthInvalid));
            UiKit::MetricRow("pending position", Count(entity.pendingPosition));

            UiKit::MetricGroup("Native highlight", nullptr, 0);
            UiKit::MetricRow("queued", Count(entity.nativeHighlightQueued));
            UiKit::MetricRow("cleared", Count(entity.nativeHighlightCleared));
            UiKit::MetricRow("failures", Count(entity.nativeHighlightFailures));
            UiKit::SectionEnd();

            UiKit::Column();
            UiKit::SectionBegin(Loc::Text(Loc::Str::SilentAimDiagnostics));
            UiKit::MetricGroup("Targets", nullptr, 0);
            UiKit::MetricRow("candidates", Count(aim.candidates));
            UiKit::MetricRow("eligible", Count(aim.eligible));
            UiKit::MetricRow("no pool", Count(aim.skippedNoHealthPool));
            UiKit::MetricRow("over cap", Count(aim.skippedHealthCap));
            UiKit::MetricRow("occluded", Count(aim.skippedOccluded));
            if (aim.targetEntityId != 0)
            {
                UiKit::MetricRow("selected",
                                 Format("0x%llX", static_cast<unsigned long long>(aim.targetEntityId)));
                UiKit::MetricRow(aim.targetHealthValid ? "health" : "health (unresolved)",
                                 Format("%.0f / %.0f", aim.targetHealth, aim.targetHealthMax));
            }

            UiKit::MetricGroup("Visibility cache", visibility.available ? "ready" : "unavailable",
                               visibility.available ? palette.success : palette.warning);
            UiKit::MetricRow("visible", Count(visibility.visible));
            UiKit::MetricRow("occluded", Count(visibility.occluded));
            UiKit::MetricRow("dropped", Count(visibility.dropped));

            UiKit::MetricGroup("No recoil",
                               modifiers.active ? "active" : (modifiers.available ? "ready" : "unavailable"),
                               (modifiers.active || modifiers.available) ? palette.success : palette.warning);
            UiKit::MetricRow(modifiers.usingWeaponTarget ? "weapon target" : "player fallback",
                             Format("0x%llX", static_cast<unsigned long long>(modifiers.targetId)));
            UiKit::MetricRow("applied", Count(modifiers.applied));
            UiKit::MetricRow("removed", Count(modifiers.removed));
            UiKit::MetricRow("retired", Count(modifiers.retiredOwnerResets));
            UiKit::MetricRow("failures", Count(modifiers.failures));

            UiKit::MetricGroup("Silent aim",
                               silent.crosshairCoreHookCreated ? "crosshair core hooked" : "unavailable",
                               silent.crosshairCoreHookCreated ? palette.success : palette.warning);
            UiKit::MetricRow("redirects", Count(silent.nativeCrosshairCoreRedirects));
            UiKit::MetricRow("rejected", Count(silent.rejectedShots));
            if (UiKit::CollapsibleRow("silent_detail", Loc::Text(Loc::Str::SilentAimDiagnostics),
                                      &g_silentDiagnosticsOpen))
            {
                UiKit::MetricRow("crosshair core calls", Count(silent.nativeCrosshairCoreCalls));
                if (silent.producerHooks == 0 && silent.listenerHooks == 0)
                {
                    UiKit::MetricGroup("Observation hooks", "disabled in this build", palette.textDisabled);
                }
                else
                {
                    UiKit::MetricRow("producer hooks", Count(silent.producerHooks));
                    UiKit::MetricRow("projectile hooks", Count(silent.listenerHooks));
                    UiKit::MetricRow("effect runs", Count(silent.effectRuns));
                    UiKit::MetricRow("attack starts", Count(silent.attackStarts));
                    UiKit::MetricRow("attack prepares", Count(silent.attackPrepares));
                    UiKit::MetricRow("crosshair calls", Count(silent.crosshairCalls));
                    UiKit::MetricRow("default crosshair", Count(silent.defaultCrosshairCalls));
                    UiKit::MetricRow("projectile events", Count(silent.projectileEvents));
                    UiKit::MetricRow("local player events", Count(silent.localPlayerEvents));
                }
            }
            UiKit::SectionEnd();

            UiKit::ColumnsEnd();
        }

        void DrawDebugPage(Features::Settings& settings)
        {
            Features::DebugSettings& debug = settings.debug;
            const float width = UiKit::ContentWidth();
            UiKit::ColumnsBegin(UiKit::ResolveColumnCount(2, width), width);

            UiKit::Column();
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionOverlay));
            UiKit::ToggleRow("show_fps", Loc::Text(Loc::Str::ShowFps), &debug.showFps);
            UiKit::ToggleRow("show_graph", Loc::Text(Loc::Str::ShowGraph), &debug.showGraph);
            UiKit::BeginDisabled(!debug.showGraph);
            UiKit::SliderRow("graph_opacity", Loc::Text(Loc::Str::GraphOpacity), &debug.graphOpacityPercent,
                             35.0f, 100.0f, Loc::Text(Loc::Str::PercentFormat));
            UiKit::EndDisabled();
            UiKit::ToggleRow("show_stats", Loc::Text(Loc::Str::ShowInternalStats), &debug.showInternalStats,
                             Loc::Text(Loc::Str::ShowInternalStatsHelp));
            UiKit::SectionEnd();

            UiKit::SectionBeginTagged(Loc::Text(Loc::Str::SectionExperimental), Loc::Text(Loc::Str::TagUnstable),
                                      UiTheme::Current().warning);
            UiKit::HelperText(Loc::Text(Loc::Str::ExperimentalNote));
            UiKit::ToggleRow("headless_aimbot", Loc::Text(Loc::Str::HeadlessAimbot), &debug.headlessAimbot,
                             Loc::Text(Loc::Str::HeadlessAimbotNote));
            if (debug.headlessAimbot)
                UiKit::WarningText(Loc::Text(Loc::Str::HeadlessAimbotHint));
            UiKit::SectionEnd();

            UiKit::Column();
            UiKit::SectionBegin(Loc::Text(Loc::Str::SectionDiagnostics));
            UiKit::ToggleRow("logging", Loc::Text(Loc::Str::DiagnosticLogging), &debug.diagnosticLogging,
                             Loc::Text(Loc::Str::DiagnosticLoggingHelp));
            UiKit::ToggleRow("crash_reporting", Loc::Text(Loc::Str::CrashReporting), &debug.crashReporting,
                             Loc::Text(Loc::Str::CrashReportingHelp));
            UiKit::ToggleRow("profiling", Loc::Text(Loc::Str::PerformanceProfiling), &debug.performanceProfiling,
                             Loc::Text(Loc::Str::PerformanceProfilingHelp));
            UiKit::ToggleRow("debugger_output", Loc::Text(Loc::Str::DebuggerOutput), &debug.debuggerOutput,
                             Loc::Text(Loc::Str::DebuggerOutputHelp));
            UiKit::SectionEnd();

            UiKit::ColumnsEnd();

            // 런타임 수치는 설정이 아니다. 설정 흐름 아래에 따로, 접을 수 있게 둔다.
            if (debug.showInternalStats)
                DrawRuntimeStatistics();
        }

        struct PageInfo
        {
            Icon icon;
            Loc::Str title;
            Loc::Str description;
        };

        PageInfo CurrentPageInfo()
        {
            switch (g_activePage)
            {
            case Page::Esp: return {Icon::Eye, Loc::Str::TabEsp, Loc::Str::EspDescription};
            case Page::Misc: return {Icon::Sliders, Loc::Str::TabMisc, Loc::Str::MiscDescription};
            case Page::Debug: return {Icon::Terminal, Loc::Str::TabDebug, Loc::Str::DebugDescription};
            default: break;
            }
            return {Icon::Crosshair, Loc::Str::TabAimbot, Loc::Str::AimbotDescription};
        }

        void DrawPageBody(Features::Settings& settings)
        {
            switch (g_activePage)
            {
            case Page::Esp: DrawEspPage(settings); break;
            case Page::Misc: DrawMiscPage(settings); break;
            case Page::Debug: DrawDebugPage(settings); break;
            default: DrawAimbotPage(settings); break;
            }
        }
    }

    void SelectPage(int index)
    {
        switch (index)
        {
        case 1: g_activePage = Page::Esp; break;
        case 2: g_activePage = Page::Misc; break;
        case 3: g_activePage = Page::Debug; break;
        default: g_activePage = Page::Aimbot; break;
        }
    }

    void ApplyStyle()
    {
        UiKit::LoadFonts();
        UiTheme::ApplyImGuiStyle();
    }

    void DrawMainMenu()
    {
        UiKit::BeginFrame();

        ImGui::SetNextWindowSize(ImVec2(960.0f, 700.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(760.0f, 520.0f), ImVec2(1600.0f, 1200.0f));
        if (!ImGui::Begin("##trainer_main_window", nullptr,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImGui::End();
            return;
        }

        Features::Settings& settings = Features::GetSettings();
        const UiTheme::Palette& palette = UiTheme::Current();
        const ImVec2 windowMin = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 windowMax(windowMin.x + windowSize.x, windowMin.y + windowSize.y);
        const float bodyHeight = (std::max)(1.0f, windowSize.y - M::kFooterHeight);

        // 창 그림자는 창 뒤에 깔린다. 창 자체의 드로우리스트에 그리면 창 사각형에 잘려서 보이지 않는다.
        ImDrawList* behind = ImGui::GetBackgroundDrawList();
        for (int layer = 4; layer >= 1; --layer)
        {
            const float spread = static_cast<float>(layer) * 2.5f;
            behind->AddRectFilled(ImVec2(windowMin.x - spread, windowMin.y - spread + 2.0f),
                                  ImVec2(windowMax.x + spread, windowMax.y + spread + 2.0f),
                                  UiTheme::WithAlpha(palette.shadow, 14), 8.0f + spread);
        }

        // 구역은 선이 아니라 면으로 나눈다. 사이드바와 상태 표시줄만 자기 면을 갖고, 본문은 창 바닥색이다.
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(windowMin, ImVec2(windowMin.x + M::kSidebarWidth, windowMin.y + bodyHeight),
                                palette.sidebar, 8.0f, ImDrawFlags_RoundCornersTopLeft);
        drawList->AddRectFilled(ImVec2(windowMin.x + M::kSidebarWidth - 1.0f, windowMin.y),
                                ImVec2(windowMin.x + M::kSidebarWidth, windowMin.y + bodyHeight), palette.border);
        drawList->AddRectFilled(ImVec2(windowMin.x, windowMin.y + bodyHeight), windowMax, palette.sidebar, 8.0f,
                                ImDrawFlags_RoundCornersBottom);
        drawList->AddRectFilled(ImVec2(windowMin.x, windowMin.y + bodyHeight),
                                ImVec2(windowMax.x, windowMin.y + bodyHeight + 1.0f), palette.border);

        // 레이아웃은 실제 계층으로 만든다. 본문 높이를 먼저 빼두고 그 안에서만 스크롤하므로
        // 상태 표시줄이 콘텐츠를 덮을 수 없다.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 14.0f));
        ImGui::BeginChild("##sidebar", ImVec2(M::kSidebarWidth, bodyHeight),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        DrawSidebar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##content", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        const PageInfo page = CurrentPageInfo();
        UiKit::PageHeader(page.icon, Loc::Text(page.title), Loc::Text(page.description));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(M::kContentPadding, 14.0f));
        ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground);
        UiKit::SetContentWidth(ImGui::GetContentRegionAvail().x);
        DrawPageBody(settings);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 0.0f));
        ImGui::BeginChild("##status", ImVec2(0.0f, M::kFooterHeight),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        DrawStatusBar(settings);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::PopStyleVar();
        ImGui::End();
    }

    // 주입 직후의 안내. 메뉴와 같은 면·같은 글자 크기를 쓰되 창이 아니라 잠깐 떴다 사라지는 토스트다.
    void DrawStartupHint()
    {
        constexpr float kSlide = 0.45f;
        constexpr float kHold = 3.0f;
        constexpr float kWidth = 300.0f;
        constexpr float kHeight = 54.0f;
        constexpr float kRestingY = 20.0f;

        static ULONGLONG startedAt = 0;
        if (startedAt == 0)
            startedAt = GetTickCount64();

        const float elapsed = static_cast<float>(GetTickCount64() - startedAt) / 1000.0f;
        if (elapsed >= kSlide + kHold + kSlide)
            return;

        float progress = 1.0f;
        if (elapsed < kSlide)
            progress = EaseInOutCubic(elapsed / kSlide);
        else if (elapsed > kSlide + kHold)
            progress = 1.0f - EaseInOutCubic((elapsed - kSlide - kHold) / kSlide);

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= kWidth)
            return;

        const float hiddenY = -kHeight - 12.0f;
        const ImVec2 minimum((io.DisplaySize.x - kWidth) * 0.5f, hiddenY + (kRestingY - hiddenY) * progress);
        const ImVec2 maximum(minimum.x + kWidth, minimum.y + kHeight);
        const UiTheme::Palette& palette = UiTheme::Current();
        const int alpha = static_cast<int>(255.0f * progress);
        ImDrawList* drawList = ImGui::GetForegroundDrawList();

        drawList->AddRectFilled(minimum, maximum, UiTheme::WithAlpha(palette.surface, alpha), 8.0f);
        drawList->AddRect(minimum, maximum, UiTheme::WithAlpha(palette.border, alpha), 8.0f, 1.0f);
        drawList->AddRectFilled(ImVec2(minimum.x, minimum.y + 12.0f), ImVec2(minimum.x + 3.0f, maximum.y - 12.0f),
                                UiTheme::WithAlpha(palette.accent, alpha), 1.5f);

        UiKit::PaintText(drawList, Font::Section, ImVec2(minimum.x + 18.0f, minimum.y + 11.0f),
                         UiTheme::WithAlpha(palette.text, alpha), Loc::Text(Loc::Str::TrainerReady));

        const char* prefix = Loc::Text(Loc::Str::StartupOpenPrefix);
        const char* key = Loc::Text(Loc::Str::InsertKey);
        const char* suffix = Loc::Text(Loc::Str::StartupOpenSuffix);
        const ImVec2 prefixSize = UiKit::MeasureText(Font::Small, prefix);
        const ImVec2 keySize = UiKit::MeasureText(Font::Micro, key);
        const float baseline = minimum.y + 32.0f;
        float cursor = minimum.x + 18.0f;
        UiKit::PaintText(drawList, Font::Small, ImVec2(cursor, baseline),
                         UiTheme::WithAlpha(palette.textSecondary, alpha), prefix);
        cursor += prefixSize.x + 7.0f;
        const ImVec2 chipMin(cursor, baseline - 1.0f);
        const ImVec2 chipMax(chipMin.x + keySize.x + 14.0f, chipMin.y + keySize.y + 6.0f);
        drawList->AddRectFilled(chipMin, chipMax, UiTheme::WithAlpha(palette.surfaceActive, alpha), 4.0f);
        drawList->AddRect(chipMin, chipMax, UiTheme::WithAlpha(palette.border, alpha), 4.0f, 1.0f);
        UiKit::PaintText(drawList, Font::Micro, ImVec2(chipMin.x + 7.0f, chipMin.y + 3.0f),
                         UiTheme::WithAlpha(palette.text, alpha), key);
        cursor = chipMax.x + 7.0f;
        UiKit::PaintText(drawList, Font::Small, ImVec2(cursor, baseline),
                         UiTheme::WithAlpha(palette.textSecondary, alpha), suffix);
    }
}
