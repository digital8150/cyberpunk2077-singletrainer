#include "fps_counter.h"

#include "features.h"
#include "../profiling.h"
#include "../ui/theme.h"
#include "../ui/ui_kit.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace FpsCounter
{
    namespace
    {
        constexpr std::size_t kHistorySize = 120;
        constexpr float kGraphWidth = 356.0f;
        constexpr float kGraphHeight = 220.0f;
        constexpr float kGraphMargin = 14.0f;
        constexpr float kHeaderHeight = 34.0f;
        constexpr float kSampleIntervalSeconds = 0.10f;
        constexpr float kSmoothingTimeSeconds = 0.28f;
        constexpr int kFpsBadgeAlpha = 224;

        struct History
        {
            std::array<float, kHistorySize> fps{};
            std::array<float, kHistorySize> frameTimeMs{};
            std::array<float, kHistorySize> trainerCpuUs{};
            std::size_t next = 0;
            std::size_t count = 0;

            void Clear()
            {
                next = 0;
                count = 0;
            }

            void Push(float fpsValue, float frameTimeValue, float cpuValue)
            {
                fps[next] = fpsValue;
                frameTimeMs[next] = frameTimeValue;
                trainerCpuUs[next] = cpuValue;
                next = (next + 1) % kHistorySize;
                count = (std::min)(count + 1, kHistorySize);
            }
        };

        struct SmoothedSample
        {
            float fps = 0.0f;
            float frameTimeMs = 0.0f;
            float trainerCpuUs = 0.0f;
            float sampleAccumulator = 0.0f;
            bool initialized = false;

            void Clear() { *this = {}; }
        };

        History g_history;
        SmoothedSample g_smoothed;
        bool g_graphWasEnabled = false;

        float Sanitize(float value)
        {
            return std::isfinite(value) && value >= 0.0f ? value : 0.0f;
        }

        ImVec2 ClampGraphPosition(ImVec2 position, const ImGuiIO& io)
        {
            const float maximumX = (std::max)(0.0f, io.DisplaySize.x - kGraphWidth);
            const float maximumY = (std::max)(0.0f, io.DisplaySize.y - kGraphHeight);
            position.x = std::clamp(position.x, 0.0f, maximumX);
            position.y = std::clamp(position.y, 0.0f, maximumY);
            return position;
        }

        void UpdateHistory(const ImGuiIO& io)
        {
            const float delta = std::clamp(Sanitize(io.DeltaTime), 0.0f, 0.25f);
            const float rawFrameTime = Sanitize(io.DeltaTime * 1000.0f);
            const float rawFps = Sanitize(io.Framerate > 0.0f
                                              ? io.Framerate
                                              : (rawFrameTime > 0.0f ? 1000.0f / rawFrameTime : 0.0f));
            const float rawCpu = Sanitize(static_cast<float>(Diagnostics::Profile::LastPresentMicroseconds() +
                                                              Diagnostics::Profile::LastTickTotalMicroseconds()));

            if (!g_smoothed.initialized)
            {
                g_smoothed.fps = rawFps;
                g_smoothed.frameTimeMs = rawFrameTime;
                g_smoothed.trainerCpuUs = rawCpu;
                g_smoothed.sampleAccumulator = kSampleIntervalSeconds;
                g_smoothed.initialized = true;
            }
            else
            {
                // 프레임률에 독립적인 EMA. 10Hz로만 히스토리에 넣어 고FPS에서도 약 12초의 추세가 보인다.
                const float alpha = 1.0f - std::exp(-delta / kSmoothingTimeSeconds);
                g_smoothed.fps += (rawFps - g_smoothed.fps) * alpha;
                g_smoothed.frameTimeMs += (rawFrameTime - g_smoothed.frameTimeMs) * alpha;
                g_smoothed.trainerCpuUs += (rawCpu - g_smoothed.trainerCpuUs) * alpha;
                g_smoothed.sampleAccumulator += delta;
            }

            if (g_smoothed.sampleAccumulator >= kSampleIntervalSeconds)
            {
                g_smoothed.sampleAccumulator = std::fmod(g_smoothed.sampleAccumulator, kSampleIntervalSeconds);
                g_history.Push(g_smoothed.fps, g_smoothed.frameTimeMs, g_smoothed.trainerCpuUs);
            }
        }

        void DrawPlot(ImDrawList* drawList, const ImVec2& origin, float width, float height, int plotAlpha,
                      int borderAlpha, const char* label,
                      const char* format, const std::array<float, kHistorySize>& values, std::size_t count,
                      std::size_t next, ImU32 color)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const ImVec2 maximum(origin.x + width, origin.y + height);
            drawList->AddRectFilled(origin, maximum, UiTheme::WithAlpha(palette.background, plotAlpha), 5.0f);
            drawList->AddRect(origin, maximum, UiTheme::WithAlpha(palette.borderSubtle, borderAlpha), 5.0f,
                              1.0f, ImDrawFlags_None);

            char valueText[40]{};
            const std::size_t lastIndex = count == 0 ? 0 : (next + kHistorySize - 1) % kHistorySize;
            snprintf(valueText, sizeof(valueText), format, count == 0 ? 0.0f : values[lastIndex]);

            UiKit::PaintText(drawList, UiKit::Font::Micro, ImVec2(origin.x + 8.0f, origin.y + 5.0f),
                             palette.textSecondary, label);
            const ImVec2 valueSize = UiKit::MeasureText(UiKit::Font::Mono, valueText);
            UiKit::PaintText(drawList, UiKit::Font::Mono,
                             ImVec2(maximum.x - valueSize.x - 8.0f, origin.y + 4.0f), color, valueText);

            const float plotLeft = origin.x + 8.0f;
            const float plotRight = maximum.x - 8.0f;
            const float plotTop = origin.y + 22.0f;
            const float plotBottom = maximum.y - 7.0f;
            const float plotHeight = (std::max)(1.0f, plotBottom - plotTop);
            const float plotWidth = (std::max)(1.0f, plotRight - plotLeft);

            float minimum = 0.0f;
            float maximumValue = 0.0f;
            if (count > 0)
            {
                minimum = values[(next + kHistorySize - count) % kHistorySize];
                maximumValue = minimum;
                for (std::size_t i = 1; i < count; ++i)
                {
                    const float value = values[(next + kHistorySize - count + i) % kHistorySize];
                    minimum = (std::min)(minimum, value);
                    maximumValue = (std::max)(maximumValue, value);
                }
            }
            if (maximumValue - minimum < 0.001f)
            {
                const float padding = (std::max)(1.0f, maximumValue * 0.10f);
                minimum = (std::max)(0.0f, minimum - padding);
                maximumValue += padding;
            }
            else
            {
                const float padding = (maximumValue - minimum) * 0.10f;
                minimum = (std::max)(0.0f, minimum - padding);
                maximumValue += padding;
            }

            const float range = (std::max)(0.001f, maximumValue - minimum);
            for (unsigned row = 1; row < 3; ++row)
            {
                const float y = plotTop + plotHeight * static_cast<float>(row) * 0.3333333f;
                drawList->AddLine(ImVec2(plotLeft, y), ImVec2(plotRight, y),
                                  UiTheme::WithAlpha(palette.borderSubtle, 150), 1.0f);
            }
            if (count == 0)
                return;

            ImVec2 previous;
            for (std::size_t i = 0; i < count; ++i)
            {
                const float value = values[(next + kHistorySize - count + i) % kHistorySize];
                const float normalized = std::clamp((value - minimum) / range, 0.0f, 1.0f);
                const float x = count == 1
                                    ? plotLeft
                                    : plotLeft + plotWidth * static_cast<float>(i) /
                                                     static_cast<float>(count - 1);
                const ImVec2 point(x, plotBottom - normalized * plotHeight);
                if (i > 0)
                    drawList->AddLine(previous, point, color, 1.5f);
                previous = point;
            }
        }

        void DrawGraph(Features::DebugSettings& settings, bool menuVisible, const ImGuiIO& io)
        {
            ImVec2 position(settings.graphPositionX, settings.graphPositionY);
            if (position.x < 0.0f || position.y < 0.0f)
                position = ImVec2(kGraphMargin,
                                  (std::max)(kGraphMargin, io.DisplaySize.y - kGraphHeight - kGraphMargin));
            position = ClampGraphPosition(position, io);
            settings.graphPositionX = position.x;
            settings.graphPositionY = position.y;

            ImGui::SetNextWindowPos(position, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(kGraphWidth, kGraphHeight), ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground;
            if (!menuVisible)
                flags |= ImGuiWindowFlags_NoInputs;
            if (!ImGui::Begin("##performance_graph", nullptr, flags))
            {
                ImGui::End();
                ImGui::PopStyleVar(2);
                return;
            }

            const UiTheme::Palette& palette = UiTheme::Current();
            const int panelAlpha = static_cast<int>(
                std::clamp(settings.graphOpacityPercent, 35.0f, 100.0f) * 2.55f + 0.5f);
            const int plotAlpha = (std::max)(0, panelAlpha - 14);
            const int borderAlpha = (std::max)(0, panelAlpha - 19);
            const ImVec2 origin = ImGui::GetWindowPos();
            const ImVec2 maximum(origin.x + kGraphWidth, origin.y + kGraphHeight);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(ImVec2(origin.x + 3.0f, origin.y + 5.0f),
                                    ImVec2(maximum.x + 3.0f, maximum.y + 5.0f), palette.shadow, 9.0f);
            drawList->AddRectFilled(origin, maximum, UiTheme::WithAlpha(palette.surface, panelAlpha), 8.0f);
            drawList->AddRect(origin, maximum, UiTheme::WithAlpha(palette.border, borderAlpha), 8.0f, 1.0f,
                              ImDrawFlags_None);
            drawList->AddRectFilled(ImVec2(origin.x, origin.y + 9.0f),
                                    ImVec2(origin.x + 3.0f, origin.y + kHeaderHeight - 9.0f), palette.accent, 1.5f);
            drawList->AddRectFilled(ImVec2(origin.x, origin.y + kHeaderHeight - 1.0f),
                                    ImVec2(maximum.x, origin.y + kHeaderHeight),
                                    UiTheme::WithAlpha(palette.borderSubtle, borderAlpha));
            UiKit::PaintText(drawList, UiKit::Font::Section, ImVec2(origin.x + 14.0f, origin.y + 9.0f),
                             palette.text, "Performance");
            UiKit::PaintText(drawList, UiKit::Font::Micro, ImVec2(maximum.x - 58.0f, origin.y + 11.0f),
                             menuVisible ? palette.accent : palette.textDisabled,
                             menuVisible ? "DRAG" : "LIVE");

            if (menuVisible)
            {
                ImGui::SetCursorScreenPos(origin);
                ImGui::InvisibleButton("##drag_header", ImVec2(kGraphWidth, kHeaderHeight));
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    position.x += io.MouseDelta.x;
                    position.y += io.MouseDelta.y;
                    position = ClampGraphPosition(position, io);
                    settings.graphPositionX = position.x;
                    settings.graphPositionY = position.y;
                }
            }

            constexpr float inset = 8.0f;
            constexpr float plotGap = 4.0f;
            constexpr float plotHeight = 55.0f;
            const float plotWidth = kGraphWidth - inset * 2.0f;
            const float plotY = origin.y + kHeaderHeight + 6.0f;
            DrawPlot(drawList, ImVec2(origin.x + inset, plotY), plotWidth, plotHeight, plotAlpha, borderAlpha,
                     "FPS", "%.0f fps",
                     g_history.fps, g_history.count, g_history.next, palette.accent);
            DrawPlot(drawList, ImVec2(origin.x + inset, plotY + plotHeight + plotGap), plotWidth, plotHeight,
                     plotAlpha, borderAlpha, "FRAME TIME", "%.2f ms", g_history.frameTimeMs, g_history.count,
                     g_history.next,
                     palette.warning);
            DrawPlot(drawList, ImVec2(origin.x + inset, plotY + (plotHeight + plotGap) * 2.0f), plotWidth,
                     plotHeight, plotAlpha, borderAlpha, "TRAINER CPU", "%.0f us", g_history.trainerCpuUs,
                     g_history.count, g_history.next, palette.success);

            ImGui::End();
            ImGui::PopStyleVar(2);
        }
    }

    void Draw(Features::DebugSettings& settings, bool menuVisible)
    {
        if (!settings.showFps && !settings.showGraph)
        {
            g_graphWasEnabled = false;
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
            return;

        if (settings.showGraph)
        {
            if (!g_graphWasEnabled)
            {
                g_history.Clear();
                g_smoothed.Clear();
            }
            g_graphWasEnabled = true;
            UpdateHistory(io);
        }
        else
        {
            g_graphWasEnabled = false;
        }

        if (settings.showFps && io.Framerate > 0.0f)
        {
            char text[32]{};
            snprintf(text, sizeof(text), "%.0f FPS", io.Framerate);
            const UiTheme::Palette& palette = UiTheme::Current();
            const ImVec2 textSize = UiKit::MeasureText(UiKit::Font::Mono, text);
            constexpr float paddingX = 9.0f;
            constexpr float paddingY = 5.0f;
            const ImVec2 maximum(io.DisplaySize.x - kGraphMargin,
                                 kGraphMargin + textSize.y + paddingY * 2.0f);
            const ImVec2 minimum(maximum.x - textSize.x - paddingX * 2.0f, kGraphMargin);
            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            drawList->AddRectFilled(minimum, maximum, UiTheme::WithAlpha(palette.surface, kFpsBadgeAlpha), 6.0f);
            drawList->AddRect(minimum, maximum, UiTheme::WithAlpha(palette.border, kFpsBadgeAlpha - 19), 6.0f, 1.0f,
                              ImDrawFlags_None);
            UiKit::PaintText(drawList, UiKit::Font::Mono,
                             ImVec2(minimum.x + paddingX, minimum.y + paddingY), palette.text, text);
        }

        if (settings.showGraph)
            DrawGraph(settings, menuVisible, io);
    }
}
