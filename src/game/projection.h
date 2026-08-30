#pragma once

namespace Game::Projection
{
    struct ScreenPoint
    {
        float x = 0.0f;
        float y = 0.0f;
        float depth = 0.0f;
        bool visible = false;
        bool behind = true;
    };

    // Uses REDengine's active camera projection routine. Coordinates are ImGui display pixels.
    bool WorldToScreen(const float world[3], float displayWidth, float displayHeight, ScreenPoint& result);

    // Active camera world position. Callers should fetch it once per frame and pass it down; the result is
    // validated by projecting it back (a point at the camera has ~zero forward depth).
    bool GetCameraPosition(float world[3]);

    // 현재 카메라 초점거리에서의 "탄젠트 1당 화면 픽셀 수". 카메라 축에서 각도 θ만큼 벗어난 방향은
    // 화면에서 tan(θ) * 이 값 만큼 떨어진 곳에 찍힌다.
    //
    // 월드→클립 변환은 아핀이므로, 기준점 하나와 세 축 방향으로 한 걸음씩 투영해 보면 clip.x와 clip.w의
    // 행벡터를 그대로 복원할 수 있다. 표준 원근 행렬에서 그 두 행은 각각 cot(fovX/2)·right와 forward이고
    // right/forward는 단위 벡터라, 두 행의 길이 비가 곧 cot(fovX/2)다. ADS나 줌으로 FOV가 바뀌면 이
    // 값도 같은 프레임에 따라 바뀐다 — 그래서 에임봇 FOV를 픽셀이 아니라 각도로 둘 수 있다.
    //
    // 프레임당 ProjectPoint 4회. 결과는 호출한 프레임 안에서만 캐시된다.
    bool GetPixelsPerTangent(float displayWidth, float displayHeight, float& pixelsPerTangent);
}
