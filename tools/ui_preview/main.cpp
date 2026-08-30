// 오버레이 UI 프리뷰 하네스.
//
// 트레이너 DLL은 게임 프로세스 안에서만 살아 있어서, UI를 눈으로 확인하려면 매번 게임을 띄우고
// 주입해야 했다. 이 실행 파일은 같은 D3D12 + ImGui 조합으로 창을 하나 만들고 `src/ui`의 화면
// 코드를 **그대로** 호출한다 (UI 코드는 복제하지 않는다 — 게임 측 카운터만 stubs.cpp가 채운다).
//
//   cp2077_ui_preview.exe --page=debug --theme=light --lang=en --shot=out.bmp
//
// --shot을 주면 지정한 프레임 수만큼 그린 뒤 백버퍼를 BMP로 저장하고 종료한다. 인자가 없으면
// 그냥 창이 떠서 직접 클릭해 볼 수 있다.
#include "../../src/framework.h"
#include "../../src/features/features.h"
#include "../../src/ui/widgets.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    constexpr UINT kFrameCount = 2;
    constexpr UINT kSrvHeapCapacity = 32;

    ComPtr<ID3D12Device> g_device;
    ComPtr<ID3D12CommandQueue> g_queue;
    ComPtr<IDXGISwapChain3> g_swapChain;
    ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> g_srvHeap;
    ComPtr<ID3D12CommandAllocator> g_allocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> g_commandList;
    ComPtr<ID3D12Resource> g_backBuffers[kFrameCount];
    D3D12_CPU_DESCRIPTOR_HANDLE g_rtvHandles[kFrameCount]{};
    ComPtr<ID3D12Fence> g_fence;
    HANDLE g_fenceEvent = nullptr;
    UINT64 g_fenceValue = 0;
    UINT g_srvDescriptorSize = 0;
    bool g_srvUsed[kSrvHeapCapacity]{};
    bool g_quit = false;

    void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                  D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
    {
        for (UINT index = 0; index < kSrvHeapCapacity; ++index)
        {
            if (g_srvUsed[index])
                continue;
            g_srvUsed[index] = true;
            *outCpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            outCpu->ptr += static_cast<SIZE_T>(index) * g_srvDescriptorSize;
            *outGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            outGpu->ptr += static_cast<UINT64>(index) * g_srvDescriptorSize;
            return;
        }
        *outCpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        *outGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    }

    void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE base = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        const UINT index = static_cast<UINT>((cpu.ptr - base.ptr) / g_srvDescriptorSize);
        if (index < kSrvHeapCapacity)
            g_srvUsed[index] = false;
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
            return 1;
        if (message == WM_DESTROY || message == WM_CLOSE)
        {
            g_quit = true;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void WaitForGpu()
    {
        const UINT64 target = ++g_fenceValue;
        g_queue->Signal(g_fence.Get(), target);
        if (g_fence->GetCompletedValue() < target)
        {
            g_fence->SetEventOnCompletion(target, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 2000);
        }
    }

    bool CreateDevice(HWND hwnd, UINT width, UINT height)
    {
        UINT factoryFlags = 0;
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory))))
            return false;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
            return false;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue))))
            return false;

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.BufferCount = kFrameCount;
        swapDesc.Width = width;
        swapDesc.Height = height;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> swapChain1;
        if (FAILED(factory->CreateSwapChainForHwnd(g_queue.Get(), hwnd, &swapDesc, nullptr, nullptr,
                                                   &swapChain1)))
            return false;
        if (FAILED(swapChain1.As(&g_swapChain)))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = kFrameCount;
        if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap))))
            return false;
        const UINT rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < kFrameCount; ++index)
        {
            g_rtvHandles[index] = rtvHandle;
            g_swapChain->GetBuffer(index, IID_PPV_ARGS(&g_backBuffers[index]));
            g_device->CreateRenderTargetView(g_backBuffers[index].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += rtvSize;
            g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[index]));
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = kSrvHeapCapacity;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap))))
            return false;
        g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(), nullptr,
                                               IID_PPV_ARGS(&g_commandList))))
            return false;
        g_commandList->Close();
        if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
            return false;
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return g_fenceEvent != nullptr;
    }

    // 백버퍼를 CPU로 되읽어 32bpp BMP로 저장한다. PNG 인코딩까지 하려면 WIC/COM이 필요한데,
    // 확인용 스크린샷 한 장에 그만한 배선을 들일 이유가 없다 (BMP는 PowerShell 한 줄로 변환된다).
    bool SaveBackBuffer(const char* path, UINT width, UINT height, UINT bufferIndex)
    {
        ID3D12Resource* source = g_backBuffers[bufferIndex].Get();
        const UINT rowPitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                              ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = static_cast<UINT64>(rowPitch) * height;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback))))
            return false;

        g_allocators[0]->Reset();
        g_commandList->Reset(g_allocators[0].Get(), nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition.pResource = source;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_commandList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        destination.PlacedFootprint.Footprint.Width = width;
        destination.PlacedFootprint.Footprint.Height = height;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch = rowPitch;
        D3D12_TEXTURE_COPY_LOCATION origin{};
        origin.pResource = source;
        origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g_commandList->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &barrier);
        g_commandList->Close();
        ID3D12CommandList* lists[] = {g_commandList.Get()};
        g_queue->ExecuteCommandLists(1, lists);
        WaitForGpu();

        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(buffer.Width)};
        if (FAILED(readback->Map(0, &range, &mapped)))
            return false;

        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        for (UINT y = 0; y < height; ++y)
        {
            const unsigned char* sourceRow = static_cast<const unsigned char*>(mapped) +
                                             static_cast<size_t>(y) * rowPitch;
            // BMP는 상하가 뒤집힌 BGRA다.
            unsigned char* targetRow = pixels.data() + static_cast<size_t>(height - 1 - y) * width * 4;
            for (UINT x = 0; x < width; ++x)
            {
                targetRow[x * 4 + 0] = sourceRow[x * 4 + 2];
                targetRow[x * 4 + 1] = sourceRow[x * 4 + 1];
                targetRow[x * 4 + 2] = sourceRow[x * 4 + 0];
                targetRow[x * 4 + 3] = 255;
            }
        }
        readback->Unmap(0, nullptr);

        FILE* file = nullptr;
        if (fopen_s(&file, path, "wb") != 0 || !file)
            return false;
        BITMAPFILEHEADER fileHeader{};
        BITMAPINFOHEADER infoHeader{};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
        fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
        infoHeader.biSize = sizeof(infoHeader);
        infoHeader.biWidth = static_cast<LONG>(width);
        infoHeader.biHeight = static_cast<LONG>(height);
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biCompression = BI_RGB;
        fwrite(&fileHeader, sizeof(fileHeader), 1, file);
        fwrite(&infoHeader, sizeof(infoHeader), 1, file);
        fwrite(pixels.data(), 1, pixels.size(), file);
        fclose(file);
        return true;
    }

    struct Options
    {
        int page = 0;
        bool light = false;
        bool english = false;
        UINT width = 960;
        UINT height = 700;
        int frames = 20;
        std::string shot;
        // 비활성/경고 표현은 기본 상태에서는 보이지 않는다. 그 조합을 한 번에 만들어 주는 변형.
        std::string variant;
        // 시작 안내 토스트는 메뉴가 닫혀 있을 때 뜨는 요소라 따로 확인해야 한다.
        bool toast = false;
        float mouseX = -1.0f;
        float mouseY = -1.0f;
        // 스크롤 끝(푸터와 겹치지 않는지, 마지막 섹션이 잘리지 않는지)을 확인하기 위한 휠 입력.
        float scroll = 0.0f;
    };

    Options ParseOptions(int argc, char** argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            const auto value = [&](const char* prefix) -> std::string {
                const size_t length = strlen(prefix);
                return argument.compare(0, length, prefix) == 0 ? argument.substr(length) : std::string();
            };
            if (!value("--page=").empty())
            {
                const std::string page = value("--page=");
                options.page = page == "esp" ? 1 : page == "misc" ? 2 : page == "debug" ? 3 : 0;
            }
            else if (!value("--theme=").empty())
            {
                options.light = value("--theme=") == "light";
            }
            else if (!value("--lang=").empty())
            {
                options.english = value("--lang=") == "en";
            }
            else if (!value("--width=").empty())
            {
                options.width = static_cast<UINT>(atoi(value("--width=").c_str()));
            }
            else if (!value("--height=").empty())
            {
                options.height = static_cast<UINT>(atoi(value("--height=").c_str()));
            }
            else if (!value("--frames=").empty())
            {
                options.frames = atoi(value("--frames=").c_str());
            }
            else if (!value("--shot=").empty())
            {
                options.shot = value("--shot=");
            }
            else if (!value("--scroll=").empty())
            {
                options.scroll = static_cast<float>(atof(value("--scroll=").c_str()));
            }
            else if (argument == "--toast")
            {
                options.toast = true;
            }
            else if (!value("--variant=").empty())
            {
                options.variant = value("--variant=");
            }
            else if (!value("--mouse=").empty())
            {
                const std::string point = value("--mouse=");
                const size_t comma = point.find(',');
                if (comma != std::string::npos)
                {
                    options.mouseX = static_cast<float>(atof(point.substr(0, comma).c_str()));
                    options.mouseY = static_cast<float>(atof(point.substr(comma + 1).c_str()));
                }
            }
        }
        return options;
    }
}

int main(int argc, char** argv)
{
    const Options options = ParseOptions(argc, argv);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"cbpk_ui_preview";
    RegisterClassExW(&windowClass);

    RECT rect{0, 0, static_cast<LONG>(options.width), static_cast<LONG>(options.height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(windowClass.lpszClassName, L"CBPK overlay preview", WS_OVERLAPPEDWINDOW, 80, 60,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                              windowClass.hInstance, nullptr);
    if (!hwnd || !CreateDevice(hwnd, options.width, options.height))
    {
        fprintf(stderr, "preview: failed to create the D3D12 device or swap chain\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    Widgets::ApplyStyle();
    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = g_device.Get();
    initInfo.CommandQueue = g_queue.Get();
    initInfo.NumFramesInFlight = kFrameCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.SrvDescriptorHeap = g_srvHeap.Get();
    initInfo.SrvDescriptorAllocFn = SrvAlloc;
    initInfo.SrvDescriptorFreeFn = SrvFree;
    ImGui_ImplDX12_Init(&initInfo);

    Features::Settings& settings = Features::GetSettings();
    settings.ui.theme = options.light ? Features::Theme::Light : Features::Theme::Dark;
    settings.ui.language = options.english ? Features::Language::English : Features::Language::Korean;
    // 프리뷰에서는 기능이 켜져 있어야 실제 활성 상태의 컨트롤을 볼 수 있다. 비활성 표현은 각
    // 페이지의 하위 토글(예: 체력 상한)에서 그대로 관찰된다.
    settings.aimbot.enabled = true;
    settings.esp.enabled = true;
    settings.debug.showInternalStats = true;
    if (options.variant == "off")
    {
        // 기능이 꺼져 있을 때 하위 설정이 "꺼짐"이 아니라 "비활성"으로 보이는지 확인한다.
        settings.aimbot.enabled = false;
        settings.esp.enabled = false;
    }
    else if (options.variant == "warn")
    {
        // 경고 박스와, 사일런트 조준에서 스무딩이 비활성이 되는 표현을 확인한다.
        settings.aimbot.silentAim = true;
        settings.esp.visibilityCheck = true;
        settings.debug.headlessAimbot = true;
        settings.debug.debuggerOutput = true;
    }
    Widgets::SelectPage(options.page);

    int frame = 0;
    bool done = false;
    while (!done)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                done = true;
        }
        if (done || g_quit)
            break;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // 호버/선택 표현을 캡처하려면 커서를 코드로 놓아야 한다 (스크린샷은 무인 실행이다).
        if (options.mouseX >= 0.0f)
        {
            ImGui::GetIO().AddMousePosEvent(options.mouseX, options.mouseY);
            ImGui::GetIO().MouseDrawCursor = true;
        }
        if (options.scroll != 0.0f)
        {
            // 휠은 커서가 올라가 있는 자식 창을 스크롤한다. 본문 한가운데를 가리켜 둔다.
            ImGui::GetIO().AddMousePosEvent(options.width * 0.5f, options.height * 0.5f);
            ImGui::GetIO().AddMouseWheelEvent(0.0f, -options.scroll);
        }
        ImGui::NewFrame();

        // 게임 장면 대용 체커보드. 오버레이가 실제로 불투명한지(뒤가 비치지 않는지)는 단색
        // 배경 위에서는 확인할 수 없다.
        ImDrawList* scene = ImGui::GetBackgroundDrawList();
        for (int y = 0; y * 64 < static_cast<int>(options.height); ++y)
        {
            for (int x = 0; x * 64 < static_cast<int>(options.width); ++x)
            {
                const ImU32 tint = ((x + y) & 1) ? IM_COL32(38, 86, 74, 255) : IM_COL32(28, 66, 58, 255);
                scene->AddRectFilled(ImVec2(x * 64.0f, y * 64.0f), ImVec2(x * 64.0f + 64.0f, y * 64.0f + 64.0f),
                                     tint);
            }
        }

        // 창 전체를 메뉴가 채우게 해서 캡처가 곧 UI 스크린샷이 되도록 한다. SetNextWindowSize는
        // DrawMainMenu 안의 기본 크기 지정에 덮이므로 이름으로 지정하는 쪽을 쓴다.
        if (!options.toast)
        {
            ImGui::SetWindowPos("##trainer_main_window", ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetWindowSize("##trainer_main_window",
                                 ImVec2(static_cast<float>(options.width), static_cast<float>(options.height)),
                                 ImGuiCond_Always);
        }
        if (options.toast)
            Widgets::DrawStartupHint();
        else
            Widgets::DrawMainMenu();
        ImGui::Render();

        const UINT backBufferIndex = g_swapChain->GetCurrentBackBufferIndex();
        g_allocators[backBufferIndex]->Reset();
        g_commandList->Reset(g_allocators[backBufferIndex].Get(), nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition.pResource = g_backBuffers[backBufferIndex].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &barrier);

        const float clearColor[4] = {0.11f, 0.26f, 0.23f, 1.0f};
        g_commandList->ClearRenderTargetView(g_rtvHandles[backBufferIndex], clearColor, 0, nullptr);
        g_commandList->OMSetRenderTargets(1, &g_rtvHandles[backBufferIndex], FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
        g_commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList.Get());

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &barrier);
        g_commandList->Close();
        ID3D12CommandList* lists[] = {g_commandList.Get()};
        g_queue->ExecuteCommandLists(1, lists);
        g_swapChain->Present(1, 0);
        WaitForGpu();

        ++frame;
        if (!options.shot.empty() && frame >= options.frames)
        {
            // Present 뒤에는 현재 인덱스가 다음 버퍼를 가리키므로 방금 그린 인덱스를 그대로 쓴다.
            if (!SaveBackBuffer(options.shot.c_str(), options.width, options.height, backBufferIndex))
                fprintf(stderr, "preview: failed to save %s\n", options.shot.c_str());
            done = true;
        }
    }

    WaitForGpu();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CloseHandle(g_fenceEvent);
    DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    return 0;
}
