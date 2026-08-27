#pragma once

struct IDXGISwapChain3;
struct ID3D12CommandQueue;

// Present 훅 콜백에서 호출되는 ImGui 오버레이 생명주기. AGENTS.md "아키텍처" 절 2~3단계.
namespace Overlay
{
    // 매 Present 호출마다 실행. 최초 호출 시 지연 초기화(ImGui 컨텍스트, RTV/SRV 힙, WndProc 후킹 등)한다.
    void OnPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* commandQueue);

    // ResizeBuffers 훅에서 호출 — 다음 OnPresent에서 렌더 타겟을 다시 만들도록 상태를 리셋한다.
    void OnResizeBuffers(IDXGISwapChain3* swapChain);

    void Shutdown();

    bool IsVisible();
}
