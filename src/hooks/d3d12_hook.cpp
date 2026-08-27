// IDXGISwapChain3::Present / ::ResizeBuffers, ID3D12CommandQueue::ExecuteCommandLists를 MinHook으로 후킹한다.
//
// D3D12 vtable은 COM 인터페이스 구현별로 프로세스 전역에서 공유되므로, 더미 디바이스/스왑체인을 하나
// 만들어서 그 vtable 함수 포인터 주소만 얻고 바로 정리한 뒤, 그 "주소"에 훅을 건다 (실제 게임이 만드는
// 진짜 스왑체인 인스턴스와는 무관하게 이후 모든 Present 호출이 걸린다).
//
// vtable 인덱스는 DXGI/D3D12 공개 헤더의 COM 인터페이스 상속 순서로 고정되어 있다 (IUnknown 3개 +
// 이후 선언 순서). 이 프로젝트에서 새로 알아낸 게 아니라 DX12 오버레이 후킹의 표준 관례값이다.
#include "d3d12_hook.h"
#include "../framework.h"
#include "../ui/overlay.h"

#include <MinHook.h>

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
    // 그래서 ExecuteCommandLists 훅에서 처음 관측되는 큐를 렌더 큐로 가정해 캡처해 둔다.
    // TODO: D3D12_COMMAND_QUEUE_DESC.Type == D3D12_COMMAND_LIST_TYPE_DIRECT 확인해서 더 견고하게 만들 것.
    ID3D12CommandQueue* g_commandQueue = nullptr;

    HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain3* swapChain, UINT syncInterval, UINT flags)
    {
        Overlay::OnPresent(swapChain, g_commandQueue);
        return oPresent(swapChain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain3* swapChain, UINT bufferCount, UINT width,
                                               UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags)
    {
        Overlay::OnResizeBuffers();
        return oResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
    }

    void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCommandLists,
                                                  ID3D12CommandList* const* commandLists)
    {
        if (!g_commandQueue)
            g_commandQueue = queue;
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
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"dummy", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                                     nullptr, wc.hInstance, nullptr);

        bool ok = false;

        do
        {
            ComPtr<ID3D12Device> device;
            if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
                break;

            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> queue;
            if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
                break;

            ComPtr<IDXGIFactory4> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                break;

            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            scDesc.BufferCount = 2;
            scDesc.Width = 64;
            scDesc.Height = 64;
            scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scDesc.SampleDesc.Count = 1;
            scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

            ComPtr<IDXGISwapChain1> swapChain1;
            if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1)))
                break;

            ComPtr<IDXGISwapChain3> swapChain3;
            if (FAILED(swapChain1.As(&swapChain3)))
                break;

            void** swapChainVTable = *reinterpret_cast<void***>(swapChain3.Get());
            void** queueVTable = *reinterpret_cast<void***>(queue.Get());

            *presentAddr = swapChainVTable[kPresentIndex];
            *resizeBuffersAddr = swapChainVTable[kResizeBuffersIndex];
            *executeCommandListsAddr = queueVTable[kExecuteCommandListsIndex];
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
        if (MH_Initialize() != MH_OK)
            return;

        void* presentAddr = nullptr;
        void* resizeBuffersAddr = nullptr;
        void* executeCommandListsAddr = nullptr;
        if (!GetVTableAddresses(&presentAddr, &resizeBuffersAddr, &executeCommandListsAddr))
            return;

        MH_CreateHook(presentAddr, &hkPresent, reinterpret_cast<void**>(&oPresent));
        MH_CreateHook(resizeBuffersAddr, &hkResizeBuffers, reinterpret_cast<void**>(&oResizeBuffers));
        MH_CreateHook(executeCommandListsAddr, &hkExecuteCommandLists,
                      reinterpret_cast<void**>(&oExecuteCommandLists));

        MH_EnableHook(MH_ALL_HOOKS);
    }

    void Shutdown()
    {
        Overlay::Shutdown();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
}
