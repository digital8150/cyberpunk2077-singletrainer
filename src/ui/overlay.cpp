// ImGui를 DX12 Present 훅 안에 얹는 표준 패턴("hijack"). 스왑체인의 백버퍼 개수만큼 RTV를,
// 폰트 텍스처용 SRV 힙 1개를 만들고, ImGui 드로우 데이터를 별도 커맨드 리스트에 기록해
// 캡처해 둔 렌더 커맨드큐로 ExecuteCommandLists 한다.
//
// 리사이즈(OnResizeBuffers)는 지금은 상태를 초기화되지 않은 상태로 되돌리기만 한다 — 다음 프레임에
// OnPresent가 다시 지연 초기화를 수행한다. 좀 더 매끄러운 처리(기존 ImGui 컨텍스트 유지한 채 RTV만
// 재생성)는 실제 게임에서 리사이즈를 테스트해 본 뒤 다듬을 것.
#include "overlay.h"
#include "widgets.h"
#include "../framework.h"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    struct FrameContext
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12Resource> backBuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
    };

    // 최신 ImGui DX12 백엔드(1.92+)는 SRV 디스크립터를 필요할 때마다(폰트 외 텍스처가 생길 때) 동적으로
    // 요청한다 — 그래서 힙을 넉넉히 잡고 간단한 슬롯 할당기(alloc/free 콜백)를 붙여줘야 한다.
    constexpr UINT kSrvHeapCapacity = 64;

    bool g_initialized = false;
    bool g_visible = true;  // Insert 키로 토글 (AGENTS.md 기술 스택 절)
    HWND g_hwnd = nullptr;
    WNDPROC g_originalWndProc = nullptr;

    ComPtr<ID3D12Device> g_device;
    ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> g_srvHeap;
    ComPtr<ID3D12GraphicsCommandList> g_commandList;
    std::vector<FrameContext> g_frameContexts;
    UINT g_bufferCount = 0;
    UINT g_rtvDescriptorSize = 0;
    UINT g_srvDescriptorSize = 0;
    bool g_srvSlotUsed[kSrvHeapCapacity] = {};

    void SrvDescriptorAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                             D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
    {
        for (UINT i = 0; i < kSrvHeapCapacity; ++i)
        {
            if (g_srvSlotUsed[i])
                continue;
            g_srvSlotUsed[i] = true;
            *outCpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            outCpu->ptr += static_cast<SIZE_T>(i) * g_srvDescriptorSize;
            *outGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            outGpu->ptr += static_cast<UINT64>(i) * g_srvDescriptorSize;
            return;
        }
        // 슬롯 소진: 스캐폴딩 단계라 조용히 슬롯 0을 재사용한다 (TODO: kSrvHeapCapacity를 늘리거나 방어 강화).
        *outCpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        *outGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    }

    void SrvDescriptorFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE base = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        const UINT index = static_cast<UINT>((cpu.ptr - base.ptr) / g_srvDescriptorSize);
        if (index < kSrvHeapCapacity)
            g_srvSlotUsed[index] = false;
    }

    LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_KEYDOWN && wParam == VK_INSERT)
            g_visible = !g_visible;

        // 메뉴가 떠 있을 때만 ImGui가 입력을 가로챈다 — 숨겨져 있으면 게임이 입력을 그대로 받는다.
        if (g_visible)
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
    }

    void CreateRenderTargets(IDXGISwapChain3* swapChain)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < g_bufferCount; ++i)
        {
            ComPtr<ID3D12Resource> backBuffer;
            swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
            g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

            g_frameContexts[i].backBuffer = backBuffer;
            g_frameContexts[i].rtvHandle = rtvHandle;
            rtvHandle.ptr += g_rtvDescriptorSize;
        }
    }

    bool InitializeForSwapChain(IDXGISwapChain3* swapChain, ID3D12CommandQueue* commandQueue)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDesc(&desc)))
            return false;
        g_hwnd = desc.OutputWindow;
        g_bufferCount = desc.BufferCount;

        if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = g_bufferCount;
        if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap))))
            return false;
        g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = kSrvHeapCapacity;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap))))
            return false;
        g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (bool& used : g_srvSlotUsed)
            used = false;

        g_frameContexts.resize(g_bufferCount);
        for (auto& frame : g_frameContexts)
        {
            if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&frame.commandAllocator))))
                return false;
        }

        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                g_frameContexts[0].commandAllocator.Get(), nullptr,
                                                IID_PPV_ARGS(&g_commandList))))
            return false;
        g_commandList->Close();

        CreateRenderTargets(swapChain);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        Widgets::ApplyStyle();

        if (!ImGui_ImplWin32_Init(g_hwnd))
            return false;

        ImGui_ImplDX12_InitInfo initInfo{};
        initInfo.Device = g_device.Get();
        initInfo.CommandQueue = commandQueue;  // 텍스처 업로드용 (신규 백엔드가 요구)
        initInfo.NumFramesInFlight = static_cast<int>(g_bufferCount);
        initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        initInfo.SrvDescriptorHeap = g_srvHeap.Get();
        initInfo.SrvDescriptorAllocFn = SrvDescriptorAlloc;
        initInfo.SrvDescriptorFreeFn = SrvDescriptorFree;
        if (!ImGui_ImplDX12_Init(&initInfo))
            return false;

        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));

        return true;
    }
}

namespace Overlay
{
    void OnPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* commandQueue)
    {
        // 렌더 큐를 아직 못 잡았으면(ExecuteCommandLists가 이번 세션에서 아직 한 번도 안 불렸으면) 초기화도
        // 포함해서 이번 프레임은 통째로 건너뛴다 — 새 ImGui DX12 백엔드는 Init 시점에 CommandQueue가 필요함.
        if (!swapChain || !commandQueue)
            return;

        if (!g_initialized)
        {
            g_initialized = InitializeForSwapChain(swapChain, commandQueue);
            if (!g_initialized)
                return;
        }

        if (!g_visible)
            return;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        Widgets::DrawMainMenu();

        ImGui::Render();

        const UINT backBufferIndex = swapChain->GetCurrentBackBufferIndex();
        FrameContext& frame = g_frameContexts[backBufferIndex];

        frame.commandAllocator->Reset();
        g_commandList->Reset(frame.commandAllocator.Get(), nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.backBuffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &barrier);

        g_commandList->OMSetRenderTargets(1, &frame.rtvHandle, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
        g_commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList.Get());

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &barrier);

        g_commandList->Close();
        ID3D12CommandList* lists[] = {g_commandList.Get()};
        // 주의: commandQueue의 vtable도 훅되어 있으므로 이 호출은 hkExecuteCommandLists를 한 번 더
        // 거쳐간다 (g_commandQueue가 이미 캡처돼 있어 별다른 부작용 없이 원본으로 패스스루된다).
        commandQueue->ExecuteCommandLists(1, lists);
    }

    void OnResizeBuffers()
    {
        // TODO: ImGui_ImplDX12_InvalidateDeviceObjects 등으로 좀 더 매끄럽게 처리 가능. 지금은 안전하게
        // 전체 재초기화 경로(다음 OnPresent에서 InitializeForSwapChain 재실행)를 탄다.
        if (g_initialized)
            Shutdown();
        g_frameContexts.clear();
        g_initialized = false;
    }

    void Shutdown()
    {
        if (!g_initialized)
            return;

        if (g_hwnd && g_originalWndProc)
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_initialized = false;
    }

    bool IsVisible() { return g_visible; }
}
