#pragma once

namespace Features
{
    struct EspSettings;
    struct FrameSnapshots;
}

namespace Esp
{
    // 스냅샷 패스는 호출자(Features::DrawOverlay)가 프레임당 한 번만 돌리고 그 결과를 넘긴다.
    void DrawOverlay(const Features::EspSettings& settings, const Features::FrameSnapshots& frame);
}
