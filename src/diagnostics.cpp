#include "diagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace
{
    SRWLOCK g_logLock = SRWLOCK_INIT;
    HANDLE g_logFile = INVALID_HANDLE_VALUE;
    wchar_t g_logPath[MAX_PATH] = {};

    void WriteLine(const char* line)
    {
        OutputDebugStringA(line);

        AcquireSRWLockExclusive(&g_logLock);
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(g_logFile, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
            FlushFileBuffers(g_logFile);
        }
        ReleaseSRWLockExclusive(&g_logLock);
    }

    const char* SafeName(const char* name)
    {
        return name ? name : "<unnamed>";
    }

#ifdef __ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__
    // page fault VA만으로는 누구 메모리인지 알 수 없다. 원인을 가르는 것은 폴트가 난 VA를 감싸는 할당이
    // 아직 살아 있는 것인지(existing) 최근에 해제된 것인지(recent freed), 그리고 그 오브젝트가 무엇인지다.
    const char* AllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type)
    {
        switch (type)
        {
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "command queue";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "command allocator";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "pipeline state";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "command list";
        case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "fence";
        case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "descriptor heap";
        case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "heap";
        case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "query heap";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "command signature";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "pipeline library";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER: return "video decoder";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_PROCESSOR: return "video processor";
        case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "resource";
        case D3D12_DRED_ALLOCATION_TYPE_PASS: return "pass";
        case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSION: return "crypto session";
        case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSIONPOLICY: return "crypto session policy";
        case D3D12_DRED_ALLOCATION_TYPE_PROTECTEDRESOURCESESSION: return "protected resource session";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER_HEAP: return "video decoder heap";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_POOL: return "command pool";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_RECORDER: return "command recorder";
        case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "state object";
        case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND: return "meta command";
        case D3D12_DRED_ALLOCATION_TYPE_SCHEDULINGGROUP: return "scheduling group";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_MOTION_ESTIMATOR: return "video motion estimator";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_MOTION_VECTOR_HEAP: return "video motion vector heap";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_EXTENSION_COMMAND: return "video extension command";
        case D3D12_DRED_ALLOCATION_TYPE_INVALID: return "invalid";
        default: return "unknown";
        }
    }

    void LogAllocationNodes(const D3D12_DRED_ALLOCATION_NODE1* node, const char* bucket)
    {
        UINT count = 0;
        for (; node && count < 32; ++count, node = node->pNext)
        {
            Diagnostics::Log("DRED %s allocation[%u]: name=%s type=%s(%d) object=%p", bucket, count,
                             SafeName(node->ObjectNameA), AllocationTypeName(node->AllocationType),
                             static_cast<int>(node->AllocationType), node->pObject);
        }
        if (count == 0)
            Diagnostics::Log("DRED %s allocation list empty", bucket);
    }
#endif
}

namespace Diagnostics
{
    void Initialize(HMODULE module)
    {
        wchar_t modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(module, modulePath, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            wchar_t* fileName = wcsrchr(modulePath, L'\\');
            if (fileName)
                *(fileName + 1) = L'\0';
            wcscpy_s(g_logPath, modulePath);
            wcscat_s(g_logPath, L"cp2077_trainer.log");
        }
        else
        {
            wcscpy_s(g_logPath, L"cp2077_trainer.log");
        }

        AcquireSRWLockExclusive(&g_logLock);
        g_logFile = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ReleaseSRWLockExclusive(&g_logLock);

        Log("============================================================");
        Log("diagnostics initialized; module=%p", module);
    }

    void Shutdown()
    {
        Log("diagnostics shutdown");

        AcquireSRWLockExclusive(&g_logLock);
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }
        ReleaseSRWLockExclusive(&g_logLock);
    }

    void Log(const char* format, ...)
    {
        char message[2048] = {};
        va_list args;
        va_start(args, format);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
        va_end(args);

        SYSTEMTIME time{};
        GetLocalTime(&time);

        char line[2304] = {};
        snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u][tid=%lu] %s\r\n", time.wHour, time.wMinute,
                 time.wSecond, time.wMilliseconds, GetCurrentThreadId(), message);
        WriteLine(line);
    }

    void LogHr(const char* operation, HRESULT hr)
    {
        Log("%s failed: hr=0x%08lX", operation, static_cast<unsigned long>(hr));
    }

    void LogDeviceRemovedData(ID3D12Device* device, const char* trigger)
    {
        if (!device)
            return;

        const HRESULT reason = device->GetDeviceRemovedReason();
        Log("device status after %s: hr=0x%08lX", trigger, static_cast<unsigned long>(reason));
        if (SUCCEEDED(reason))
            return;

#ifdef __ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__
        ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
        const HRESULT queryHr = device->QueryInterface(IID_PPV_ARGS(&dred));
        if (FAILED(queryHr))
        {
            LogHr("QueryInterface(ID3D12DeviceRemovedExtendedData1)", queryHr);
            return;
        }

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        const HRESULT breadcrumbHr = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
        if (SUCCEEDED(breadcrumbHr))
        {
            const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;
            UINT count = 0;
            for (; node && count < 16; ++count, node = node->pNext)
            {
                const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                Log("DRED breadcrumb[%u]: list=%s queue=%s completed=%u/%u listPtr=%p queuePtr=%p", count,
                    SafeName(node->pCommandListDebugNameA), SafeName(node->pCommandQueueDebugNameA), last,
                    node->BreadcrumbCount, node->pCommandList, node->pCommandQueue);
            }
            if (count == 0)
                Log("DRED breadcrumb list empty: auto-breadcrumbs are off for this device");
        }
        else
        {
            LogHr("GetAutoBreadcrumbsOutput1", breadcrumbHr);
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
        const HRESULT pageFaultHr = dred->GetPageFaultAllocationOutput1(&pageFault);
        if (SUCCEEDED(pageFaultHr))
        {
            Log("DRED page fault VA=0x%llX", static_cast<unsigned long long>(pageFault.PageFaultVA));
            LogAllocationNodes(pageFault.pHeadExistingAllocationNode, "existing");
            LogAllocationNodes(pageFault.pHeadRecentFreedAllocationNode, "recent freed");
        }
        else
        {
            LogHr("GetPageFaultAllocationOutput1", pageFaultHr);
        }
#endif
    }

    const wchar_t* LogPath()
    {
        return g_logPath;
    }
}
