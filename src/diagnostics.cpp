#include "diagnostics.h"

#include "profiling.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <cstdint>
#include <DbgHelp.h>
#include <tlhelp32.h>

namespace
{
    SRWLOCK g_logLock = SRWLOCK_INIT;
    HANDLE g_logFile = INVALID_HANDLE_VALUE;
    wchar_t g_logPath[MAX_PATH] = {};
    wchar_t g_baseDir[MAX_PATH] = {};
    PVOID g_vehHandle = nullptr;
    std::atomic<bool> g_dumpWritten{false};

    // ---- 치명적 폴트 직기록 경로 --------------------------------------------
    // 평시 로그(Log)는 링에 넣고 writer 스레드가 1초 cadence로 쓴다. 그것이 바로 프로세스를 죽이는
    // 폴트를 기록하지 못하는 이유다: 링은 다음 메인 틱에서야 비워지는데 그 틱이 오지 않는다.
    // 2026-08-28 15:46:10 크래시가 정확히 그 모양이었다. VEH 기록 0건, WER 덤프 없음, 남은 것은
    // CDPR 자체 post-mortem뿐이었고 우리 쪽 증거는 통째로 비어 있었다.
    //
    // 그래서 치명적 폴트만은 링을 거치지 않고 전용 핸들로 곧바로 내보낸다. 이 경로의 설계 목표는
    // "로거 자신이 크래시나 프리징의 원인이 되지 않는 것" 하나뿐이고, 그 대가로 기록 품질은 양보했다.
    //   - 핸들러 안에서 CRT도, 할당도, 로더 락을 잡을 수 있는 API도 부르지 않는다. 포맷터는 손으로 썼다.
    //     부르는 WinAPI는 GetSystemTimeAsFileTime / GetCurrentThreadId / GetCurrentProcessId / WriteFile 넷뿐이다.
    //   - 파일 핸들, 모듈 테이블, 타임존 보정값은 Initialize에서 미리 준비한다.
    //   - 프로세스 수명 전체에 쓰기 예산을 둔다(kFatalRecordBudget). ACCESS_VIOLATION은 게임이 엔티티
    //     스트리밍 중 정상적으로 내고 자체 SEH로 복구하기도 한다. 예산이 없으면 이 코드가 매 프레임
    //     디스크를 두드리는, 바로 그 프리징 경로가 된다.
    //   - 스택 버퍼 대신 정적 버퍼를 쓴다(EXCEPTION_STACK_OVERFLOW에서는 남은 스택이 한 페이지뿐이다).
    //     상호배제는 try-lock 하나로 끝내고 잡지 못하면 포기한다. 어떤 경우에도 대기하지 않는다.
    //   - 전 구간을 __try로 감싸고, 코어 레코드와 스택 스캔을 따로 써서 스캔이 폴트나도 코어는 남긴다.
    constexpr std::size_t kFatalRecordCapacity = 4096;
    constexpr int kFatalRecordBudget = 16;
    constexpr std::size_t kFatalModuleSlots = 256;
    constexpr std::size_t kFatalStackScanSlots = 96;
    constexpr int kFatalStackScanHits = 12;
    constexpr char kFatalHexDigits[] = "0123456789ABCDEF";

    // Initialize에서 한 번 채우고 그 뒤로는 읽기 전용이다. 핸들러가 주소를 모듈 이름으로 바꾸는 데
    // 쓴다. 그 순간에 GetModuleHandleEx를 부르면 로더 락이라 그럴 수 없기 때문이다.
    struct FatalModuleRange
    {
        std::uintptr_t base;
        std::uintptr_t end;
        char name[64];
    };

    FatalModuleRange g_fatalModules[kFatalModuleSlots]{};
    std::atomic<std::size_t> g_fatalModuleCount{0};
    std::uintptr_t g_selfModuleBase = 0;
    std::uintptr_t g_selfModuleEnd = 0;

    HANDLE g_fatalFile = INVALID_HANDLE_VALUE;
    wchar_t g_fatalPath[MAX_PATH]{};
    std::atomic<bool> g_fatalEnabled{false};
    std::atomic<int> g_fatalBudget{kFatalRecordBudget};
    volatile LONG g_fatalBufferGate = 0;
    char g_fatalBuffer[kFatalRecordCapacity]{};
    // UTC를 로컬로 옮기는 보정값. 핸들러에서 GetLocalTime을 부를 수 없어서(타임존 캐시/레지스트리를
    // 만질 수 있다) 미리 재 둔다. 세션 도중 DST가 바뀌면 한 시간 어긋나므로 같은 줄에 원본 FILETIME도
    // 함께 남긴다.
    std::int64_t g_localTimeBias100ns = 0;

    LPTOP_LEVEL_EXCEPTION_FILTER g_previousUnhandledFilter = nullptr;
    std::atomic<bool> g_unhandledFilterInstalled{false};

    // ── 비동기 로그 큐 ──────────────────────────────────────────────────────────────
    // 로그를 부르는 쪽은 Present 스레드와 게임 메인 틱이다. 그 스레드에서 디스크를 만지면 프레임이
    // 그만큼 밀린다. 예전 구현은 한 줄마다 SRW 배타 락 + WriteFile + FlushFileBuffers를 했다.
    // FlushFileBuffers는 파일 시스템 캐시를 물리 디스크까지 밀어내는 동기 호출이라 기계식 HDD에서는
    // 한 번에 수 ms가 든다. 오버레이 오류 경로처럼 매 프레임 찍히는 줄이 하나라도 생기면 그것만으로
    // 게임이 멈춘다.
    //
    // 지금은 호출자가 스택에서 포맷해 링 버퍼에 넣고 즉시 돌아간다. 실제 쓰기는 전용 writer 스레드가
    // 배치로 모아서 하고, FlushFileBuffers는 줄 단위가 아니라 1초 cadence와 명시적 Flush() 시점에만
    // 부른다. 프로세스가 죽어도 WriteFile까지 끝난 내용은 파일 시스템 캐시에 남으므로 살아남는다.
    // cadence flush가 막는 것은 그보다 위, 머신 전체가 굳어 전원을 내리는 경우다 (이 프로젝트에서
    // 실제로 겪은 시나리오라 유지한다).
    constexpr std::size_t kLogCellCapacity = 1024;
    constexpr std::uint32_t kLogRingSize = 512;  // 2의 거듭제곱이어야 한다.
    constexpr std::uint32_t kLogRingMask = kLogRingSize - 1;
    constexpr DWORD kWriterIdleWaitMs = 1000;
    constexpr ULONGLONG kFlushIntervalMs = 1000;
    constexpr DWORD kWriterJoinTimeoutMs = 5000;
    constexpr DWORD kFlushWaitTimeoutMs = 2000;

    struct LogCell
    {
        std::atomic<std::uint32_t> sequence;
        std::uint32_t length;
        char text[kLogCellCapacity];
    };

    LogCell g_logRing[kLogRingSize]{};
    std::atomic<std::uint32_t> g_logEnqueuePos{0};
    std::atomic<std::uint32_t> g_logDequeuePos{0};
    std::atomic<std::uint32_t> g_logDropped{0};

    HANDLE g_writerThread = nullptr;
    HANDLE g_logDataEvent = nullptr;    // auto-reset: 큐에 뭔가 들어왔다.
    HANDLE g_flushDoneEvent = nullptr;  // auto-reset: 요청받은 flush가 끝났다.
    std::atomic<bool> g_writerRunning{false};
    std::atomic<bool> g_writerStopping{false};
    std::atomic<bool> g_flushRequested{false};

    // 진단 로그 자체의 마스터 스위치. CBPK_LOG=0이면 Log()가 원자 로드 하나만 하고 돌아간다.
    std::atomic<bool> g_loggingEnabled{true};
    // OutputDebugStringA는 디버거가 없어도 매번 SEH 예외를 일으킨다(DBG_PRINTEXCEPTION_C). 즉 줄마다
    // 예외 디스패치가 돌고, CBPK_VEH=1이면 우리 VEH까지 그 경로에 얹힌다. DebugView를 실제로 볼 때만
    // CBPK_DBGOUT=1로 켠다. 켜더라도 호출은 writer 스레드에서만 일어난다.
    std::atomic<bool> g_debugOutputEnabled{false};

    // writer가 없을 때(초기화 전, 기동 실패, 종료 후) 쓰는 동기 경로.
    void WriteLineSync(const char* line, std::size_t length)
    {
        if (g_debugOutputEnabled.load(std::memory_order_relaxed))
            OutputDebugStringA(line);

        AcquireSRWLockExclusive(&g_logLock);
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(g_logFile, line, static_cast<DWORD>(length), &written, nullptr);
            FlushFileBuffers(g_logFile);
        }
        ReleaseSRWLockExclusive(&g_logLock);
    }

    // Vyukov 방식 bounded MPMC 큐. 생산자는 여러 게임 스레드, 소비자는 writer 하나다. 락도, 할당도,
    // 시스템 콜도 없다.
    bool EnqueueLine(const char* line, std::size_t length)
    {
        if (length >= kLogCellCapacity)
            length = kLogCellCapacity - 1;

        std::uint32_t pos = g_logEnqueuePos.load(std::memory_order_relaxed);
        LogCell* cell = nullptr;
        for (;;)
        {
            cell = &g_logRing[pos & kLogRingMask];
            const std::uint32_t sequence = cell->sequence.load(std::memory_order_acquire);
            const std::int32_t difference =
                static_cast<std::int32_t>(sequence) - static_cast<std::int32_t>(pos);
            if (difference == 0)
            {
                if (g_logEnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (difference < 0)
            {
                // 링이 꽉 찼다. 게임 스레드를 세우느니 버리고, 몇 줄을 버렸는지만 남긴다.
                g_logDropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            else
            {
                pos = g_logEnqueuePos.load(std::memory_order_relaxed);
            }
        }

        memcpy(cell->text, line, length);
        cell->text[length] = 0;
        cell->length = static_cast<std::uint32_t>(length);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool DequeueLine(char* out, std::size_t capacity, std::size_t& outLength)
    {
        std::uint32_t pos = g_logDequeuePos.load(std::memory_order_relaxed);
        LogCell* cell = nullptr;
        for (;;)
        {
            cell = &g_logRing[pos & kLogRingMask];
            const std::uint32_t sequence = cell->sequence.load(std::memory_order_acquire);
            const std::int32_t difference =
                static_cast<std::int32_t>(sequence) - static_cast<std::int32_t>(pos + 1);
            if (difference == 0)
            {
                if (g_logDequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (difference < 0)
            {
                return false;  // 비었다.
            }
            else
            {
                pos = g_logDequeuePos.load(std::memory_order_relaxed);
            }
        }

        outLength = cell->length;
        if (outLength >= capacity)
            outLength = capacity - 1;
        memcpy(out, cell->text, outLength);
        out[outLength] = 0;
        cell->sequence.store(pos + kLogRingMask + 1, std::memory_order_release);
        return true;
    }

    void WriteBatch(const char* data, std::size_t length)
    {
        if (length == 0 || g_logFile == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        WriteFile(g_logFile, data, static_cast<DWORD>(length), &written, nullptr);
    }

    // 큐를 비우고 디스크에 쓴다. 여러 줄을 하나의 WriteFile로 합쳐서 줄 수가 아니라 배치 수만큼만
    // I/O가 일어나게 한다. 반환값은 실제로 쓴 것이 있었는지 여부다.
    bool DrainQueueToDisk()
    {
        constexpr std::size_t kBatchCapacity = 64 * 1024;
        static char batch[kBatchCapacity];
        char line[kLogCellCapacity];
        std::size_t lineLength = 0;
        std::size_t used = 0;
        bool wroteAnything = false;
        const bool debugOutput = g_debugOutputEnabled.load(std::memory_order_relaxed);

        while (DequeueLine(line, sizeof(line), lineLength))
        {
            if (debugOutput)
                OutputDebugStringA(line);

            if (used + lineLength > kBatchCapacity)
            {
                WriteBatch(batch, used);
                wroteAnything = wroteAnything || used > 0;
                used = 0;
            }

            memcpy(batch + used, line, lineLength);
            used += lineLength;
        }

        if (used > 0)
        {
            WriteBatch(batch, used);
            wroteAnything = true;
        }

        const std::uint32_t dropped = g_logDropped.exchange(0, std::memory_order_relaxed);
        if (dropped != 0)
        {
            char notice[128]{};
            const int length = snprintf(notice, sizeof(notice), "[log] %lu lines dropped: async queue full\r\n",
                                        static_cast<unsigned long>(dropped));
            if (length > 0)
            {
                WriteBatch(notice, static_cast<std::size_t>(length));
                wroteAnything = true;
            }
        }

        return wroteAnything;
    }

    DWORD WINAPI LogWriterThread(LPVOID)
    {
        ULONGLONG lastFlush = GetTickCount64();
        bool dirty = false;

        for (;;)
        {
            // 정지 플래그는 드레인보다 먼저 읽는다. 이 순서면 플래그를 세운 뒤 들어온 줄까지 이번
            // 드레인이 가져가므로 마지막 줄을 흘리지 않는다.
            const bool stopping = g_writerStopping.load(std::memory_order_acquire);
            if (DrainQueueToDisk())
                dirty = true;

            const bool flushRequested = g_flushRequested.exchange(false, std::memory_order_acq_rel);
            const ULONGLONG now = GetTickCount64();
            if (dirty && (flushRequested || stopping || now - lastFlush >= kFlushIntervalMs))
            {
                if (g_logFile != INVALID_HANDLE_VALUE)
                    FlushFileBuffers(g_logFile);
                lastFlush = now;
                dirty = false;
            }

            if (flushRequested && g_flushDoneEvent)
                SetEvent(g_flushDoneEvent);

            if (stopping)
                break;

            WaitForSingleObject(g_logDataEvent, kWriterIdleWaitMs);
        }

        return 0;
    }

    void StartLogWriter()
    {
        for (std::uint32_t index = 0; index < kLogRingSize; ++index)
            g_logRing[index].sequence.store(index, std::memory_order_relaxed);
        g_logEnqueuePos.store(0, std::memory_order_relaxed);
        g_logDequeuePos.store(0, std::memory_order_relaxed);

        g_logDataEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_flushDoneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_logDataEvent || !g_flushDoneEvent)
            return;

        g_writerStopping.store(false, std::memory_order_relaxed);
        g_writerThread = CreateThread(nullptr, 0, LogWriterThread, nullptr, 0, nullptr);
        if (g_writerThread)
            g_writerRunning.store(true, std::memory_order_release);
    }

    // writer는 반드시 FreeLibraryAndExitThread 이전에 끝나야 한다. 언로드 후에도 살아 있으면 이미
    // 언맵된 코드를 실행하게 된다. 그래서 여기서만은 실제로 join한다 (상한 5초, writer가 블록되는
    // 지점은 최대 1초 대기와 WriteFile뿐이라 실전에서 걸릴 일이 없다).
    void StopLogWriter()
    {
        if (!g_writerRunning.exchange(false, std::memory_order_acq_rel))
            return;

        g_writerStopping.store(true, std::memory_order_release);
        if (g_logDataEvent)
            SetEvent(g_logDataEvent);
        bool joined = true;
        if (g_writerThread)
        {
            joined = WaitForSingleObject(g_writerThread, kWriterJoinTimeoutMs) == WAIT_OBJECT_0;
            CloseHandle(g_writerThread);
            g_writerThread = nullptr;
        }
        if (g_logDataEvent)
        {
            CloseHandle(g_logDataEvent);
            g_logDataEvent = nullptr;
        }
        if (g_flushDoneEvent)
        {
            CloseHandle(g_flushDoneEvent);
            g_flushDoneEvent = nullptr;
        }

        // writer가 시간 안에 못 빠져나왔더라도 남은 줄은 잃지 않는다.
        DrainQueueToDisk();
        if (!joined)
        {
            // 여기까지 왔다는 것은 writer가 5초 안에 못 빠져나왔다는 뜻이다. 그 다음은
            // FreeLibraryAndExitThread이므로 언맵된 코드를 실행하게 된다. 실제로 걸릴 일은 없지만
            // 걸렸다면 그 크래시의 원인을 알 수 있게 흔적을 남긴다.
            const char notice[] = "[log] writer thread did not exit within the join timeout\r\n";
            DWORD written = 0;
            if (g_logFile != INVALID_HANDLE_VALUE)
                WriteFile(g_logFile, notice, static_cast<DWORD>(sizeof(notice) - 1), &written, nullptr);
        }
        if (g_logFile != INVALID_HANDLE_VALUE)
            FlushFileBuffers(g_logFile);
    }

    wchar_t g_configPath[MAX_PATH] = {};

    // Config::Initialize가 쓰는 것과 같은 파일이다. 진단 토글은 Config보다 먼저 필요하므로(로그가
    // 그 전에 열려야 한다) 여기서 직접 읽는다.
    void ResolveConfigPath()
    {
        wchar_t localAppData[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (length == 0 || length >= MAX_PATH - 24)
            return;
        if (swprintf_s(g_configPath, L"%ls\\cbpk\\config.ini", localAppData) < 0)
            g_configPath[0] = L'\0';
    }

    // 토글 하나를 푸는 순서: 환경 변수가 있으면 그것, 없으면 config.ini의 [diagnostics] 값,
    // 그것도 없으면 컴파일 기본값. 환경 변수를 위에 두는 이유는 게임을 그 변수와 함께 띄운
    // 세션에서만 일회성으로 덮어쓰고 싶은 경우가 있기 때문이고, ini를 두는 이유는 이미 떠 있는
    // 게임에 나중에 주입할 때 환경 변수를 심을 방법이 없기 때문이다.
    //
    // ini에 키가 없으면 기본값을 한 번 써 넣어서 파일만 봐도 어떤 스위치가 있는지 알 수 있게 한다.
    bool ReadToggle(const wchar_t* environmentName, const wchar_t* iniKey, bool fallback)
    {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(environmentName, value, 8);
        if (length > 0 && length < 8)
            return value[0] == L'1';

        if (g_configPath[0] == L'\0')
            return fallback;

        constexpr UINT kMissing = 0xFFFF;
        const UINT stored = GetPrivateProfileIntW(L"diagnostics", iniKey, kMissing, g_configPath);
        if (stored != kMissing)
            return stored != 0;

        WritePrivateProfileStringW(L"diagnostics", iniKey, fallback ? L"1" : L"0", g_configPath);
        return fallback;
    }

    void EnsureTrailingSeparator(wchar_t* path, std::size_t capacity)
    {
        const std::size_t length = wcsnlen(path, capacity);
        if (length == 0 || length + 2 >= capacity)
            return;
        if (path[length - 1] != L'\\' && path[length - 1] != L'/')
        {
            path[length] = L'\\';
            path[length + 1] = L'\0';
        }
    }

    // 로그와 덤프가 놓일 디렉터리를 고른다. 우선순위:
    //   1. CBPK_LOG_DIR (명시적 지정)
    //   2. %LOCALAPPDATA%\cp2077_trainer\
    //   3. DLL이 있는 디렉터리 (예전 동작)
    // 2번이 기본인 이유는 두 가지다. 개발 트리가 있는 E:는 기계식 HDD이고 %LOCALAPPDATA%는 NVMe라
    // writer 스레드의 flush가 훨씬 싸다. 그리고 로그와 덤프가 빌드 산출물 디렉터리 밖으로 나가므로
    // 클린 빌드에 쓸려나가지 않는다.
    void ResolveBaseDirectory(HMODULE module, wchar_t* moduleDir, std::size_t capacity)
    {
        moduleDir[0] = L'\0';
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, modulePath, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            wchar_t* fileName = wcsrchr(modulePath, L'\\');
            if (fileName)
                *(fileName + 1) = L'\0';
            wcscpy_s(moduleDir, capacity, modulePath);
        }

        wchar_t overrideDir[MAX_PATH]{};
        const DWORD overrideDirLength = GetEnvironmentVariableW(L"CBPK_LOG_DIR", overrideDir, MAX_PATH);
        if (overrideDirLength > 0 && overrideDirLength < MAX_PATH)
        {
            EnsureTrailingSeparator(overrideDir, MAX_PATH);
            CreateDirectoryW(overrideDir, nullptr);
            wcscpy_s(g_baseDir, overrideDir);
            return;
        }

        wchar_t localAppData[MAX_PATH]{};
        const DWORD localLength = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (localLength > 0 && localLength < MAX_PATH - 20)
        {
            EnsureTrailingSeparator(localAppData, MAX_PATH);
            wcscat_s(localAppData, L"cp2077_trainer\\");
            if (CreateDirectoryW(localAppData, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
            {
                wcscpy_s(g_baseDir, localAppData);
                return;
            }
        }

        wcscpy_s(g_baseDir, moduleDir);
    }

    // 로그가 무한히 자라지 않게 한 번만 굴린다. 예전 세션의 로그는 9.7 MB까지 갔었다.
    void RollLogIfLarge(const wchar_t* path)
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            return;

        const ULONGLONG size =
            (static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        if (size < 32ull * 1024ull * 1024ull)
            return;

        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t rolled[MAX_PATH]{};
        swprintf_s(rolled, L"%ls.%04u%02u%02u_%02u%02u%02u.old", path, time.wYear, time.wMonth, time.wDay,
                   time.wHour, time.wMinute, time.wSecond);
        MoveFileW(path, rolled);
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
            Diagnostics::Flush();
            return true;
        }
        else
        {
            Diagnostics::Log("MiniDumpWriteDump failed [reason=%s]: error=%lu", reason, GetLastError());
            Diagnostics::Flush();
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

    // ---- 손으로 쓴 포맷터 -----------------------------------------------------
    // snprintf 계열은 로케일 락을 잡고 구현에 따라 할당도 한다. 예외 문맥에서는 둘 다 금지라
    // 필요한 최소한만 직접 만든다. 전부 순수 계산이고 어떤 전역 상태도 만지지 않는다.
    struct FatalWriter
    {
        char* buffer;
        std::size_t capacity;
        std::size_t length;
    };

    void PutChar(FatalWriter& writer, char value)
    {
        if (writer.length + 1 < writer.capacity)
            writer.buffer[writer.length++] = value;
    }

    void PutText(FatalWriter& writer, const char* text)
    {
        if (!text)
            return;
        for (std::size_t i = 0; i < 512 && text[i]; ++i)
            PutChar(writer, text[i]);
    }

    void PutHex(FatalWriter& writer, std::uint64_t value, int digits)
    {
        if (digits < 1)
            digits = 1;
        if (digits > 16)
            digits = 16;
        for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
            PutChar(writer, kFatalHexDigits[(value >> shift) & 0xF]);
    }

    void PutHexTrim(FatalWriter& writer, std::uint64_t value)
    {
        int digits = 1;
        for (std::uint64_t probe = value >> 4; probe != 0; probe >>= 4)
            ++digits;
        PutHex(writer, value, digits);
    }

    void PutDec(FatalWriter& writer, std::uint64_t value, int minDigits)
    {
        char digits[24];
        int count = 0;
        do
        {
            digits[count++] = static_cast<char>('0' + static_cast<int>(value % 10));
            value /= 10;
        } while (value != 0 && count < 24);

        for (int pad = count; pad < minDigits; ++pad)
            PutChar(writer, '0');
        while (count > 0)
            PutChar(writer, digits[--count]);
    }

    // Initialize가 떠 둔 스냅샷만 본다. 정적 배열 선형 스캔이라 락도 시스템 콜도 없다.
    const FatalModuleRange* FindFatalModule(std::uintptr_t address)
    {
        if (address == 0)
            return nullptr;
        const std::size_t count = g_fatalModuleCount.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (address >= g_fatalModules[i].base && address < g_fatalModules[i].end)
                return &g_fatalModules[i];
        }
        return nullptr;
    }

    void PutAddress(FatalWriter& writer, std::uintptr_t address)
    {
        PutText(writer, "0x");
        PutHex(writer, address, 16);
        if (const FatalModuleRange* range = FindFatalModule(address))
        {
            PutText(writer, " (");
            PutText(writer, range->name);
            PutText(writer, "+0x");
            PutHexTrim(writer, address - range->base);
            PutChar(writer, ')');
        }
    }

    // GetSystemTimeAsFileTime은 KUSER_SHARED_DATA를 읽는 것이 전부라 락도 할당도 없다.
    // GetLocalTime은 그렇지 않으므로 여기서 부르지 않고, 미리 재 둔 보정값을 더한다.
    void PutFatalTimestamp(FatalWriter& writer)
    {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const std::uint64_t utc =
            (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        const std::uint64_t local = utc + static_cast<std::uint64_t>(g_localTimeBias100ns);
        const std::uint64_t msOfDay = (local / 10000ull) % 86400000ull;

        PutDec(writer, msOfDay / 3600000ull, 2);
        PutChar(writer, ':');
        PutDec(writer, (msOfDay / 60000ull) % 60ull, 2);
        PutChar(writer, ':');
        PutDec(writer, (msOfDay / 1000ull) % 60ull, 2);
        PutChar(writer, '.');
        PutDec(writer, msOfDay % 1000ull, 3);
        PutText(writer, " (utc_ft=0x");
        PutHex(writer, utc, 16);
        PutChar(writer, ')');
    }

    void FlushFatalWriter(FatalWriter& writer)
    {
        const HANDLE file = g_fatalFile;
        if (file == INVALID_HANDLE_VALUE || writer.length == 0)
            return;

        // FILE_FLAG_WRITE_THROUGH로 열어 뒀으므로 FlushFileBuffers를 따로 부르지 않는다.
        // 그쪽은 메타데이터까지 밀어내는 훨씬 비싼 호출이다.
        DWORD written = 0;
        WriteFile(file, writer.buffer, static_cast<DWORD>(writer.length), &written, nullptr);
    }

    // ---- 레코드 본문 ---------------------------------------------------------
    void BuildFatalCoreRecord(PEXCEPTION_POINTERS exceptionInfo, const char* origin)
    {
        FatalWriter writer{g_fatalBuffer, kFatalRecordCapacity, 0};

        const EXCEPTION_RECORD* record = exceptionInfo ? exceptionInfo->ExceptionRecord : nullptr;
        const DWORD code = record ? record->ExceptionCode : 0;

        PutText(writer, "[FATAL][");
        PutText(writer, origin);
        PutText(writer, "] ");
        PutFatalTimestamp(writer);
        PutText(writer, " pid=");
        PutDec(writer, GetCurrentProcessId(), 1);
        PutText(writer, " tid=");
        PutDec(writer, GetCurrentThreadId(), 1);
        PutText(writer, "\r\n  code=0x");
        PutHex(writer, code, 8);
        PutChar(writer, ' ');
        PutText(writer, ExceptionCodeToString(code));

        if (record)
        {
            const std::uintptr_t faultAddress = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);

            PutText(writer, "\r\n  at=");
            PutAddress(writer, faultAddress);

            if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
            {
                const ULONG_PTR accessType = record->ExceptionInformation[0];
                PutText(writer, "\r\n  access=");
                PutText(writer, accessType == 0   ? "READ"
                                : accessType == 1 ? "WRITE"
                                : accessType == 8 ? "EXECUTE(DEP)"
                                                  : "UNKNOWN");
                PutText(writer, " target=");
                PutAddress(writer, static_cast<std::uintptr_t>(record->ExceptionInformation[1]));
            }

            // 이 한 줄이 "트레이너가 그랬는가"에 대한 즉답이다. 폴트 주소가 우리 모듈 안이면 우리 코드다.
            const bool inTrainer = g_selfModuleBase != 0 && faultAddress >= g_selfModuleBase &&
                                   faultAddress < g_selfModuleEnd;
            PutText(writer, "\r\n  fault-in-trainer=");
            PutText(writer, inTrainer ? "YES" : "no");
        }

        if (exceptionInfo && exceptionInfo->ContextRecord)
        {
            const CONTEXT* context = exceptionInfo->ContextRecord;
            PutText(writer, "\r\n  rip=0x");
            PutHex(writer, context->Rip, 16);
            PutText(writer, " rsp=0x");
            PutHex(writer, context->Rsp, 16);
            PutText(writer, " rbp=0x");
            PutHex(writer, context->Rbp, 16);
            PutText(writer, "\r\n  rax=0x");
            PutHex(writer, context->Rax, 16);
            PutText(writer, " rbx=0x");
            PutHex(writer, context->Rbx, 16);
            PutText(writer, " rcx=0x");
            PutHex(writer, context->Rcx, 16);
            PutText(writer, " rdx=0x");
            PutHex(writer, context->Rdx, 16);
            PutText(writer, "\r\n  rsi=0x");
            PutHex(writer, context->Rsi, 16);
            PutText(writer, " rdi=0x");
            PutHex(writer, context->Rdi, 16);
            PutText(writer, " r8=0x");
            PutHex(writer, context->R8, 16);
            PutText(writer, " r9=0x");
            PutHex(writer, context->R9, 16);
            PutText(writer, "\r\n  r10=0x");
            PutHex(writer, context->R10, 16);
            PutText(writer, " r11=0x");
            PutHex(writer, context->R11, 16);
            PutText(writer, " r12=0x");
            PutHex(writer, context->R12, 16);
            PutText(writer, " r13=0x");
            PutHex(writer, context->R13, 16);
        }

        PutText(writer, "\r\n");
        FlushFatalWriter(writer);
    }

    // 스택을 되감지 않고 훑기만 한다. RtlLookupFunctionEntry/StackWalk64는 락을 잡을 수 있어서
    // 예외 문맥에서 부를 수 없다. 대신 RSP 위쪽(이미 커밋된 방향)의 값들 중 Initialize에서 떠 둔
    // 모듈 범위 안에 떨어지는 것만 골라 적는다. 리턴 주소가 아닌 값도 섞이지만, "그때 어느 모듈이
    // 콜 스택에 있었는가"는 이것만으로 충분히 읽힌다.
    // 읽기 자체가 폴트날 수 있으므로 루프만 따로 감싼다. 바깥에서 감싸면 폴트가 난 순간 그때까지 모은
    // 줄까지 통째로 날아간다 -- 실제로 첫 테스트에서 스택 꼭대기를 넘어 읽다가 그렇게 잃었다.
    void ScanStackForModules(FatalWriter& writer, const std::uintptr_t* slots, std::size_t count)
    {
        volatile int hits = 0;
        __try
        {
            for (std::size_t i = 0; i < count && hits < kFatalStackScanHits; ++i)
            {
                const std::uintptr_t value = slots[i];
                const FatalModuleRange* range = FindFatalModule(value);
                if (!range)
                    continue;

                PutText(writer, "\r\n    +0x");
                PutHexTrim(writer, i * sizeof(std::uintptr_t));
                PutChar(writer, ' ');
                PutAddress(writer, value);
                ++hits;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            PutText(writer, "\r\n    <stack read faulted here>");
        }

        if (hits == 0)
            PutText(writer, " <no address inside a snapshotted module>");
    }

    void BuildFatalStackScan(PEXCEPTION_POINTERS exceptionInfo)
    {
        if (!exceptionInfo || !exceptionInfo->ContextRecord)
            return;

        const std::uintptr_t stackPointer = exceptionInfo->ContextRecord->Rsp;
        if (stackPointer == 0 || (stackPointer & 7) != 0)
            return;

        // 스레드 스택의 실제 경계를 넘어서 읽지 않는다. GetCurrentThreadStackLimits는 TEB를 읽는 것이
        // 전부라 예외 문맥에서도 안전하고, VEH/UEF 모두 폴트가 난 그 스레드에서 돌기 때문에 값도 맞다.
        ULONG_PTR lowLimit = 0;
        ULONG_PTR highLimit = 0;
        GetCurrentThreadStackLimits(&lowLimit, &highLimit);
        if (stackPointer < lowLimit || stackPointer >= highLimit)
            return;

        std::size_t count = (highLimit - stackPointer) / sizeof(std::uintptr_t);
        if (count > kFatalStackScanSlots)
            count = kFatalStackScanSlots;
        if (count == 0)
            return;

        FatalWriter writer{g_fatalBuffer, kFatalRecordCapacity, 0};
        PutText(writer, "  stack:");
        ScanStackForModules(writer, reinterpret_cast<const std::uintptr_t*>(stackPointer), count);
        PutText(writer, "\r\n");
        FlushFatalWriter(writer);
    }

    // __try를 쓰므로 이 함수에는 소멸자를 가진 객체를 두지 않는다(C2712). 코어와 스택 스캔을 따로
    // 감싸는 이유는, 스캔이 폴트나도 코어 레코드는 이미 디스크에 나가 있게 하려는 것이다.
    void GuardedFatalRecord(PEXCEPTION_POINTERS exceptionInfo, const char* origin)
    {
        __try
        {
            BuildFatalCoreRecord(exceptionInfo, origin);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        __try
        {
            BuildFatalStackScan(exceptionInfo);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // ignoreBudget은 UEF 전용이다. 그 경로는 프로세스당 사실상 한 번뿐인데, 그때 남길 레코드가
    // 앞서 복구된 예외들에게 예산을 다 뺏기면 정작 죽는 순간의 기록이 없어진다.
    void RecordFatalException(PEXCEPTION_POINTERS exceptionInfo, const char* origin, bool ignoreBudget)
    {
        if (!g_fatalEnabled.load(std::memory_order_relaxed) || g_fatalFile == INVALID_HANDLE_VALUE)
            return;

        // 정적 버퍼 하나를 두 스레드가 동시에 쓰지 못하게 막는다. 잡지 못하면 기다리지 않고 포기한다.
        // 크래시 핸들러에서 락을 기다리는 것이야말로 우리가 없애려는 프리징이다.
        if (InterlockedCompareExchange(&g_fatalBufferGate, 1, 0) != 0)
            return;

        // 예산은 게이트를 잡은 뒤에 깎는다. 순서를 뒤집으면 여러 스레드가 동시에 폴트를 낼 때 게이트에서
        // 밀린 쪽이 아무것도 쓰지 않고 예산만 태운다. 8스레드 동시 폴트 테스트에서 실제로 16줄이 아니라
        // 1줄만 남았다.
        if (!ignoreBudget && g_fatalBudget.fetch_sub(1, std::memory_order_relaxed) <= 0)
        {
            // 0 밑으로 계속 내려가지 않게 붙잡아 둔다. 다시 양수가 되는 경로는 없다.
            g_fatalBudget.store(0, std::memory_order_relaxed);
            InterlockedExchange(&g_fatalBufferGate, 0);
            return;
        }

        GuardedFatalRecord(exceptionInfo, origin);

        InterlockedExchange(&g_fatalBufferGate, 0);
    }

    // VEH가 잡는 것은 first-chance라 "게임이 스스로 복구한 예외"와 구분되지 않는다. 여기까지 왔다는
    // 것은 아무도 처리하지 않았다는 뜻이고, 즉 이 예외가 실제로 프로세스를 죽인다는 확정 신호다.
    LONG WINAPI FatalUnhandledExceptionFilter(PEXCEPTION_POINTERS exceptionInfo)
    {
        if (!t_insideVeh)
        {
            t_insideVeh = true;
            RecordFatalException(exceptionInfo, "unhandled", true);
            t_insideVeh = false;
        }

        // 링에 남아 있는 평시 로그를 디스크까지 민다. 마지막 1초가 통째로 사라지던 구멍이 이것이다.
        // 이 경로는 프로세스가 죽는 중이라 기다려도 되고, writer가 이미 죽었더라도 Flush의 타임아웃이
        // 상한을 잡아 준다.
        Diagnostics::Flush();

        // CDPR 자체 post-mortem(CrashInfo.json)이 지금까지 유일한 증거였다. 반드시 이어서 부른다.
        if (g_previousUnhandledFilter)
            return g_previousUnhandledFilter(exceptionInfo);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // ---- 준비 (전부 평범한 문맥에서, Initialize 시점에만) ----------------------
    void ComputeLocalTimeBias()
    {
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        GetSystemTime(&utc);
        GetLocalTime(&local);

        FILETIME utcFileTime{};
        FILETIME localFileTime{};
        if (!SystemTimeToFileTime(&utc, &utcFileTime) || !SystemTimeToFileTime(&local, &localFileTime))
            return;

        const std::uint64_t utcTicks =
            (static_cast<std::uint64_t>(utcFileTime.dwHighDateTime) << 32) | utcFileTime.dwLowDateTime;
        const std::uint64_t localTicks =
            (static_cast<std::uint64_t>(localFileTime.dwHighDateTime) << 32) | localFileTime.dwLowDateTime;
        g_localTimeBias100ns = static_cast<std::int64_t>(localTicks) - static_cast<std::int64_t>(utcTicks);
    }

    // Toolhelp을 쓰는 이유는 kernel32에 있어 추가 링크가 필요 없기 때문이다. 스냅샷은 여기서 한 번만
    // 뜬다. 주입 시점에 이미 대부분의 모듈이 올라와 있고, 그 뒤에 로드된 모듈은 원시 주소로 남는다.
    void SnapshotFatalModules(HMODULE self)
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
            return;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        std::size_t count = 0;

        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (count >= kFatalModuleSlots)
                    break;

                FatalModuleRange& range = g_fatalModules[count];
                range.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                range.end = range.base + entry.modBaseSize;
                if (WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, range.name,
                                        static_cast<int>(sizeof(range.name)), nullptr, nullptr) == 0)
                {
                    range.name[0] = '?';
                    range.name[1] = '\0';
                }
                range.name[sizeof(range.name) - 1] = '\0';

                if (self && reinterpret_cast<HMODULE>(range.base) == self)
                {
                    g_selfModuleBase = range.base;
                    g_selfModuleEnd = range.end;
                }
                ++count;
            } while (Module32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        g_fatalModuleCount.store(count, std::memory_order_release);
    }

    // 세션 헤더 + 모듈 표. 크래시 파일 하나만 보고도 원시 주소를 풀 수 있어야 하므로 파일 안에 같이 둔다.
    // WRITE_THROUGH 핸들이라 줄마다 쓰면 그만큼 동기 I/O가 되니, 한 버퍼에 모아 WriteFile 한 번으로 끝낸다.
    char g_fatalHeaderBuffer[64 * 1024];

    void WriteFatalSessionHeader(HMODULE self)
    {
        if (g_fatalFile == INVALID_HANDLE_VALUE)
            return;

        SYSTEMTIME time{};
        GetLocalTime(&time);

        const std::size_t count = g_fatalModuleCount.load(std::memory_order_acquire);
        int offset = snprintf(g_fatalHeaderBuffer, sizeof(g_fatalHeaderBuffer),
                              "\r\n============================================================\r\n"
                              "[SESSION] %04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu trainer=%p "
                              "range=0x%016llX-0x%016llX\r\n"
                              "[SESSION] modules=%zu (snapshotted at init; anything loaded later shows as a raw address)\r\n",
                              time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                              time.wMilliseconds, GetCurrentProcessId(), static_cast<void*>(self),
                              static_cast<unsigned long long>(g_selfModuleBase),
                              static_cast<unsigned long long>(g_selfModuleEnd), count);
        if (offset < 0)
            return;

        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t remaining = sizeof(g_fatalHeaderBuffer) - static_cast<std::size_t>(offset);
            if (remaining < 128)
                break;
            const int written = snprintf(g_fatalHeaderBuffer + offset, remaining, "  0x%016llX-0x%016llX %s\r\n",
                                         static_cast<unsigned long long>(g_fatalModules[i].base),
                                         static_cast<unsigned long long>(g_fatalModules[i].end),
                                         g_fatalModules[i].name);
            if (written < 0 || static_cast<std::size_t>(written) >= remaining)
                break;
            offset += written;
        }

        DWORD writtenBytes = 0;
        WriteFile(g_fatalFile, g_fatalHeaderBuffer, static_cast<DWORD>(offset), &writtenBytes, nullptr);
    }

    void OpenFatalSink(HMODULE self)
    {
        wcscpy_s(g_fatalPath, g_baseDir);
        wcscat_s(g_fatalPath, L"cp2077_fatal.log");
        RollLogIfLarge(g_fatalPath);

        // WRITE_THROUGH: 이 파일에 쓰는 시점은 프로세스가 죽는 중이라 파일 시스템 캐시에만 남겨서는
        // 안심할 수 없다. 대신 쓰기 횟수를 예산으로 묶어 뒀기 때문에 비용의 상한이 정해져 있다.
        g_fatalFile = CreateFileW(g_fatalPath, FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (g_fatalFile == INVALID_HANDLE_VALUE)
            return;

        ComputeLocalTimeBias();
        SnapshotFatalModules(self);
        WriteFatalSessionHeader(self);
        g_fatalEnabled.store(true, std::memory_order_release);
    }

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

        // 링은 다음 메인 틱에서야 비워진다. 그 틱이 오지 않는 경우가 바로 우리가 놓쳤던 크래시라,
        // 여기서 한 번 더 디스크에 직접 남긴다. 예산과 게이트가 비용의 상한을 잡는다.
        RecordFatalException(exceptionInfo, "veh", false);

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
        ResolveConfigPath();
        g_loggingEnabled.store(ReadToggle(L"CBPK_LOG", L"logging", true), std::memory_order_relaxed);
        g_debugOutputEnabled.store(ReadToggle(L"CBPK_DBGOUT", L"debug_output", false), std::memory_order_relaxed);
        const bool profilingEnabled = ReadToggle(L"CBPK_PROFILE", L"profiling", true);
        Profile::SetEnabled(profilingEnabled);

        wchar_t moduleDir[MAX_PATH]{};
        ResolveBaseDirectory(module, moduleDir, MAX_PATH);

        wcscpy_s(g_logPath, g_baseDir);
        wcscat_s(g_logPath, L"cp2077_trainer.log");
        RollLogIfLarge(g_logPath);

        AcquireSRWLockExclusive(&g_logLock);
        g_logFile = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ReleaseSRWLockExclusive(&g_logLock);

        // 고른 디렉터리에 못 쓰면 (권한, 경로 오타 등) 조용히 로그를 잃지 말고 예전 위치로 되돌아간다.
        if (g_logFile == INVALID_HANDLE_VALUE && moduleDir[0] != L'\0' && _wcsicmp(g_baseDir, moduleDir) != 0)
        {
            wcscpy_s(g_baseDir, moduleDir);
            wcscpy_s(g_logPath, g_baseDir);
            wcscat_s(g_logPath, L"cp2077_trainer.log");
            AcquireSRWLockExclusive(&g_logLock);
            g_logFile = CreateFileW(g_logPath, FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            ReleaseSRWLockExclusive(&g_logLock);
        }

        StartLogWriter();

        // VEH는 기본으로 끈다. 관측만 하는 장치인데 프로세스 전체의 예외 경로에 끼어들기 때문에,
        // 켜 두는 것 자체가 게임 동작을 바꿀 수 있는 변수다. 필요할 때만 CBPK_VEH=1로 켠다.
        // 마지막(우선순위 0)에 등록해 게임/RED4ext/CET의 핸들러가 먼저 처리할 기회를 갖게 한다.
        // 예전 구현은 우선순위 1로 모든 핸들러보다 앞에 끼어들었다.
        // 치명적 폴트 기록은 기본으로 켠다. 관측용 VEH(아래)와 달리 이쪽은 평시에 아무 일도 하지
        // 않는다 -- 실제로 쓰기가 일어나는 것은 프로세스를 죽일 만한 폴트가 났을 때뿐이고, 그마저
        // 예산으로 묶여 있다. 끄려면 CBPK_FATAL=0 또는 ini의 fatal_log=0.
        const bool fatalRequested = ReadToggle(L"CBPK_FATAL", L"fatal_log", true);
        if (fatalRequested)
            OpenFatalSink(module);

        const bool fatalActive = g_fatalEnabled.load(std::memory_order_acquire);
        if (fatalActive)
        {
            // 마지막 기회 필터. VEH와 달리 여기까지 온 예외는 실제로 프로세스를 죽인다는 확정 신호다.
            // 이전 필터(= CDPR post-mortem)를 반드시 붙잡아 두고 이어서 부른다.
            g_previousUnhandledFilter = SetUnhandledExceptionFilter(FatalUnhandledExceptionFilter);
            g_unhandledFilterInstalled.store(true, std::memory_order_release);
        }

        // VEH는 기본으로 끈다(관측 용도). 다만 치명적 폴트 직기록이 켜져 있으면 first-chance를 볼
        // 유일한 통로라서 같이 등록한다.
        const bool vehRequested = ReadToggle(L"CBPK_VEH", L"veh", false);
        if (vehRequested || fatalActive)
            g_vehHandle = AddVectoredExceptionHandler(0, VectoredExceptionHandler);

        Log("============================================================");
        Log("diagnostics initialized; module=%p veh=%s", module,
            g_vehHandle       ? (vehRequested ? "active" : "active (registered for the fatal sink)")
            : vehRequested    ? "failed"
            : fatalActive     ? "not registered"
                              : "disabled (set CBPK_VEH=1 to enable)");
        Log("log sink: path=%ls async=%s dbgout=%s (CBPK_LOG_DIR overrides the directory)", g_logPath,
            g_writerRunning.load(std::memory_order_acquire) ? "on" : "off (writer thread unavailable)",
            g_debugOutputEnabled.load(std::memory_order_relaxed) ? "on" : "off");
        Log("fatal sink: %s path=%ls budget=%d modules=%zu unhandled-filter=%s",
            fatalActive ? "armed" : (fatalRequested ? "requested but the file could not be opened" : "disabled"),
            g_fatalPath[0] != L'\0' ? g_fatalPath : L"<unavailable>", kFatalRecordBudget,
            g_fatalModuleCount.load(std::memory_order_acquire),
            g_unhandledFilterInstalled.load(std::memory_order_acquire) ? "chained" : "not installed");
        Log("diagnostics toggles: logging=%d profiling=%d veh=%d dbgout=%d fatal=%d config=%ls",
            g_loggingEnabled.load(std::memory_order_relaxed) ? 1 : 0, profilingEnabled ? 1 : 0,
            vehRequested ? 1 : 0, g_debugOutputEnabled.load(std::memory_order_relaxed) ? 1 : 0,
            fatalRequested ? 1 : 0, g_configPath[0] != L'\0' ? g_configPath : L"<unavailable>");
        Flush();
    }

    void Shutdown()
    {
        DrainExceptionLog();
        Log("diagnostics shutdown");

        // 순서가 중요하다: 먼저 무장을 풀어 새 레코드가 들어오지 않게 하고, 핸들러를 뗀 다음에야
        // 핸들을 닫는다. 반대로 하면 닫힌 핸들에 쓰는(최악의 경우 재사용된 핸들에 쓰는) 창이 열린다.
        g_fatalEnabled.store(false, std::memory_order_release);

        if (g_vehHandle)
        {
            RemoveVectoredExceptionHandler(g_vehHandle);
            g_vehHandle = nullptr;
        }

        if (g_unhandledFilterInstalled.exchange(false, std::memory_order_acq_rel))
        {
            // 우리 위에 다른 필터가 올라와 있으면 그것을 밀어내지 않는다.
            const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(g_previousUnhandledFilter);
            if (current != FatalUnhandledExceptionFilter)
                SetUnhandledExceptionFilter(current);
            g_previousUnhandledFilter = nullptr;
        }

        if (g_fatalFile != INVALID_HANDLE_VALUE)
        {
            // 정상 종료 표시. 이 줄이 없는 세션 헤더는 "예외 하나 없이 사라졌다"는 뜻이고, 그것 자체가
            // TerminateProcess/행/전원 차단을 가리키는 신호다.
            SYSTEMTIME time{};
            GetLocalTime(&time);
            char closing[128]{};
            const int length = snprintf(closing, sizeof(closing), "[SESSION] closed cleanly at %02u:%02u:%02u.%03u\r\n",
                                        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
            if (length > 0)
            {
                DWORD written = 0;
                WriteFile(g_fatalFile, closing, static_cast<DWORD>(length), &written, nullptr);
            }

            const HANDLE fatalFile = g_fatalFile;
            g_fatalFile = INVALID_HANDLE_VALUE;
            CloseHandle(fatalFile);
        }

        StopLogWriter();

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
        if (!g_loggingEnabled.load(std::memory_order_relaxed))
            return;

        SYSTEMTIME time{};
        GetLocalTime(&time);

        // 접두사와 본문을 한 버퍼에 바로 쓴다. 예전에는 2 KB 버퍼에 포맷한 뒤 2.3 KB 버퍼로 다시
        // 복사했다. 게임 스레드에서 하는 일은 이 vsnprintf 하나와 링에 넣는 memcpy가 전부다.
        char line[kLogCellCapacity];
        int offset = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u][tid=%lu] ", time.wHour, time.wMinute,
                              time.wSecond, time.wMilliseconds, GetCurrentThreadId());
        if (offset < 0 || static_cast<std::size_t>(offset) >= sizeof(line))
            return;

        va_list args;
        va_start(args, format);
        const int body = vsnprintf_s(line + offset, sizeof(line) - static_cast<std::size_t>(offset) - 2, _TRUNCATE,
                                     format, args);
        va_end(args);

        std::size_t length = static_cast<std::size_t>(offset);
        if (body > 0)
            length += static_cast<std::size_t>(body);
        else if (body < 0)
            length = strnlen(line, sizeof(line) - 2);  // _TRUNCATE로 잘렸을 때.

        line[length++] = '\r';
        line[length++] = '\n';

        if (g_writerRunning.load(std::memory_order_acquire))
        {
            EnqueueLine(line, length);
            if (g_logDataEvent)
                SetEvent(g_logDataEvent);
            return;
        }

        WriteLineSync(line, length);
    }

    void Flush()
    {
        if (!g_writerRunning.load(std::memory_order_acquire))
            return;

        g_flushRequested.store(true, std::memory_order_release);
        if (g_logDataEvent)
            SetEvent(g_logDataEvent);
        if (g_flushDoneEvent)
            WaitForSingleObject(g_flushDoneEvent, kFlushWaitTimeoutMs);
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
