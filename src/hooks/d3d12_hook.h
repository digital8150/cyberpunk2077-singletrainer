#pragma once

// D3D12 스왑체인/커맨드큐 훅 설치·해제. AGENTS.md "아키텍처" 절의 1~2단계를 구현한다.
namespace Hooks
{
    void Initialize();
    // Returns true only when every detour callback has drained and the DLL can safely be unloaded.
    bool Shutdown();
}
