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
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;
            for (UINT i = 0; node && i < 16; ++i, node = node->pNext)
            {
                const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                Log("DRED breadcrumb[%u]: list=%s queue=%s completed=%u/%u listPtr=%p queuePtr=%p", i,
                    SafeName(node->pCommandListDebugNameA), SafeName(node->pCommandQueueDebugNameA), last,
                    node->BreadcrumbCount, node->pCommandList, node->pCommandQueue);
            }
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)) && pageFault.PageFaultVA != 0)
        {
            Log("DRED page fault VA=0x%llX", static_cast<unsigned long long>(pageFault.PageFaultVA));
        }
#endif
    }

    const wchar_t* LogPath()
    {
        return g_logPath;
    }
}
