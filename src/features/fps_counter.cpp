#include "fps_counter.h"
#include "../profiling.h"

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
        constexpr std::size_t kHistorySize = 240;
        constexpr float kGraphWidth = 340.0f;
        constexpr float kGraphHeight = 190.0f;

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

        History g_history;
        bool g_graphWasEnabled = false;

        float Sanitize(float value)
        {
            return std::isfinite(value) && value >= 0.0f ? value : 0.0f;
        }

        void DrawPlot(ImDrawList* drawList, const ImVec2& origin, float width, float height, const char* label,
                      int precision, const std::array<float, kHistorySize>& values, std::size_t count,
                      std::size_t next, ImU32 color)
        {
            const ImVec2 maximum(origin.x + width, origin.y + height);
            drawList->AddRectFilled(origin, maximum, IM_COL32(13, 16, 22, 225), 5.0f);
            drawList->AddRect(origin, maximum, IM_COL32(54, 64, 78, 210), 5.0f, 1.0f, ImDrawFlags_None);

            char valueText[32]{};
            const std::size_t lastIndex = count == 0 ? 0 : (next + kHistorySize - 1) % kHistorySize;
            const float current = count == 0 ? 0.0f : values[lastIndex];
            if (precision == 0)
                snprintf(valueText, sizeof(valueText), "%.0f", current);
            else
                snprintf(valueText, sizeof(valueText), "%.2f", current);

            const ImVec2 labelSize = ImGui::CalcTextSize(label);
            const ImVec2 valueSize = ImGui::CalcTextSize(valueText);
            drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 4.0f), IM_COL32(190, 201, 216, 255), label);
            drawList->AddText(ImVec2(maximum.x - valueSize.x - 8.0f, origin.y + 4.0f), color, valueText);

            const float plotLeft = origin.x + 8.0f;
            const float plotRight = maximum.x - 8.0f;
            const float plotTop = origin.y + labelSize.y + 8.0f;
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
                drawList->AddLine(ImVec2(plotLeft, y), ImVec2(plotRight, y), IM_COL32(60, 70, 84, 100), 1.0f);
            }

            if (count == 0)
                return;

            ImVec2 previous;
            for (std::size_t i = 0; i < count; ++i)
            {
                const float value = values[(next + kHistorySize - count + i) % kHistorySize];
                const float normalized = std::clamp((value - minimum) / range, 0.0f, 1.0f);
                const float x = count == 1 ? plotLeft : plotLeft + plotWidth *
                                                               static_cast<float>(i) /
                                                               static_cast<float>(count - 1);
                const float y = plotBottom - normalized * plotHeight;
                const ImVec2 point(x, y);
                if (i > 0)
                    drawList->AddLine(previous, point, color, 1.7f);
                previous = point;
            }
        }

        void DrawGraph(const ImGuiIO& io)
        {
            const float x = 14.0f;
            const float y = (std::max)(14.0f, io.DisplaySize.y - kGraphHeight - 14.0f);
            const ImVec2 origin(x, y);
            const ImVec2 maximum(x + kGraphWidth, y + kGraphHeight);
            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            drawList->AddRectFilled(origin, maximum, IM_COL32(18, 20, 26, 232), 8.0f);
            drawList->AddRect(origin, maximum, IM_COL32(64, 151, 245, 175), 8.0f, 1.0f, ImDrawFlags_None);

            constexpr float inset = 8.0f;
            constexpr float plotGap = 4.0f;
            constexpr float plotHeight = 53.0f;
            const float plotWidth = kGraphWidth - inset * 2.0f;
            DrawPlot(drawList, ImVec2(x + inset, y + inset), plotWidth, plotHeight, "FPS", 0, g_history.fps,
                     g_history.count, g_history.next, IM_COL32(92, 205, 255, 255));
            DrawPlot(drawList, ImVec2(x + inset, y + inset + plotHeight + plotGap), plotWidth, plotHeight, "ms", 2,
                     g_history.frameTimeMs, g_history.count, g_history.next, IM_COL32(255, 196, 96, 255));
            DrawPlot(drawList, ImVec2(x + inset, y + inset + (plotHeight + plotGap) * 2.0f), plotWidth,
                     plotHeight, "us", 0, g_history.trainerCpuUs, g_history.count, g_history.next,
                     IM_COL32(178, 133, 255, 255));
        }
    }

    void Draw(bool fpsEnabled, bool graphEnabled)
    {
        if (!fpsEnabled && !graphEnabled)
        {
            g_graphWasEnabled = false;
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
            return;

        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        if (graphEnabled)
        {
            if (!g_graphWasEnabled)
                g_history.Clear();
            g_graphWasEnabled = true;

            const float frameTimeMs = Sanitize(io.DeltaTime * 1000.0f);
            const float fps = Sanitize(io.Framerate > 0.0f ? io.Framerate
                                                            : (frameTimeMs > 0.0f ? 1000.0f / frameTimeMs : 0.0f));
            const std::uint64_t cpuMicroseconds = Diagnostics::Profile::LastPresentMicroseconds() +
                                                  Diagnostics::Profile::LastTickTotalMicroseconds();
            g_history.Push(fps, frameTimeMs, static_cast<float>(cpuMicroseconds));
        }
        else
        {
            g_graphWasEnabled = false;
        }

        if (fpsEnabled && io.Framerate > 0.0f)
        {
            char text[32]{};
            snprintf(text, sizeof(text), "%.0f FPS", io.Framerate);
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            constexpr float paddingX = 10.0f;
            constexpr float paddingY = 6.0f;
            const ImVec2 maximum(io.DisplaySize.x - 14.0f, 14.0f + textSize.y + paddingY * 2.0f);
            const ImVec2 minimum(maximum.x - textSize.x - paddingX * 2.0f, 14.0f);
            drawList->AddRectFilled(minimum, maximum, IM_COL32(18, 20, 26, 210), 7.0f);
            drawList->AddRect(minimum, maximum, IM_COL32(64, 151, 245, 170), 7.0f, 1.0f, ImDrawFlags_None);
            drawList->AddText(ImVec2(minimum.x + paddingX, minimum.y + paddingY), IM_COL32(235, 241, 248, 255),
                              text);
        }

        if (graphEnabled)
            DrawGraph(io);
    }
}
