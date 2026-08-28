#include "diagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <DbgHelp.h>

namespace
{
    SRWLOCK g_logLock = SRWLOCK_INIT;
    HANDLE g_logFile = INVALID_HANDLE_VALUE;
    wchar_t g_logPath[MAX_PATH] = {};
    wchar_t g_baseDir[MAX_PATH] = {};
    PVOID g_vehHandle = nullptr;
    std::atomic<bool> g_dumpWritten{false};

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

    const char* ExceptionCodeToString(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
        case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN / FAST_FAIL";
        case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
        case 0xC0000417: return "STATUS_INVALID_CRUNTIME_PARAMETER";
        case 0x80000001: return "STATUS_GUARD_PAGE_VIOLATION";
        default: return "UNKNOWN_EXCEPTION";
        }
    }

    bool WriteMiniDumpInternal(PEXCEPTION_POINTERS exceptionInfo, const char* reason)
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);

        wchar_t dumpPath[MAX_PATH] = {};
        if (g_baseDir[0] != L'\0')
        {
            swprintf_s(dumpPath, L"%lscp2077_crash_%lu_%04u%02u%02u_%02u%02u%02u.dmp",
                       g_baseDir, GetCurrentProcessId(), time.wYear, time.wMonth, time.wDay,
                       time.wHour, time.wMinute, time.wSecond);
        }
        else
        {
            swprintf_s(dumpPath, L"cp2077_crash_%lu_%04u%02u%02u_%02u%02u%02u.dmp",
                       GetCurrentProcessId(), time.wYear, time.wMonth, time.wDay,
                       time.wHour, time.wMinute, time.wSecond);
        }

        HANDLE dumpFile = CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dumpFile == INVALID_HANDLE_VALUE)
        {
            Diagnostics::Log("MiniDump creation failed: cannot open file %ls (error=%lu)", dumpPath, GetLastError());
            return false;
        }

        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exceptionInfo;
        mei.ClientPointers = FALSE;

        const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs | MiniDumpWithProcessThreadData |
            MiniDumpWithHandleData | MiniDumpWithUnloadedModules);

        const BOOL success = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), dumpFile,
            dumpType, exceptionInfo ? &mei : nullptr, nullptr, nullptr);

        CloseHandle(dumpFile);

        if (success)
        {
            Diagnostics::Log("MiniDump written successfully [reason=%s]: path=%ls", reason, dumpPath);
            return true;
        }
        else
        {
            Diagnostics::Log("MiniDumpWriteDump failed [reason=%s]: error=%lu", reason, GetLastError());
            return false;
        }
    }

    bool IsFatalException(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_STACK_CHECK:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INVALID_DISPOSITION:
        case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN / FAST_FAIL
        case 0xC0000374: // STATUS_HEAP_CORRUPTION
        case 0xC0000417: // STATUS_INVALID_CRUNTIME_PARAMETER
        case 0x80000001: // STATUS_GUARD_PAGE_VIOLATION
            return true;
        default:
            return false;
        }
    }

    LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS exceptionInfo)
    {
        if (!exceptionInfo || !exceptionInfo->ExceptionRecord)
            return EXCEPTION_CONTINUE_SEARCH;

        const DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
        if (!IsFatalException(code))
            return EXCEPTION_CONTINUE_SEARCH;

        // Identify fault address and module
        void* faultAddr = exceptionInfo->ExceptionRecord->ExceptionAddress;
        HMODULE hMod = nullptr;
        char modName[MAX_PATH] = "<unknown>";
        uintptr_t modBase = 0;
        uintptr_t modOffset = reinterpret_cast<uintptr_t>(faultAddr);

        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(faultAddr), &hMod) && hMod)
        {
            modBase = reinterpret_cast<uintptr_t>(hMod);
            modOffset = reinterpret_cast<uintptr_t>(faultAddr) - modBase;
            char fullPath[MAX_PATH] = {};
            if (GetModuleFileNameA(hMod, fullPath, MAX_PATH))
            {
                const char* slash = strrchr(fullPath, '\\');
                if (!slash)
                    slash = strrchr(fullPath, '/');
                strncpy_s(modName, slash ? slash + 1 : fullPath, _TRUNCATE);
            }
        }

        Diagnostics::Log("================================================================================");
        Diagnostics::Log("[CRASH/EXCEPTION VEH INTERCEPTED]");
        Diagnostics::Log("ExceptionCode   : 0x%08lX (%s)", code, ExceptionCodeToString(code));
        Diagnostics::Log("FaultAddress    : %p (%s+0x%llX, base=%p)",
                         faultAddr, modName, static_cast<unsigned long long>(modOffset), reinterpret_cast<void*>(modBase));

        if (code == EXCEPTION_ACCESS_VIOLATION && exceptionInfo->ExceptionRecord->NumberParameters >= 2)
        {
            const ULONG_PTR avType = exceptionInfo->ExceptionRecord->ExceptionInformation[0];
            const ULONG_PTR targetAddr = exceptionInfo->ExceptionRecord->ExceptionInformation[1];
            const char* opName = (avType == 0) ? "READ" : ((avType == 1) ? "WRITE" : ((avType == 8) ? "EXECUTE(DEP)" : "UNKNOWN"));
            Diagnostics::Log("AccessViolation : Attempted to %s invalid address %p", opName, reinterpret_cast<void*>(targetAddr));
        }

        PCONTEXT ctx = exceptionInfo->ContextRecord;
        if (ctx)
        {
            Diagnostics::Log("Registers (x64) :");
            Diagnostics::Log("  RIP=%016llX  RSP=%016llX  RBP=%016llX  EFLAGS=%08lX",
                             ctx->Rip, ctx->Rsp, ctx->Rbp, ctx->EFlags);
            Diagnostics::Log("  RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX",
                             ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
            Diagnostics::Log("  RSI=%016llX  RDI=%016llX  R8 =%016llX  R9 =%016llX",
                             ctx->Rsi, ctx->Rdi, ctx->R8, ctx->R9);
            Diagnostics::Log("  R10=%016llX  R11=%016llX  R12=%016llX  R13=%016llX",
                             ctx->R10, ctx->R11, ctx->R12, ctx->R13);
            Diagnostics::Log("  R14=%016llX  R15=%016llX",
                             ctx->R14, ctx->R15);

            if (ctx->Rsp)
            {
                Diagnostics::Log("Stack Walk / Return Addresses (from RSP=%p):", reinterpret_cast<void*>(ctx->Rsp));
                uintptr_t* stackPtr = reinterpret_cast<uintptr_t*>(ctx->Rsp);
                int frameCount = 0;
                __try
                {
                    for (int i = 0; i < 256 && frameCount < 24; ++i)
                    {
                        uintptr_t val = stackPtr[i];
                        if (val < 0x10000)
                            continue;

                        HMODULE frameMod = nullptr;
                        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<LPCSTR>(val), &frameMod) && frameMod)
                        {
                            char framePath[MAX_PATH] = {};
                            if (GetModuleFileNameA(frameMod, framePath, MAX_PATH))
                            {
                                const char* slash = strrchr(framePath, '\\');
                                const char* frameModName = slash ? slash + 1 : framePath;
                                uintptr_t frameOffset = val - reinterpret_cast<uintptr_t>(frameMod);
                                Diagnostics::Log("  frame[%02d] (RSP+%04X): %s+0x%llX (addr=%p)",
                                                 frameCount++, i * 8, frameModName,
                                                 static_cast<unsigned long long>(frameOffset), reinterpret_cast<void*>(val));
                            }
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    Diagnostics::Log("  [stack walk terminated due to memory read fault]");
                }
            }
        }

        Diagnostics::Log("================================================================================");

        // Write minidump once for any fatal exception
        if (!g_dumpWritten.exchange(true))
        {
            WriteMiniDumpInternal(exceptionInfo, "veh_fatal");
        }

        return EXCEPTION_CONTINUE_SEARCH;
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
            wcscpy_s(g_baseDir, modulePath);
            wcscpy_s(g_logPath, modulePath);
            wcscat_s(g_logPath, L"cp2077_trainer.log");
        }
        else
        {
            wcscpy_s(g_baseDir, L"");
            wcscpy_s(g_logPath, L"cp2077_trainer.log");
        }

        AcquireSRWLockExclusive(&g_logLock);
        g_logFile = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ReleaseSRWLockExclusive(&g_logLock);

        g_vehHandle = AddVectoredExceptionHandler(1, VectoredExceptionHandler);

        Log("============================================================");
        Log("diagnostics initialized; module=%p veh=%s", module, g_vehHandle ? "active" : "failed");
    }

    void Shutdown()
    {
        Log("diagnostics shutdown");

        if (g_vehHandle)
        {
            RemoveVectoredExceptionHandler(g_vehHandle);
            g_vehHandle = nullptr;
        }

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

    bool WriteMiniDump(PEXCEPTION_POINTERS exceptionInfo, const char* reason)
    {
        return WriteMiniDumpInternal(exceptionInfo, reason);
    }

    const wchar_t* LogPath()
    {
        return g_logPath;
    }
}
