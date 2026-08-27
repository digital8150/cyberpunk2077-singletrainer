// IDXGISwapChain3::Present / ::ResizeBuffers, ID3D12CommandQueue::ExecuteCommandLists를 MinHook으로 후킹한다.
//
// D3D12 vtable은 COM 인터페이스 구현별로 프로세스 전역에서 공유되므로, 더미 디바이스/스왑체인을 하나
// 만들어서 그 vtable 함수 포인터 주소만 얻고 바로 정리한 뒤, 그 "주소"에 훅을 건다 (실제 게임이 만드는
// 진짜 스왑체인 인스턴스와는 무관하게 이후 모든 Present 호출이 걸린다).
//
// vtable 인덱스는 DXGI/D3D12 공개 헤더의 COM 인터페이스 상속 순서로 고정되어 있다 (IUnknown 3개 +
// 이후 선언 순서). 이 프로젝트에서 새로 알아낸 게 아니라 DX12 오버레이 후킹의 표준 관례값이다.
#include "d3d12_hook.h"
#include "cursor_hook.h"
#include "hook_lifecycle.h"
#include "../framework.h"
#include "../diagnostics.h"
#include "../game/entity_tracker.h"
#include "../game/player_modifiers.h"
#include "../game/visibility.h"
#include "../features/aimbot.h"
#include "../ui/overlay.h"

#include <MinHook.h>

#include <atomic>

namespace
{
    constexpr size_t kPresentIndex = 8;              // IDXGISwapChain::Present
    constexpr size_t kResizeBuffersIndex = 13;        // IDXGISwapChain::ResizeBuffers
    constexpr size_t kExecuteCommandListsIndex = 10;  // ID3D12CommandQueue::ExecuteCommandLists

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

    PresentFn oPresent = nullptr;
    ResizeBuffersFn oResizeBuffers = nullptr;
    ExecuteCommandListsFn oExecuteCommandLists = nullptr;

    // Present 훅 시점엔 어떤 커맨드큐가 "렌더용"인지 알 수 없다 (스왑체인 API엔 큐가 안 딸려 온다).
    // 처음 관측된 Direct 큐는 진단용으로만 남긴다. 실제 제출에는 아래의 스레드별 마지막 큐를 사용한다.
    // 서로 다른 렌더 스레드에서도 안전하게 로그에 표시할 수 있도록 포인터 자체는 atomic으로 게시한다.
    std::atomic<ID3D12CommandQueue*> g_firstDirectQueue{nullptr};
    // DX12 엔진은 커맨드리스트 기록을 여러 스레드에서 해도 실제 큐 제출과 Present는 보통 같은 렌더
    // 스레드에서 연달아 수행한다. 그 스레드에서 마지막으로 본 Direct 큐만 Present에 넘겨, 무관한
    // Direct 큐(업로드/보조 렌더 등)를 "첫 큐"라는 이유만으로 쓰지 않는다.
    thread_local ID3D12CommandQueue* t_lastDirectQueue = nullptr;
    std::atomic_bool g_loggedFirstPresent{false};
    bool g_minHookInitialized = false;
    bool g_hooksEnabled = false;

    HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags)
    {
        HookLifecycle::CallbackGuard callback;
        if (HookLifecycle::IsShuttingDown())
            return oPresent(swapChain, syncInterval, flags);

        ID3D12CommandQueue* commandQueue = t_lastDirectQueue;
        if (!g_loggedFirstPresent.exchange(true))
            Diagnostics::Log("first Present intercepted: swapChain=%p sameThreadDirectQueue=%p firstDirectQueue=%p",
                             swapChain, commandQueue, g_firstDirectQueue.load(std::memory_order_acquire));

        Overlay::OnPresent(swapChain, commandQueue);
        return oPresent(swapChain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain3* swapChain, UINT bufferCount, UINT width,
                                               UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags)
    {
        HookLifecycle::CallbackGuard callback;
        if (HookLifecycle::IsShuttingDown())
            return oResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);

        Overlay::OnResizeBuffers(swapChain);
        return oResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
    }

    void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCommandLists,
                                                  ID3D12CommandList* const* commandLists)
    {
        HookLifecycle::CallbackGuard callback;
        if (HookLifecycle::IsShuttingDown())
        {
            oExecuteCommandLists(queue, numCommandLists, commandLists);
            return;
        }

        // 엔진은 보통 큐를 여러 개(Direct/Compute/Copy) 굴린다. RTV를 건드리는 ImGui 커맨드리스트는
        // Direct 큐로만 제출할 수 있으므로 그게 아니면 절대 캡처하면 안 된다 — 예전엔 이 필터가 없어서
        // "그냥 처음 관측된 큐"를 렌더 큐로 오인해 캡처했고, 그게 Compute/Copy 큐였을 경우 그 큐로
        // ImGui 커맨드리스트를 ExecuteCommandLists 하는 게 되어(불법 제출) GPU가 그대로 멈춰버렸다
        // (실제로 재현됨 — 펜스 대기가 영원히 안 풀리는 형태의 행으로 나타났다).
        const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            t_lastDirectQueue = queue;
            ID3D12CommandQueue* expected = nullptr;
            if (g_firstDirectQueue.compare_exchange_strong(expected, queue, std::memory_order_release,
                                                           std::memory_order_relaxed))
            {
                Diagnostics::Log("captured Direct queue candidate: queue=%p priority=%d flags=0x%X nodeMask=0x%X",
                                 queue, desc.Priority, static_cast<unsigned>(desc.Flags), desc.NodeMask);
            }
        }
        oExecuteCommandLists(queue, numCommandLists, commandLists);
    }

    // 더미 창 + 디바이스 + 스왑체인을 만들어 필요한 vtable 함수 포인터 3개만 뽑아내고 바로 정리한다.
    bool GetVTableAddresses(void** presentAddr, void** resizeBuffersAddr, void** executeCommandListsAddr)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"cp2077_trainer_dummy";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            Diagnostics::Log("RegisterClassExW(dummy) failed: error=%lu", GetLastError());
            return false;
        }

        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"dummy", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                                     nullptr, wc.hInstance, nullptr);
        if (!hwnd)
        {
            Diagnostics::Log("CreateWindowExW(dummy) failed: error=%lu", GetLastError());
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            return false;
        }

        bool ok = false;

        do
        {
            ComPtr<ID3D12Device> device;
            HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            if (FAILED(hr))
            {
                Diagnostics::LogHr("D3D12CreateDevice(dummy)", hr);
                break;
            }

            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> queue;
            hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
            if (FAILED(hr))
            {
                Diagnostics::LogHr("CreateCommandQueue(dummy)", hr);
                break;
            }

            ComPtr<IDXGIFactory4> factory;
            hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if (FAILED(hr))
            {
                Diagnostics::LogHr("CreateDXGIFactory1(dummy)", hr);
                break;
            }

            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            scDesc.BufferCount = 2;
            scDesc.Width = 64;
            scDesc.Height = 64;
            scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scDesc.SampleDesc.Count = 1;
            scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

            ComPtr<IDXGISwapChain1> swapChain1;
            hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1);
            if (FAILED(hr))
            {
                Diagnostics::LogHr("CreateSwapChainForHwnd(dummy)", hr);
                break;
            }

            ComPtr<IDXGISwapChain3> swapChain3;
            hr = swapChain1.As(&swapChain3);
            if (FAILED(hr))
            {
                Diagnostics::LogHr("QueryInterface(IDXGISwapChain3 dummy)", hr);
                break;
            }

            void** swapChainVTable = *reinterpret_cast<void***>(swapChain3.Get());
            void** queueVTable = *reinterpret_cast<void***>(queue.Get());

            *presentAddr = swapChainVTable[kPresentIndex];
            *resizeBuffersAddr = swapChainVTable[kResizeBuffersIndex];
            *executeCommandListsAddr = queueVTable[kExecuteCommandListsIndex];
            Diagnostics::Log("resolved hook targets: Present=%p ResizeBuffers=%p ExecuteCommandLists=%p",
                             *presentAddr, *resizeBuffersAddr, *executeCommandListsAddr);
            ok = true;
        } while (false);

        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return ok;
    }
}

namespace Hooks
{
    void Initialize()
    {
        Diagnostics::Log("hook initialization started");

        MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        {
            Diagnostics::Log("MH_Initialize failed: %s (%d)", MH_StatusToString(status), status);
            return;
        }
        g_minHookInitialized = true;

        void* presentAddr = nullptr;
        void* resizeBuffersAddr = nullptr;
        void* executeCommandListsAddr = nullptr;
        if (!GetVTableAddresses(&presentAddr, &resizeBuffersAddr, &executeCommandListsAddr))
        {
            Diagnostics::Log("failed to resolve hook targets");
            return;
        }

        status = MH_CreateHook(presentAddr, &hkPresent, reinterpret_cast<void**>(&oPresent));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(Present) failed: %s (%d)", MH_StatusToString(status), status);
            return;
        }

        status = MH_CreateHook(resizeBuffersAddr, &hkResizeBuffers, reinterpret_cast<void**>(&oResizeBuffers));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(ResizeBuffers) failed: %s (%d)", MH_StatusToString(status), status);
            return;
        }

        status = MH_CreateHook(executeCommandListsAddr, &hkExecuteCommandLists,
                               reinterpret_cast<void**>(&oExecuteCommandLists));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(ExecuteCommandLists) failed: %s (%d)", MH_StatusToString(status), status);
            return;
        }

        // 게임 주소 훅은 선택 사항이다. 실패해도 렌더 오버레이는 계속 사용할 수 있다.
        Game::EntityTracker::CreateHook();
        // Visibility is optional: its synchronous physics work is drained from the game-main-tick hook.
        Game::Visibility::CreateHook();
        CursorHook::CreateHooks();

        status = MH_EnableHook(MH_ALL_HOOKS);
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_EnableHook failed: %s (%d)", MH_StatusToString(status), status);
            return;
        }
        g_hooksEnabled = true;

        Diagnostics::Log("all hooks enabled");
    }

    bool Shutdown()
    {
        Diagnostics::Log("hook shutdown started");

        // Native highlight events and StatsSystem modifiers are owned by the game main tick. Request their cleanup
        // while the tick hook is still active; invoking engine code from this unload worker would race the engine.
        if (!Game::EntityTracker::PrepareForShutdown(2500) ||
            !Game::PlayerModifiers::PrepareForShutdown(2500))
        {
            Diagnostics::Log("hook shutdown aborted: main-tick feature cleanup did not acknowledge safely");
            return false;
        }
        HookLifecycle::BeginShutdown();

        if (g_hooksEnabled)
        {
            const MH_STATUS disableStatus = MH_DisableHook(MH_ALL_HOOKS);
            if (disableStatus != MH_OK)
            {
                Diagnostics::Log("MH_DisableHook failed: %s (%d)", MH_StatusToString(disableStatus), disableStatus);
                return false;
            }
            g_hooksEnabled = false;
        }

        if (!HookLifecycle::WaitForCallbacks(5000))
        {
            Diagnostics::Log("hook shutdown aborted: detour callbacks did not drain within 5000 ms");
            return false;
        }

        // All hooks are disabled and drained before clearing main-tick visibility state.
        if (!Game::Visibility::Shutdown())
        {
            Diagnostics::Log("hook shutdown aborted: visibility callbacks did not drain");
            return false;
        }

        // Present/Resize callbacks can mutate overlay state, so drain them before restoring the separately hooked
        // WndProc. A second drain closes the small window in which a WndProc callback could already be entering.
        if (!Overlay::BeginShutdown())
            return false;
        if (!HookLifecycle::WaitForCallbacks(5000))
        {
            Diagnostics::Log("hook shutdown aborted: WndProc callbacks did not drain within 5000 ms");
            return false;
        }

        Overlay::Shutdown();
        CursorHook::Shutdown();
        Aimbot::Shutdown();
        Game::PlayerModifiers::Shutdown();
        Game::EntityTracker::Shutdown();
        if (g_minHookInitialized)
        {
            const MH_STATUS uninitializeStatus = MH_Uninitialize();
            if (uninitializeStatus != MH_OK)
            {
                Diagnostics::Log("MH_Uninitialize failed: %s (%d)", MH_StatusToString(uninitializeStatus),
                                 uninitializeStatus);
                return false;
            }
            g_minHookInitialized = false;
        }
        g_firstDirectQueue.store(nullptr, std::memory_order_release);
        Diagnostics::Log("hook shutdown finished");
        return true;
    }
}
