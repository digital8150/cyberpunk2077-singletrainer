#include "fps_counter.h"
#include "features.h"
#include "../profiling.h"
#include "../framework.h"
#include "../ui/ui_kit.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cfloat>

namespace FpsCounter
{
    namespace
    {
        using Slot = Diagnostics::Profile::Slot;
        constexpr unsigned kHistory = 120;
        constexpr float kWidth = 662.0f;
        constexpr ImU32 kText = IM_COL32(239, 245, 252, 255);
        constexpr ImU32 kMuted = IM_COL32(169, 187, 205, 255);
        constexpr ImU32 kPresent = IM_COL32(93, 213, 255, 255);
        constexpr ImU32 kTick = IM_COL32(123, 235, 172, 255);
        constexpr ImU32 kFrame = IM_COL32(255, 199, 104, 255);
        struct Row { const char* label; Slot slot; unsigned depth; unsigned branch; };
        constexpr Row kRows[] = {
            {"PRESENT / measured features", Slot::PresentTotal, 0, 0},
            {"Snapshot", Slot::SnapshotPass, 1, 0},
            {"Lock wait", Slot::SnapshotLockWait, 2, 0},
            {"Pose requests", Slot::PoseRequestPass, 1, 0},
            {"ESP draw", Slot::EspFrame, 1, 0},
            {"Aim selection / update", Slot::AimbotFrame, 1, 0},
            {"MAIN TICK / trainer", Slot::TickTotal, 0, 1},
            {"Pose", Slot::TickPose, 1, 1},
            {"Joint slot reads", Slot::PoseSlots, 2, 1},
            {"Health", Slot::TickHealth, 1, 1},
            {"Collect", Slot::HealthCollect, 2, 1},
            {"Invoke", Slot::HealthInvoke, 2, 1},
            {"Attitude", Slot::TickAttitude, 1, 1},
            {"Collect", Slot::AttitudeCollect, 2, 1},
            {"Invoke", Slot::AttitudeInvoke, 2, 1},
            {"Native highlight", Slot::TickHighlight, 1, 1},
            {"Collect", Slot::HighlightCollect, 2, 1},
            {"Player modifiers", Slot::TickPlayerModifiers, 1, 1},
            {"Visibility", Slot::TickVisibility, 1, 1},
        };
        struct History
        {
            std::array<float, kHistory> frame{}, peak{}, present{}, tick{};
            unsigned next = 0, count = 0, samples = 0;
            float elapsed = 0, frameSum = 0, framePeak = 0, presentSum = 0, tickSum = 0;
        } g_history;
        Diagnostics::Profile::WindowSnapshot g_window;
        bool g_wasEnabled = false;
        bool g_expanded[2] = {true, true};
        float g_alpha = 1.0f;
        float TextSize(UiKit::Font role)
        {
            return role == UiKit::Font::Mono ? 14.0f : (std::max)(12.0f, UiKit::FontSize(role));
        }

        ImU32 Ink(ImU32 color)
        {
            return (color & 0x00FFFFFFu) | (static_cast<ImU32>((color >> 24) * g_alpha) << 24);
        }
        void Text(ImDrawList* draw, float x, float y, const char* text, ImU32 color = kText,
                  UiKit::Font font = UiKit::Font::Micro)
        {
            draw->AddText(UiKit::FontFace(font), TextSize(font), ImVec2(x + 1, y + 1), Ink(IM_COL32(0, 0, 0, 220)), text);
            draw->AddText(UiKit::FontFace(font), TextSize(font), ImVec2(x, y), Ink(color), text);
        }
        void Right(ImDrawList* draw, float x, float y, const char* text, ImU32 color = kText)
        {
            const float width = UiKit::FontFace(UiKit::Font::Mono)->CalcTextSizeA(TextSize(UiKit::Font::Mono), FLT_MAX, 0, text).x;
            Text(draw, x - width, y, text, color, UiKit::Font::Mono);
        }
        void Rule(ImDrawList* draw, float x, float y, float width)
        {
            draw->AddLine(ImVec2(x, y), ImVec2(x + width, y), Ink(IM_COL32(190, 211, 230, 65)));
        }
        void Update(const ImGuiIO& io)
        {
            const float dt = std::isfinite(io.DeltaTime) && io.DeltaTime > 0 ? io.DeltaTime : 0;
            if (dt == 0) return;
            auto& h = g_history;
            h.elapsed += dt;
            h.frameSum += dt * 1000;
            h.framePeak = (std::max)(h.framePeak, dt * 1000);
            h.presentSum += static_cast<float>(Diagnostics::Profile::LastPresentMicroseconds());
            h.tickSum += static_cast<float>(Diagnostics::Profile::LastTickTotalMicroseconds());
            ++h.samples;
            if (h.elapsed < 0.1f) return;
            h.frame[h.next] = h.frameSum / h.samples;
            h.peak[h.next] = h.framePeak;
            h.present[h.next] = h.presentSum / h.samples;
            h.tick[h.next] = h.tickSum / h.samples;
            h.next = (h.next + 1) % kHistory;
            h.count = (std::min)(h.count + 1, kHistory);
            h.elapsed = h.frameSum = h.framePeak = h.presentSum = h.tickSum = 0;
            h.samples = 0;
        }
        void Curve(ImDrawList* draw, ImVec2 origin, float width, float height,
                   const std::array<float, kHistory>& values, float ceiling, ImU32 color, float thickness)
        {
            ImVec2 previous;
            for (unsigned i = 0; i < g_history.count; ++i)
            {
                const unsigned index = (g_history.next + kHistory - g_history.count + i) % kHistory;
                const float x = origin.x + width * (kHistory - g_history.count + i) / (kHistory - 1);
                const float y = origin.y + height * (1 - std::clamp(values[index] / ceiling, 0.0f, 1.0f));
                const ImVec2 point(x, y);
                if (i) draw->AddLine(previous, point, Ink(color), thickness);
                previous = point;
            }
        }
        void Plots(ImDrawList* draw, ImVec2 p)
        {
            const auto& h = g_history;
            const unsigned last = (h.next + kHistory - 1) % kHistory;
            char label[128];
            snprintf(label, sizeof(label), "%.0f FPS   /   %.2f ms", ImGui::GetIO().Framerate,
                     h.count ? h.frame[last] : 0);
            Text(draw, p.x, p.y, label, kFrame, UiKit::Font::Mono);
            Text(draw, p.x + 330, p.y, "PRESENT", kPresent);
            Text(draw, p.x + 450, p.y, "MAIN TICK", kTick);
            float frameTop = 16.67f, cpuTop = 100;
            for (unsigned i = 0; i < h.count; ++i)
            {
                const unsigned j = (h.next + kHistory - h.count + i) % kHistory;
                frameTop = (std::max)(frameTop, h.peak[j]);
                cpuTop = (std::max)(cpuTop, (std::max)(h.present[j], h.tick[j]));
            }
            frameTop *= 1.15f;
            cpuTop *= 1.15f;
            for (unsigned graph = 0; graph < 2; ++graph)
            {
                const float left = p.x + graph * 330;
                const float top = p.y + 28;
                for (unsigned line = 0; line < 3; ++line) Rule(draw, left, top + line * 25, 300);
                snprintf(label, sizeof(label), "0 - %.0f %s", graph ? cpuTop : frameTop, graph ? "us" : "ms");
                Text(draw, left, top + 54, label, kMuted);
            }
            Curve(draw, ImVec2(p.x, p.y + 28), 300, 50, h.peak, frameTop, IM_COL32(255, 199, 104, 90), 1);
            Curve(draw, ImVec2(p.x, p.y + 28), 300, 50, h.frame, frameTop, kFrame, 1.5f);
            if (Diagnostics::Profile::Enabled())
            {
                Curve(draw, ImVec2(p.x + 330, p.y + 28), 300, 50, h.present, cpuTop, kPresent, 1.5f);
                Curve(draw, ImVec2(p.x + 330, p.y + 28), 300, 50, h.tick, cpuTop, kTick, 1.5f);
            }
        }
        float Table(ImDrawList* draw, ImVec2 p, bool menuVisible, bool valid)
        {
            Text(draw, p.x, p.y, "SCOPE", kMuted);
            Text(draw, p.x + 294, p.y, "AVG us / F|T", kMuted);
            Text(draw, p.x + 424, p.y, "MAX us / CALL", kMuted);
            Text(draw, p.x + 552, p.y, "CALLS / F|T", kMuted);
            float y = p.y + 24;
            for (const auto& row : kRows)
            {
                if (row.depth && !g_expanded[row.branch]) continue;
                const ImU32 color = row.branch ? kTick : kPresent;
                const auto& metric = g_window.Get(row.slot);
                const auto& root = g_window.Get(row.branch ? Slot::TickTotal : Slot::PresentTotal);
                if (row.depth == 0)
                {
                    Rule(draw, p.x, y - 3, 630);
                    Text(draw, p.x, y, g_expanded[row.branch] ? "-" : "+", color);
                    if (menuVisible)
                    {
                        ImGui::PushID(static_cast<int>(row.branch));
                        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
                        ImGui::InvisibleButton("##branch", ImVec2(280, 20));
                        if (ImGui::IsItemClicked()) g_expanded[row.branch] = !g_expanded[row.branch];
                        ImGui::PopID();
                    }
                }
                Text(draw, p.x + 14 + row.depth * 13, y, row.label,
                     row.depth == 0 ? color : row.depth == 2 ? kMuted : kText);
                char value[48];
                if (valid && metric.count && root.count)
                {
                    snprintf(value, sizeof(value), "%.2f", metric.total / root.count);
                    Right(draw, p.x + 400, y, value, color);
                    snprintf(value, sizeof(value), "%.2f", metric.maximum);
                    Right(draw, p.x + 530, y, value);
                    snprintf(value, sizeof(value), "%.2f", static_cast<double>(metric.count) / root.count);
                    Right(draw, p.x + 630, y, value, kMuted);
                }
                else
                {
                    Right(draw, p.x + 400, y, "--", kMuted);
                    Right(draw, p.x + 530, y, "--", kMuted);
                    Right(draw, p.x + 630, y, "--", kMuted);
                }
                y += 24;
            }
            return y;
        }
        void ValueLine(ImDrawList* draw, float x, float y, const char* title, Slot slot, bool valid, const char* unit)
        {
            Text(draw, x, y, title, kMuted);
            const auto& metric = g_window.Get(slot);
            char text[80];
            if (valid && metric.count)
                snprintf(text, sizeof(text), "%.1f / %.1f %s", metric.Average(), metric.maximum, unit);
            else snprintf(text, sizeof(text), "--");
            Right(draw, x + 300, y, text);
        }
        void DrawGraph(Features::DebugSettings& settings, bool menuVisible, const ImGuiIO& io)
        {
            unsigned rows = 0;
            for (auto& row : kRows) if (!row.depth || g_expanded[row.branch]) ++rows;
            const float contentHeight = settings.graphAdvanced ? 350 + rows * 24.0f : 300.0f;
            const float height = (std::min)(contentHeight, io.DisplaySize.y - 16);
            const float width = (std::min)(kWidth, io.DisplaySize.x);
            auto clamp = [&](ImVec2 p) {
                return ImVec2(std::clamp(p.x, 0.0f, (std::max)(0.0f, io.DisplaySize.x - width)),
                              std::clamp(p.y, 0.0f, (std::max)(0.0f, io.DisplaySize.y - height)));
            };
            ImVec2 position(settings.graphPositionX, settings.graphPositionY);
            if (position.x < 0 || position.y < 0) position = ImVec2(14, io.DisplaySize.y - height - 14);
            position = clamp(position);
            settings.graphPositionX = position.x;
            settings.graphPositionY = position.y;
            ImGui::SetNextWindowPos(position);
            ImGui::SetNextWindowSize(ImVec2(width, height));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar;
            if (!menuVisible) flags |= ImGuiWindowFlags_NoInputs;
            ImGui::Begin("##performance_graph", nullptr, flags);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            static bool previousAdvanced = settings.graphAdvanced;
            if (previousAdvanced != settings.graphAdvanced) ImGui::SetScrollY(0);
            previousAdvanced = settings.graphAdvanced;
            const int bgAlpha = static_cast<int>(std::clamp(settings.graphBackgroundOpacityPercent, 0.0f, 85.0f) * 2.55f);
            if (bgAlpha > 0)
            {
                draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height),
                                    IM_COL32(10, 17, 26, bgAlpha), 10.0f);
                draw->AddRect(position, ImVec2(position.x + width, position.y + height),
                              IM_COL32(165, 198, 224, bgAlpha / 3), 10.0f);
            }
            const ImVec2 p(position.x + 16, position.y + 12 - ImGui::GetScrollY());
            g_alpha = std::clamp(settings.graphOpacityPercent / 100.0f, 0.35f, 1.0f);
            const bool enabled = Diagnostics::Profile::Enabled();
            if (enabled) Diagnostics::Profile::ReadWindow(g_window);
            else g_window = {};
            const bool valid = enabled && g_window.capturedAt != 0 && GetTickCount64() - g_window.capturedAt < 10000;
            Text(draw, p.x, p.y, "PERFORMANCE", kText, UiKit::Font::Section);
            char status[128];
            snprintf(status, sizeof(status), "%s / %s", !enabled ? "PROFILING OFF" : !valid ? "WAITING" : "LIVE",
                     menuVisible ? "DRAG HEADER" : "INSERT TO INSPECT");
            Right(draw, p.x + 630, p.y + 2, status, enabled ? kMuted : kFrame);
            if (menuVisible)
            {
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y));
                ImGui::InvisibleButton("##drag", ImVec2(630, 26));
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    const auto next = clamp(ImVec2(position.x + io.MouseDelta.x, position.y + io.MouseDelta.y));
                    settings.graphPositionX = next.x;
                    settings.graphPositionY = next.y;
                }
            }
            const bool ko = Features::GetSettings().ui.language == Features::Language::Korean;
            const char* modeNames[] = {ko ? "단순" : "SIMPLE", ko ? "고급" : "ADVANCED"};
            for (unsigned mode = 0; mode < 2; ++mode)
            {
                const ImVec2 button(p.x + mode * 104, p.y + 32);
                const bool selected = settings.graphAdvanced == (mode == 1);
                if (selected)
                    draw->AddRectFilled(button, ImVec2(button.x + 96, button.y + 25), Ink(IM_COL32(60, 110, 145, 90)), 5);
                Text(draw, button.x + 10, button.y + 5, modeNames[mode], selected ? kPresent : kMuted);
                if (menuVisible)
                {
                    ImGui::PushID(static_cast<int>(mode));
                    ImGui::SetCursorScreenPos(button);
                    ImGui::InvisibleButton("##graph_mode", ImVec2(96, 25));
                    if (ImGui::IsItemClicked()) settings.graphAdvanced = mode == 1;
                    ImGui::PopID();
                }
            }
            Plots(draw, ImVec2(p.x, p.y + 72));
            snprintf(status, sizeof(status), "TREND ~12s / 100ms bins     DETAIL %.1fs window / F = Present, T = main tick",
                     g_window.durationMs / 1000.0);
            Text(draw, p.x, p.y + 173, status, kMuted);
            if (!settings.graphAdvanced)
            {
                Rule(draw, p.x, p.y + 197, 630);
                ValueLine(draw, p.x, p.y + 210, "Present CPU", Slot::PresentTotal, valid, "us");
                ValueLine(draw, p.x + 330, p.y + 210, "Main tick CPU", Slot::TickTotal, valid, "us");
                Text(draw, p.x, p.y + 242, ko ? "CPU 평균 / 최대 · 상세 계측은 고급 모드" : "CPU average / maximum. Open Advanced for detailed timings.", kMuted);
                ImGui::SetCursorPos(ImVec2(0, contentHeight - 1));
                ImGui::Dummy(ImVec2(1, 1));
                ImGui::End();
                ImGui::PopStyleVar(2);
                return;
            }
            const float y = Table(draw, ImVec2(p.x, p.y + 197), menuVisible, valid);
            Rule(draw, p.x, y + 2, 630);
            Text(draw, p.x, y + 9, "POSE DELIVERY    average / maximum", kText);
            ValueLine(draw, p.x, y + 31, "Requested", Slot::PoseRequested, valid, "NPC");
            ValueLine(draw, p.x + 330, y + 31, "Update gap", Slot::PoseIntervalMs, valid, "ms");
            ValueLine(draw, p.x, y + 51, "Deferred", Slot::PoseDeferred, valid, "NPC");
            ValueLine(draw, p.x + 330, y + 51, "ESP age", Slot::EspPoseAgeMs, valid, "ms");
            ValueLine(draw, p.x, y + 71, "Processed", Slot::PoseProcessed, valid, "NPC");
            ValueLine(draw, p.x + 330, y + 71, "Aim age", Slot::AimPoseAgeMs, valid, "ms");
            Text(draw, p.x, y + 94, "Inclusive scopes: nested rows are not additive. CPU paths exclude GPU work.", kMuted);
            ImGui::SetCursorPos(ImVec2(0, contentHeight));
            ImGui::Dummy(ImVec2(1, 1));
            ImGui::End();
            ImGui::PopStyleVar(2);
        }
    }
    void Draw(Features::DebugSettings& settings, bool menuVisible)
    {
        const auto& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
        if (settings.showGraph)
        {
            if (!g_wasEnabled) { g_history = {}; g_window = {}; }
            Update(io);
            DrawGraph(settings, menuVisible, io);
        }
        g_wasEnabled = settings.showGraph;
        if (settings.showFps && io.Framerate > 0)
        {
            g_alpha = 1;
            char text[40];
            snprintf(text, sizeof(text), "%.0f FPS", io.Framerate);
            Right(ImGui::GetForegroundDrawList(), io.DisplaySize.x - 14, 14, text);
        }
    }
}
