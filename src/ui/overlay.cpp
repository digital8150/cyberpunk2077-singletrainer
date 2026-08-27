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
#include "../diagnostics.h"
#include "../features/features.h"
#include "../hooks/hook_lifecycle.h"
#include "../hooks/cursor_hook.h"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    struct BackBufferContext
    {
        ComPtr<ID3D12Resource> backBuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
    };

    struct AllocatorContext
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        UINT64 fenceValue = 0;  // 이 얼로케이터를 쓴 마지막 커맨드리스트가 GPU에서 끝났음을 보장하는 값
    };

    // 최신 ImGui DX12 백엔드(1.92+)는 SRV 디스크립터를 필요할 때마다(폰트 외 텍스처가 생길 때) 동적으로
    // 요청한다 — 그래서 힙을 넉넉히 잡고 간단한 슬롯 할당기(alloc/free 콜백)를 붙여줘야 한다.
    constexpr UINT kSrvHeapCapacity = 64;
    constexpr UINT kMinimumAllocatorCount = 4;
    constexpr UINT kMaximumAllocatorCount = 32;

    bool g_initialized = false;
    bool g_renderingDisabled = false;  // 치명적 초기화/동기화 오류 뒤 게임을 살리기 위한 fail-closed 상태
    bool g_visible = false;  // 로드 시 숨김, Insert 키로 토글
    HWND g_hwnd = nullptr;
    IDXGISwapChain3* g_swapChain = nullptr;  // 이 인스턴스 외의 보조 스왑체인 Present는 무시한다.
    WNDPROC g_originalWndProc = nullptr;
    bool g_wndProcRestored = false;
    bool g_imguiContextCreated = false;
    bool g_headlessSilentAim = false;
    bool g_win32BackendInitialized = false;
    bool g_dx12BackendInitialized = false;
    bool g_loggedFirstRenderedFrame = false;
    bool g_loggedAllocatorExpansion = false;

    ComPtr<ID3D12Device> g_device;
    ComPtr<ID3D12CommandQueue> g_commandQueue;
    ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> g_srvHeap;
    ComPtr<ID3D12GraphicsCommandList> g_commandList;
    std::vector<BackBufferContext> g_backBuffers;
    std::vector<AllocatorContext> g_allocatorPool;
    UINT g_bufferCount = 0;
    UINT g_rtvDescriptorSize = 0;
    UINT g_srvDescriptorSize = 0;
    DXGI_FORMAT g_rtvFormat = DXGI_FORMAT_UNKNOWN;
    bool g_srvSlotUsed[kSrvHeapCapacity] = {};

    // 프레임 동기화용 펜스. D3D12는 커맨드 얼로케이터를 "GPU가 그걸로 만든 커맨드리스트 실행을 다 끝낸
    // 뒤"에만 Reset해도 된다 — 이걸 안 지키면 정의되지 않은 동작이고, 실제로 드라이버 TDR/행을 유발할 수
    // 있다(첫 인젝션 테스트에서 겪은 크래시의 유력한 원인 — 원래 코드엔 이 동기화가 아예 없었음).
    ComPtr<ID3D12Fence> g_fence;
    UINT64 g_fenceLastSignaled = 0;
    HANDLE g_fenceEvent = nullptr;
    std::mutex g_renderMutex;
    std::atomic_uint64_t g_presentCount{0};
    std::atomic_uint64_t g_submitCount{0};
    std::atomic_uint64_t g_allocatorMissCount{0};
    ULONGLONG g_lastTelemetryTick = 0;
    UINT g_telemetryWindows = 0;

    // 게임 자체 GPU 에러 핸들러(gpuApiDX12Error)가 device removal을 감지하면 곧바로 프로세스를 죽이기
    // 때문에, Present 경로의 펜스 검사까지 살아남지 못하는 크래시가 있다. 별도 스레드가 짧은 주기로
    // GetDeviceRemovedReason을 확인해 DRED breadcrumb/page fault를 먼저 덤프한다. DRED 자체는 디바이스
    // 생성 전에 켜져 있어야 데이터가 채워지므로, 주입 전에 d3dconfig로 forced-on 해 둘 것.
    constexpr DWORD kDeviceWatchdogPollMs = 8;
    HANDLE g_deviceWatchdogThread = nullptr;
    HANDLE g_deviceWatchdogStop = nullptr;

    DWORD WINAPI DeviceRemovalWatchdog(LPVOID parameter)
    {
        ID3D12Device* device = static_cast<ID3D12Device*>(parameter);
        for (;;)
        {
            if (WaitForSingleObject(g_deviceWatchdogStop, kDeviceWatchdogPollMs) == WAIT_OBJECT_0)
                break;

            const HRESULT reason = device->GetDeviceRemovedReason();
            if (FAILED(reason))
            {
                Diagnostics::Log("device watchdog observed removal");
                Diagnostics::LogDeviceRemovedData(device, "device watchdog");
                break;
            }
        }
        device->Release();
        return 0;
    }

    void StartDeviceWatchdog()
    {
        if (g_deviceWatchdogThread || !g_device)
            return;

        g_deviceWatchdogStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_deviceWatchdogStop)
        {
            Diagnostics::Log("device watchdog not started: CreateEventW failed error=%lu", GetLastError());
            return;
        }

        ID3D12Device* device = g_device.Get();
        device->AddRef();
        g_deviceWatchdogThread = CreateThread(nullptr, 0, DeviceRemovalWatchdog, device, 0, nullptr);
        if (!g_deviceWatchdogThread)
        {
            Diagnostics::Log("device watchdog not started: CreateThread failed error=%lu", GetLastError());
            device->Release();
            CloseHandle(g_deviceWatchdogStop);
            g_deviceWatchdogStop = nullptr;
            return;
        }
        Diagnostics::Log("device watchdog started: pollMs=%lu", kDeviceWatchdogPollMs);
    }

    void StopDeviceWatchdog()
    {
        if (g_deviceWatchdogStop)
            SetEvent(g_deviceWatchdogStop);
        if (g_deviceWatchdogThread)
        {
            WaitForSingleObject(g_deviceWatchdogThread, INFINITE);
            CloseHandle(g_deviceWatchdogThread);
            g_deviceWatchdogThread = nullptr;
        }
        if (g_deviceWatchdogStop)
        {
            CloseHandle(g_deviceWatchdogStop);
            g_deviceWatchdogStop = nullptr;
        }
    }

    void DisableOverlayRendering()
    {
        g_renderingDisabled = true;
        g_visible = false;
        if (g_imguiContextCreated && ImGui::GetCurrentContext())
            ImGui::GetIO().MouseDrawCursor = false;
    }

    enum class AllocatorStatus
    {
        Ready,
        Exhausted,
        Failed,
    };

    // DLSS Frame Generation can issue asynchronous presents and keep more frames in flight than the swap-chain's
    // back-buffer count. Allocators therefore live in a separate pool instead of being fixed one-per-back-buffer.
    // A busy allocator is never reset; the pool grows on demand up to a conservative bound.
    AllocatorStatus AcquireAllocator(AllocatorContext*& result)
    {
        result = nullptr;
        const UINT64 completed = g_fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            Diagnostics::Log("overlay fence reports device removal");
            Diagnostics::LogDeviceRemovedData(g_device.Get(), "fence GetCompletedValue");
            return AllocatorStatus::Failed;
        }

        for (AllocatorContext& allocator : g_allocatorPool)
        {
            if (allocator.fenceValue == 0 || completed >= allocator.fenceValue)
            {
                result = &allocator;
                return AllocatorStatus::Ready;
            }
        }

        if (g_allocatorPool.size() < kMaximumAllocatorCount)
        {
            AllocatorContext allocator;
            const HRESULT hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                 IID_PPV_ARGS(&allocator.commandAllocator));
            if (FAILED(hr))
            {
                Diagnostics::LogHr("CreateCommandAllocator(pool expansion)", hr);
                return AllocatorStatus::Failed;
            }
            g_allocatorPool.emplace_back(std::move(allocator));
            result = &g_allocatorPool.back();
            if (!g_loggedAllocatorExpansion)
            {
                Diagnostics::Log("overlay allocator pool expanded beyond initial size: size=%zu",
                                 g_allocatorPool.size());
                g_loggedAllocatorExpansion = true;
            }
            return AllocatorStatus::Ready;
        }

        g_allocatorMissCount.fetch_add(1, std::memory_order_relaxed);
        return AllocatorStatus::Exhausted;
    }

    void MaybeLogTelemetry()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_lastTelemetryTick == 0)
        {
            g_lastTelemetryTick = now;
            return;
        }
        if (now - g_lastTelemetryTick < 5000)
            return;

        const std::uint64_t presents = g_presentCount.exchange(0, std::memory_order_relaxed);
        const std::uint64_t submits = g_submitCount.exchange(0, std::memory_order_relaxed);
        const std::uint64_t misses = g_allocatorMissCount.exchange(0, std::memory_order_relaxed);
        ++g_telemetryWindows;
        if (g_telemetryWindows <= 3 || misses > 0)
        {
            Diagnostics::Log("overlay cadence (5s): presents=%llu submitted=%llu allocatorMisses=%llu pool=%zu",
                             static_cast<unsigned long long>(presents), static_cast<unsigned long long>(submits),
                             static_cast<unsigned long long>(misses), g_allocatorPool.size());
        }
        g_lastTelemetryTick = now;
    }

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
        Diagnostics::Log("SRV descriptor heap exhausted; capacity=%u", kSrvHeapCapacity);
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

    // 메뉴가 떠 있는 동안 게임으로 넘기면 안 되는 입력 메시지들 — 안 막으면 메뉴를 클릭/타이핑하는 동안
    // 카메라가 같이 돌아가거나 총이 나가는 등 게임이 입력을 동시에 받는다. WM_INPUT은 FPS류 게임이 raw
    // mouse look에 흔히 쓰는 메시지라 이것도 같이 막아야 마우스가 실제로 메뉴 조작에만 쓰인다.
    bool IsInputMessage(UINT msg)
    {
        return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
               msg == WM_INPUT || msg == WM_CHAR || msg == WM_SETCURSOR;
    }

    LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        HookLifecycle::CallbackGuard callback;
        WNDPROC originalWndProc = g_originalWndProc;
        if (HookLifecycle::IsShuttingDown())
            return CallWindowProcW(originalWndProc, hwnd, msg, wParam, lParam);

        if (msg == WM_KEYDOWN && wParam == VK_INSERT)
        {
            g_visible = !g_visible;
            Diagnostics::Log("overlay visibility toggled: visible=%d", g_visible ? 1 : 0);
            ImGui::GetIO().MouseDrawCursor = g_visible;
            CursorHook::SetMenuCapture(g_visible);
            // 게임이 매 프레임 마우스를 창 중앙에 클립/고정해서 커서가 안 보이는 경우가 많다 (raw input
            // 카메라 조작) — 메뉴를 열 때 클립을 풀어서 커서가 창 안에서 자유롭게 움직이게 한다.
            if (g_visible)
                ClipCursor(nullptr);
        }

        // 메뉴가 떠 있을 때만 ImGui가 입력을 가로챈다 — 숨겨져 있으면 게임이 입력을 그대로 받는다.
        // Insert는 게임이 원래 안 쓰는 키라 별도 예외 없이 그냥 메뉴가 열린 동안엔 모든 입력을 막는다.
        if (g_visible)
        {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            if (IsInputMessage(msg))
                return true;
        }

        return CallWindowProcW(originalWndProc, hwnd, msg, wParam, lParam);
    }

    bool CreateRenderTargets(IDXGISwapChain3* swapChain)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < g_bufferCount; ++i)
        {
            ComPtr<ID3D12Resource> backBuffer;
            const HRESULT hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
            if (FAILED(hr))
            {
                Diagnostics::LogHr("IDXGISwapChain::GetBuffer", hr);
                return false;
            }
            g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

            g_backBuffers[i].backBuffer = backBuffer;
            g_backBuffers[i].rtvHandle = rtvHandle;
            rtvHandle.ptr += g_rtvDescriptorSize;
        }
        return true;
    }

    void ReleaseOverlayResources(bool waitForGpu)
    {
        if (g_hwnd && g_originalWndProc && !g_wndProcRestored)
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        g_originalWndProc = nullptr;
        g_wndProcRestored = false;
        CursorHook::SetMenuCapture(false);

        if (waitForGpu && g_fence && g_fenceEvent && g_fenceLastSignaled > 0)
        {
            const UINT64 completed = g_fence->GetCompletedValue();
            if (completed != UINT64_MAX && completed < g_fenceLastSignaled)
            {
                const HRESULT hr = g_fence->SetEventOnCompletion(g_fenceLastSignaled, g_fenceEvent);
                if (SUCCEEDED(hr))
                {
                    const DWORD waitResult = WaitForSingleObject(g_fenceEvent, 250);
                    if (waitResult != WAIT_OBJECT_0)
                        Diagnostics::Log("shutdown fence wait skipped after result=0x%08lX", waitResult);
                }
                else
                {
                    Diagnostics::LogHr("shutdown SetEventOnCompletion", hr);
                }
            }
        }

        // DX12 백엔드가 SRV free 콜백을 호출할 수 있으므로 SRV 힙보다 먼저 종료한다.
        if (g_dx12BackendInitialized)
        {
            ImGui_ImplDX12_Shutdown();
            g_dx12BackendInitialized = false;
        }
        if (g_win32BackendInitialized)
        {
            ImGui_ImplWin32_Shutdown();
            g_win32BackendInitialized = false;
        }
        if (g_imguiContextCreated && ImGui::GetCurrentContext())
        {
            ImGui::DestroyContext();
            g_imguiContextCreated = false;
        }

        if (g_fenceEvent)
        {
            CloseHandle(g_fenceEvent);
            g_fenceEvent = nullptr;
        }

        g_backBuffers.clear();
        g_allocatorPool.clear();
        g_commandList.Reset();
        g_commandQueue.Reset();
        g_fence.Reset();
        g_srvHeap.Reset();
        g_rtvHeap.Reset();
        StopDeviceWatchdog();
        g_device.Reset();

        g_fenceLastSignaled = 0;
        g_bufferCount = 0;
        g_rtvDescriptorSize = 0;
        g_srvDescriptorSize = 0;
        g_rtvFormat = DXGI_FORMAT_UNKNOWN;
        g_hwnd = nullptr;
        g_swapChain = nullptr;
        g_initialized = false;
        g_loggedFirstRenderedFrame = false;
        g_loggedAllocatorExpansion = false;
        g_presentCount.store(0, std::memory_order_relaxed);
        g_submitCount.store(0, std::memory_order_relaxed);
        g_allocatorMissCount.store(0, std::memory_order_relaxed);
        g_lastTelemetryTick = 0;
        g_telemetryWindows = 0;
        g_headlessSilentAim = false;
        for (bool& used : g_srvSlotUsed)
            used = false;
    }

    bool InitializeForSwapChain(IDXGISwapChain3* swapChain, ID3D12CommandQueue* commandQueue)
    {
        Diagnostics::Log("overlay initialization started: swapChain=%p queue=%p", swapChain, commandQueue);

        DXGI_SWAP_CHAIN_DESC desc{};
        HRESULT hr = swapChain->GetDesc(&desc);
        if (FAILED(hr))
        {
            Diagnostics::LogHr("IDXGISwapChain::GetDesc", hr);
            return false;
        }
        g_hwnd = desc.OutputWindow;
        g_bufferCount = desc.BufferCount;
        g_rtvFormat = desc.BufferDesc.Format;
        Diagnostics::Log("swap-chain desc: hwnd=%p buffers=%u format=%u size=%ux%u flags=0x%X", g_hwnd,
                         g_bufferCount, static_cast<unsigned>(g_rtvFormat), desc.BufferDesc.Width,
                         desc.BufferDesc.Height, desc.Flags);

        if (!g_hwnd || g_bufferCount == 0 || g_rtvFormat == DXGI_FORMAT_UNKNOWN)
        {
            Diagnostics::Log("invalid swap-chain description");
            return false;
        }

        hr = swapChain->GetDevice(IID_PPV_ARGS(&g_device));
        if (FAILED(hr))
        {
            Diagnostics::LogHr("IDXGISwapChain::GetDevice", hr);
            return false;
        }

        StartDeviceWatchdog();

        ComPtr<ID3D12Device> queueDevice;
        hr = commandQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
        if (FAILED(hr))
        {
            Diagnostics::LogHr("ID3D12CommandQueue::GetDevice", hr);
            return false;
        }

        ComPtr<IUnknown> swapChainDeviceIdentity;
        ComPtr<IUnknown> queueDeviceIdentity;
        g_device.As(&swapChainDeviceIdentity);
        queueDevice.As(&queueDeviceIdentity);
        if (swapChainDeviceIdentity.Get() != queueDeviceIdentity.Get())
        {
            Diagnostics::Log("queue candidate belongs to a different D3D12 device; refusing overlay submission");
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = g_bufferCount;
        hr = g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap));
        if (FAILED(hr))
        {
            Diagnostics::LogHr("CreateDescriptorHeap(RTV)", hr);
            return false;
        }
        g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = kSrvHeapCapacity;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap));
        if (FAILED(hr))
        {
            Diagnostics::LogHr("CreateDescriptorHeap(SRV)", hr);
            return false;
        }
        g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (bool& used : g_srvSlotUsed)
            used = false;

        g_backBuffers.resize(g_bufferCount);
        const UINT initialAllocatorCount = (std::max)(g_bufferCount, kMinimumAllocatorCount);
        g_allocatorPool.resize(initialAllocatorCount);
        for (auto& allocator : g_allocatorPool)
        {
            hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator.commandAllocator));
            if (FAILED(hr))
            {
                Diagnostics::LogHr("CreateCommandAllocator", hr);
                return false;
            }
        }

        hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         g_allocatorPool[0].commandAllocator.Get(), nullptr,
                                         IID_PPV_ARGS(&g_commandList));
        if (FAILED(hr))
        {
            Diagnostics::LogHr("CreateCommandList", hr);
            return false;
        }
        g_commandList->SetName(L"cp2077_trainer ImGui command list");
        hr = g_commandList->Close();
        if (FAILED(hr))
        {
            Diagnostics::LogHr("ID3D12GraphicsCommandList::Close(initial)", hr);
            return false;
        }

        hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        if (FAILED(hr))
        {
            Diagnostics::LogHr("CreateFence", hr);
            return false;
        }
        g_fence->SetName(L"cp2077_trainer ImGui fence");
        g_fenceLastSignaled = 0;
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_fenceEvent)
            return false;

        if (!CreateRenderTargets(swapChain))
            return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        g_imguiContextCreated = true;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::GetIO().MouseDrawCursor = g_visible;  // 게임이 OS 커서를 숨기는 경우가 많아 ImGui가 직접 그림
        Widgets::ApplyStyle();

        if (!ImGui_ImplWin32_Init(g_hwnd))
        {
            Diagnostics::Log("ImGui_ImplWin32_Init failed");
            return false;
        }
        g_win32BackendInitialized = true;

        ImGui_ImplDX12_InitInfo initInfo{};
        initInfo.Device = g_device.Get();
        initInfo.CommandQueue = commandQueue;  // 텍스처 업로드용 (신규 백엔드가 요구)
        initInfo.NumFramesInFlight = static_cast<int>(g_bufferCount);
        // 게임의 실제 스왑체인 포맷을 써야 한다. HDR에서 흔한 R10G10B10A2/R16G16B16A16 등에
        // R8G8B8A8용 PSO를 제출하면 디버그 레이어 오류 또는 device removal이 날 수 있다.
        initInfo.RTVFormat = g_rtvFormat;
        initInfo.SrvDescriptorHeap = g_srvHeap.Get();
        initInfo.SrvDescriptorAllocFn = SrvDescriptorAlloc;
        initInfo.SrvDescriptorFreeFn = SrvDescriptorFree;
        if (!ImGui_ImplDX12_Init(&initInfo))
        {
            Diagnostics::Log("ImGui_ImplDX12_Init failed; RTV format=%u", static_cast<unsigned>(g_rtvFormat));
            return false;
        }
        g_dx12BackendInitialized = true;

        SetLastError(ERROR_SUCCESS);
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));
        if (!g_originalWndProc)
        {
            Diagnostics::Log("SetWindowLongPtrW(WndProc) failed: error=%lu", GetLastError());
            return false;
        }
        g_wndProcRestored = false;
        CursorHook::SetMenuCapture(g_visible);

        g_swapChain = swapChain;
        g_commandQueue = commandQueue;
        Diagnostics::Log("overlay initialization completed");
        return true;
    }

    void RunHeadlessSilentAimFrame(IDXGISwapChain3* swapChain)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDesc(&desc)))
            return;

        if (!g_headlessSilentAim)
        {
            g_headlessSilentAim = true;
            Diagnostics::Log("headless diagnostics frame active: no D3D12 overlay submissions");
        }
        Features::UpdateHeadless(static_cast<float>(desc.BufferDesc.Width),
                                 static_cast<float>(desc.BufferDesc.Height));
    }
}

namespace Overlay
{
    void OnPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* commandQueue)
    {
        std::lock_guard<std::mutex> renderGuard(g_renderMutex);

        // 오버레이 펜스가 device removal을 두 번 관측했다 (00:20:15 hr=0x887A0006 DEVICE_HUNG,
        // 02:42:53 hr=0x887A0001). 두 번 모두 DRED가 꺼져 있어 breadcrumb/page fault 데이터는 없었고,
        // "제출 큐가 원인"은 아직 측정으로 확인되지 않은 가설이다. 그 가설을 격리해 볼 수 있도록 GPU
        // 제출 없이 타겟 선택만 CPU에서 돌리는 진단 모드를 남겨둔다. 예전에는 사일런트 에임을 켜기만 하면
        // 무조건 이 경로로 빠져 메뉴가 통째로 사라졌고, WndProc 훅이 오버레이 초기화 경로에서만 걸리므로
        // Insert까지 죽어서 설정을 되돌릴 방법이 없었다. 이제는 별도 진단 토글로 분리하고, 오버레이가 한 번
        // 초기화된 뒤 메뉴가 닫혀 있을 때만 적용한다.
        if (swapChain && g_initialized && swapChain == g_swapChain && !g_visible &&
            Features::GetSettings().aimbot.headlessDiagnostics)
        {
            RunHeadlessSilentAimFrame(swapChain);
            return;
        }
        if (g_headlessSilentAim)
        {
            g_headlessSilentAim = false;
            Diagnostics::Log("headless diagnostics frame ended: overlay submissions resumed");
        }

        // 렌더 큐를 아직 못 잡았으면(ExecuteCommandLists가 이번 세션에서 아직 한 번도 안 불렸으면) 초기화도
        // 포함해서 이번 프레임은 통째로 건너뛴다 — 새 ImGui DX12 백엔드는 Init 시점에 CommandQueue가 필요함.
        if (!swapChain || (!commandQueue && !g_commandQueue) || g_renderingDisabled)
            return;

        if (g_initialized && swapChain != g_swapChain)
            return;

        g_presentCount.fetch_add(1, std::memory_order_relaxed);
        MaybeLogTelemetry();

        if (!g_initialized)
        {
            if (!InitializeForSwapChain(swapChain, commandQueue))
            {
                Diagnostics::Log("overlay initialization failed; rendering disabled for this swap chain");
                ReleaseOverlayResources(false);
                DisableOverlayRendering();
                return;
            }
            g_initialized = true;
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        Features::DrawOverlay();
        Widgets::DrawStartupHint();
        if (g_visible)
            Widgets::DrawMainMenu();

        ImGui::Render();

        ID3D12CommandQueue* renderQueue = g_commandQueue ? g_commandQueue.Get() : commandQueue;
        const UINT backBufferIndex = swapChain->GetCurrentBackBufferIndex();
        if (backBufferIndex >= g_backBuffers.size())
        {
            Diagnostics::Log("invalid back-buffer index: index=%u contexts=%zu", backBufferIndex,
                             g_backBuffers.size());
            DisableOverlayRendering();
            return;
        }
        BackBufferContext& backBuffer = g_backBuffers[backBufferIndex];

        AllocatorContext* allocator = nullptr;
        const AllocatorStatus allocatorStatus = AcquireAllocator(allocator);
        if (allocatorStatus == AllocatorStatus::Exhausted)
            return;
        if (allocatorStatus == AllocatorStatus::Failed || !allocator)
        {
            Diagnostics::Log("overlay rendering disabled after fence/device failure");
            DisableOverlayRendering();
            return;
        }

        HRESULT hr = allocator->commandAllocator->Reset();
        if (FAILED(hr))
        {
            Diagnostics::LogHr("ID3D12CommandAllocator::Reset", hr);
            Diagnostics::LogDeviceRemovedData(g_device.Get(), "command allocator Reset");
            DisableOverlayRendering();
            return;
        }
        hr = g_commandList->Reset(allocator->commandAllocator.Get(), nullptr);
        if (FAILED(hr))
        {
            Diagnostics::LogHr("ID3D12GraphicsCommandList::Reset", hr);
            Diagnostics::LogDeviceRemovedData(g_device.Get(), "command list Reset");
            DisableOverlayRendering();
            return;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffer.backBuffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &barrier);

        g_commandList->OMSetRenderTargets(1, &backBuffer.rtvHandle, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
        g_commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList.Get());

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &barrier);

        hr = g_commandList->Close();
        if (FAILED(hr))
        {
            Diagnostics::LogHr("ID3D12GraphicsCommandList::Close", hr);
            Diagnostics::LogDeviceRemovedData(g_device.Get(), "command list Close");
            DisableOverlayRendering();
            return;
        }
        ID3D12CommandList* lists[] = {g_commandList.Get()};
        // 주의: commandQueue의 vtable도 훅되어 있으므로 이 호출은 hkExecuteCommandLists를 한 번 더
        // 거쳐간다 (같은 Direct 큐가 스레드별 마지막 큐로 다시 기록될 뿐 원본으로 패스스루된다).
        renderQueue->ExecuteCommandLists(1, lists);

        // 이 프레임에서 이 얼로케이터를 다시 Reset해도 되는 시점을 펜스 값으로 남겨둔다.
        ++g_fenceLastSignaled;
        hr = renderQueue->Signal(g_fence.Get(), g_fenceLastSignaled);
        if (FAILED(hr))
        {
            Diagnostics::LogHr("ID3D12CommandQueue::Signal", hr);
            Diagnostics::LogDeviceRemovedData(g_device.Get(), "command queue Signal");
            DisableOverlayRendering();
            return;
        }
        allocator->fenceValue = g_fenceLastSignaled;
        g_submitCount.fetch_add(1, std::memory_order_relaxed);

        if (!g_loggedFirstRenderedFrame)
        {
            Diagnostics::Log("first overlay frame submitted: buffer=%u fence=%llu", backBufferIndex,
                             static_cast<unsigned long long>(g_fenceLastSignaled));
            g_loggedFirstRenderedFrame = true;
        }
    }

    void OnResizeBuffers(IDXGISwapChain3* swapChain)
    {
        std::lock_guard<std::mutex> renderGuard(g_renderMutex);
        if (!g_initialized || swapChain != g_swapChain)
            return;

        // TODO: ImGui_ImplDX12_InvalidateDeviceObjects 등으로 좀 더 매끄럽게 처리 가능. 지금은 안전하게
        // 전체 재초기화 경로(다음 OnPresent에서 InitializeForSwapChain 재실행)를 탄다.
        Diagnostics::Log("ResizeBuffers intercepted; releasing overlay resources");
        ReleaseOverlayResources(true);
        g_renderingDisabled = false;
    }

    bool BeginShutdown()
    {
        // WndProc is not a MinHook detour. Restore the window callback separately, but keep the saved function
        // pointer alive until HookLifecycle confirms that callbacks which already entered this DLL have returned.
        if (g_hwnd && g_originalWndProc && !g_wndProcRestored)
        {
            SetLastError(ERROR_SUCCESS);
            const LONG_PTR previous =
                SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
            if (previous == 0 && GetLastError() != ERROR_SUCCESS)
            {
                Diagnostics::Log("failed to restore WndProc during shutdown: error=%lu", GetLastError());
                return false;
            }
            g_wndProcRestored = true;
        }
        g_visible = false;
        CursorHook::SetMenuCapture(false);
        return true;
    }

    void Shutdown()
    {
        ReleaseOverlayResources(true);
        DisableOverlayRendering();
    }

    bool IsVisible() { return g_visible; }
}
