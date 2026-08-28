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
        // 여기 있는 코드만 "프로세스를 죽일 만한 폴트"로 본다. 예전 목록에는 게임이 정상 동작 중에
        // 일상적으로 일으키고 스스로 복구하는 것들이 섞여 있었다:
        //   - STATUS_GUARD_PAGE_VIOLATION: 스택 확장/할당자 write barrier에 쓰인다.
        //   - EXCEPTION_IN_PAGE_ERROR: 메모리 맵 파일 페이징. 이 게임은 애셋을 그렇게 스트리밍한다.
        //   - EXCEPTION_FLT_*, DATATYPE_MISALIGNMENT, ARRAY_BOUNDS: 마스킹되거나 상위에서 처리된다.
        // 그것들까지 잡으면 정상 경로마다 핸들러가 돌아 손해만 본다. ACCESS_VIOLATION은 남겨두되,
        // 이것도 엔티티 스트리밍 중에는 정상적으로 발생하고 게임 SEH가 복구한다는 점을 기억할 것.
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_INVALID_DISPOSITION:
        case EXCEPTION_STACK_OVERFLOW:
        case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN / FAST_FAIL
        case 0xC0000374: // STATUS_HEAP_CORRUPTION
        case 0xC0000417: // STATUS_INVALID_CRUNTIME_PARAMETER
            return true;
        default:
            return false;
        }
    }

    // VEH가 남기는 것은 이 구조체 하나가 전부다. 포맷도, 모듈 조회도, 락도, 디스크도 없다.
    struct ExceptionRecordSlot
    {
        DWORD code;
        DWORD threadId;
        std::uintptr_t faultAddress;
        std::uintptr_t targetAddress;
        ULONG_PTR accessType;
        bool hasAccessInfo;
    };

    constexpr std::size_t kExceptionRingSize = 32;
    ExceptionRecordSlot g_exceptionRing[kExceptionRingSize]{};
    std::atomic<std::uint32_t> g_exceptionWriteIndex{0};
    std::atomic<std::uint32_t> g_exceptionDrainIndex{0};
    std::atomic<std::uint32_t> g_exceptionDropped{0};

    // 핸들러 재진입 차단. 핸들러 안에서 다시 폴트가 나면 (예전 구현은 로더 락과 디스크 I/O를 거기서
    // 했으므로 충분히 가능했다) 무한 재귀로 스택을 태운다. 스택 오버플로 예외까지 fatal로 잡고 있어서
    // 그 경우 확실히 죽는 구조였다.
    thread_local bool t_insideVeh = false;

    LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS exceptionInfo)
    {
        if (!exceptionInfo || !exceptionInfo->ExceptionRecord)
            return EXCEPTION_CONTINUE_SEARCH;

        const DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
        if (!IsFatalException(code) || t_insideVeh)
            return EXCEPTION_CONTINUE_SEARCH;

        t_insideVeh = true;

        // 링 버퍼에 사실만 적어 둔다. 여기서 하는 일은 원자적 인덱스 증가와 POD 대입뿐이라
        // 마이크로초 단위이고, 어떤 락도 잡지 않으므로 어느 스레드 문맥에서 불려도 안전하다.
        const std::uint32_t index = g_exceptionWriteIndex.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t drained = g_exceptionDrainIndex.load(std::memory_order_relaxed);
        if (index - drained < kExceptionRingSize)
        {
            ExceptionRecordSlot& slot = g_exceptionRing[index % kExceptionRingSize];
            slot.code = code;
            slot.threadId = GetCurrentThreadId();
            slot.faultAddress = reinterpret_cast<std::uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress);
            slot.hasAccessInfo = code == EXCEPTION_ACCESS_VIOLATION &&
                                 exceptionInfo->ExceptionRecord->NumberParameters >= 2;
            slot.accessType = slot.hasAccessInfo ? exceptionInfo->ExceptionRecord->ExceptionInformation[0] : 0;
            slot.targetAddress = slot.hasAccessInfo
                                     ? static_cast<std::uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionInformation[1])
                                     : 0;
        }
        else
        {
            g_exceptionDropped.fetch_add(1, std::memory_order_relaxed);
        }

        t_insideVeh = false;

        // 관측만 하고 아무것도 처리하지 않는다. SEH와 상위 필터가 그대로 이어받는다.
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

        // VEH는 기본으로 끈다. 관측만 하는 장치인데 프로세스 전체의 예외 경로에 끼어들기 때문에,
        // 켜 두는 것 자체가 게임 동작을 바꿀 수 있는 변수다. 필요할 때만 CBPK_VEH=1로 켠다.
        // 마지막(우선순위 0)에 등록해 게임/RED4ext/CET의 핸들러가 먼저 처리할 기회를 갖게 한다.
        // 예전 구현은 우선순위 1로 모든 핸들러보다 앞에 끼어들었다.
        wchar_t vehSetting[8] = {};
        const DWORD vehSettingLength = GetEnvironmentVariableW(L"CBPK_VEH", vehSetting, 8);
        const bool vehRequested = vehSettingLength > 0 && vehSettingLength < 8 && vehSetting[0] == L'1';
        if (vehRequested)
            g_vehHandle = AddVectoredExceptionHandler(0, VectoredExceptionHandler);

        Log("============================================================");
        Log("diagnostics initialized; module=%p veh=%s", module,
            !vehRequested ? "disabled (set CBPK_VEH=1 to enable)" : (g_vehHandle ? "active" : "failed"));
    }

    void Shutdown()
    {
        DrainExceptionLog();
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

    void DrainExceptionLog()
    {
        std::uint32_t drained = g_exceptionDrainIndex.load(std::memory_order_relaxed);
        const std::uint32_t written = g_exceptionWriteIndex.load(std::memory_order_acquire);
        if (drained == written)
            return;

        // 링을 덮어썼다면 가장 오래된 것부터 버리고 살아 있는 구간만 읽는다.
        if (written - drained > kExceptionRingSize)
            drained = written - kExceptionRingSize;

        for (; drained != written; ++drained)
        {
            const ExceptionRecordSlot slot = g_exceptionRing[drained % kExceptionRingSize];

            // 모듈 조회는 여기서 한다. 로더 락을 잡을 수 있는 호출이라 예외 문맥에서는 절대 부르면
            // 안 되지만, 평범한 틱 문맥인 여기서는 안전하다.
            char modName[MAX_PATH] = "<unknown>";
            std::uintptr_t modOffset = slot.faultAddress;
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(slot.faultAddress), &module) &&
                module)
            {
                modOffset = slot.faultAddress - reinterpret_cast<std::uintptr_t>(module);
                char fullPath[MAX_PATH] = {};
                if (GetModuleFileNameA(module, fullPath, MAX_PATH))
                {
                    const char* slash = strrchr(fullPath, '\\');
                    if (!slash)
                        slash = strrchr(fullPath, '/');
                    strncpy_s(modName, slash ? slash + 1 : fullPath, _TRUNCATE);
                }
            }

            if (slot.hasAccessInfo)
            {
                const char* opName = slot.accessType == 0   ? "READ"
                                     : slot.accessType == 1 ? "WRITE"
                                     : slot.accessType == 8 ? "EXECUTE(DEP)"
                                                            : "UNKNOWN";
                Log("[VEH] 0x%08lX (%s) tid=%lu at %p (%s+0x%llX) %s target=%p", slot.code,
                    ExceptionCodeToString(slot.code), slot.threadId,
                    reinterpret_cast<void*>(slot.faultAddress), modName,
                    static_cast<unsigned long long>(modOffset), opName,
                    reinterpret_cast<void*>(slot.targetAddress));
            }
            else
            {
                Log("[VEH] 0x%08lX (%s) tid=%lu at %p (%s+0x%llX)", slot.code,
                    ExceptionCodeToString(slot.code), slot.threadId,
                    reinterpret_cast<void*>(slot.faultAddress), modName,
                    static_cast<unsigned long long>(modOffset));
            }
        }

        g_exceptionDrainIndex.store(drained, std::memory_order_release);

        const std::uint32_t dropped = g_exceptionDropped.exchange(0, std::memory_order_relaxed);
        if (dropped != 0)
            Log("[VEH] %lu exception records dropped: ring full", static_cast<unsigned long>(dropped));
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
