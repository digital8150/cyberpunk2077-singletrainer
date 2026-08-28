"""Dump call-stack candidates for every thread of a live (possibly hung) process.

No debugger required. For each thread it suspends, reads RIP/RSP through
GetThreadContext, scans the thread stack for qwords that land inside a loaded
module's executable range, and resolves them to module+offset. When dbghelp can
load symbols for a module (a PDB sitting next to the DLL), the nearest symbol
name is printed too.

Intended for post-mortem triage of a frozen game process: run it while the
process is still alive to find which module each thread is parked in.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)

PROCESS_ALL = 0x1F0FFF
THREAD_GET_CONTEXT = 0x0008
THREAD_SUSPEND_RESUME = 0x0002
THREAD_QUERY_INFORMATION = 0x0040
TH32CS_SNAPTHREAD = 0x00000004
CONTEXT_AMD64 = 0x00100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2
CONTEXT_SEGMENTS = CONTEXT_AMD64 | 0x4
CONTEXT_FULL = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS

MEM_COMMIT = 0x1000
EXECUTABLE_PROTECT = (0x10, 0x20, 0x40, 0x80)


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_ulonglong), ("High", ctypes.c_longlong)]


class CONTEXT64(ctypes.Structure):
    _pack_ = 16
    _fields_ = [
        ("P1Home", ctypes.c_ulonglong), ("P2Home", ctypes.c_ulonglong),
        ("P3Home", ctypes.c_ulonglong), ("P4Home", ctypes.c_ulonglong),
        ("P5Home", ctypes.c_ulonglong), ("P6Home", ctypes.c_ulonglong),
        ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
        ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD),
        ("SegFs", wt.WORD), ("SegGs", wt.WORD), ("SegSs", wt.WORD),
        ("EFlags", wt.DWORD),
        ("Dr0", ctypes.c_ulonglong), ("Dr1", ctypes.c_ulonglong),
        ("Dr2", ctypes.c_ulonglong), ("Dr3", ctypes.c_ulonglong),
        ("Dr6", ctypes.c_ulonglong), ("Dr7", ctypes.c_ulonglong),
        ("Rax", ctypes.c_ulonglong), ("Rcx", ctypes.c_ulonglong),
        ("Rdx", ctypes.c_ulonglong), ("Rbx", ctypes.c_ulonglong),
        ("Rsp", ctypes.c_ulonglong), ("Rbp", ctypes.c_ulonglong),
        ("Rsi", ctypes.c_ulonglong), ("Rdi", ctypes.c_ulonglong),
        ("R8", ctypes.c_ulonglong), ("R9", ctypes.c_ulonglong),
        ("R10", ctypes.c_ulonglong), ("R11", ctypes.c_ulonglong),
        ("R12", ctypes.c_ulonglong), ("R13", ctypes.c_ulonglong),
        ("R14", ctypes.c_ulonglong), ("R15", ctypes.c_ulonglong),
        ("Rip", ctypes.c_ulonglong),
        ("FltSave", ctypes.c_byte * 512),
        ("VectorRegister", M128A * 26), ("VectorControl", ctypes.c_ulonglong),
        ("DebugControl", ctypes.c_ulonglong), ("LastBranchToRip", ctypes.c_ulonglong),
        ("LastBranchFromRip", ctypes.c_ulonglong), ("LastExceptionToRip", ctypes.c_ulonglong),
        ("LastExceptionFromRip", ctypes.c_ulonglong),
    ]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
        ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
        ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
        ("dwFlags", wt.DWORD),
    ]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wt.DWORD),
                ("EntryPoint", ctypes.c_void_p)]


class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong), ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wt.DWORD), ("__alignment1", wt.DWORD),
        ("RegionSize", ctypes.c_ulonglong), ("State", wt.DWORD),
        ("Protect", wt.DWORD), ("Type", wt.DWORD), ("__alignment2", wt.DWORD),
    ]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG),
        ("Reserved", ctypes.c_ulonglong * 2), ("Index", wt.ULONG),
        ("Size", wt.ULONG), ("ModBase", ctypes.c_ulonglong),
        ("Flags", wt.ULONG), ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong), ("Register", wt.ULONG),
        ("Scope", wt.ULONG), ("Tag", wt.ULONG),
        ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
        ("Name", ctypes.c_char * 1024),
    ]


def _bind():
    table = [
        (k32.OpenProcess, [wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE),
        (k32.OpenThread, [wt.DWORD, wt.BOOL, wt.DWORD], wt.HANDLE),
        (k32.CloseHandle, [wt.HANDLE], wt.BOOL),
        (k32.SuspendThread, [wt.HANDLE], wt.DWORD),
        (k32.ResumeThread, [wt.HANDLE], wt.DWORD),
        (k32.GetThreadContext, [wt.HANDLE, ctypes.POINTER(CONTEXT64)], wt.BOOL),
        (k32.ReadProcessMemory,
         [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
          ctypes.POINTER(ctypes.c_size_t)], wt.BOOL),
        (k32.VirtualQueryEx,
         [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MEMORY_BASIC_INFORMATION64),
          ctypes.c_size_t], ctypes.c_size_t),
        (k32.CreateToolhelp32Snapshot, [wt.DWORD, wt.DWORD], wt.HANDLE),
        (k32.Thread32First, [wt.HANDLE, ctypes.POINTER(THREADENTRY32)], wt.BOOL),
        (k32.Thread32Next, [wt.HANDLE, ctypes.POINTER(THREADENTRY32)], wt.BOOL),
        (psapi.EnumProcessModules,
         [wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD, ctypes.POINTER(wt.DWORD)], wt.BOOL),
        (psapi.GetModuleInformation,
         [wt.HANDLE, wt.HMODULE, ctypes.POINTER(MODULEINFO), wt.DWORD], wt.BOOL),
        (psapi.GetModuleFileNameExA,
         [wt.HANDLE, wt.HMODULE, ctypes.c_char_p, wt.DWORD], wt.DWORD),
        (dbghelp.SymInitialize, [wt.HANDLE, ctypes.c_char_p, wt.BOOL], wt.BOOL),
        (dbghelp.SymCleanup, [wt.HANDLE], wt.BOOL),
        (dbghelp.SymSetOptions, [wt.DWORD], wt.DWORD),
        (dbghelp.SymLoadModuleEx,
         [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulonglong,
          wt.DWORD, ctypes.c_void_p, wt.DWORD], ctypes.c_ulonglong),
        (dbghelp.SymFromAddr,
         [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong),
          ctypes.POINTER(SYMBOL_INFO)], wt.BOOL),
    ]
    for fn, argtypes, restype in table:
        fn.argtypes = argtypes
        fn.restype = restype


_bind()


def modules_of(process):
    needed = wt.DWORD()
    array = (wt.HMODULE * 2048)()
    if not psapi.EnumProcessModules(process, array, ctypes.sizeof(array), ctypes.byref(needed)):
        return []
    count = min(needed.value // ctypes.sizeof(wt.HMODULE), 2048)
    result = []
    for i in range(count):
        info = MODULEINFO()
        if not psapi.GetModuleInformation(process, array[i], ctypes.byref(info), ctypes.sizeof(info)):
            continue
        path = ctypes.create_string_buffer(260)
        psapi.GetModuleFileNameExA(process, array[i], path, 260)
        full = path.value.decode("mbcs", "replace")
        result.append((info.lpBaseOfDll or 0, info.SizeOfImage, full.rsplit("\\", 1)[-1], full))
    result.sort()
    return result


def resolve(mods, address):
    for base, size, name, _ in mods:
        if base <= address < base + size:
            return name, address - base
    return None, 0


def read(process, address, size):
    buf = (ctypes.c_ubyte * size)()
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(process, ctypes.c_void_p(address), buf, size, ctypes.byref(got)):
        return None
    return bytes(buf[:got.value])


def executable_ranges(process):
    ranges = []
    address = 0
    mbi = MEMORY_BASIC_INFORMATION64()
    while address < 0x7FFFFFFF0000:
        if k32.VirtualQueryEx(process, ctypes.c_void_p(address), ctypes.byref(mbi),
                              ctypes.sizeof(mbi)) == 0:
            break
        if mbi.State == MEM_COMMIT and (mbi.Protect & 0xFF) in EXECUTABLE_PROTECT:
            ranges.append((mbi.BaseAddress, mbi.BaseAddress + mbi.RegionSize))
        if mbi.RegionSize == 0:
            break
        address = mbi.BaseAddress + mbi.RegionSize
    return ranges


def in_ranges(ranges, value):
    for low, high in ranges:
        if low <= value < high:
            return True
    return False


def stack_region(process, rsp):
    mbi = MEMORY_BASIC_INFORMATION64()
    if k32.VirtualQueryEx(process, ctypes.c_void_p(rsp), ctypes.byref(mbi),
                          ctypes.sizeof(mbi)) == 0:
        return None
    return mbi.BaseAddress, mbi.BaseAddress + mbi.RegionSize


def main():
    parser = argparse.ArgumentParser(
        description="Dump per-thread stack candidates of a live process (no debugger needed).")
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--tid", type=int, action="append", default=[],
                        help="only these thread ids (repeatable)")
    parser.add_argument("--frames", type=int, default=32,
                        help="max return-address candidates per thread")
    parser.add_argument("--symbols", default="",
                        help="extra semicolon-separated symbol search path")
    parser.add_argument("--filter-module", default="",
                        help="only print threads whose RIP or stack touches this module substring")
    args = parser.parse_args()

    process = k32.OpenProcess(PROCESS_ALL, False, args.pid)
    if not process:
        print("OpenProcess failed err=%d" % ctypes.get_last_error())
        return 1

    mods = modules_of(process)
    print("modules: %d" % len(mods))

    dbghelp.SymSetOptions(0x00000004 | 0x00000010 | 0x00000200 | 0x80000000)
    search = args.symbols.encode("mbcs") if args.symbols else None
    have_symbols = bool(dbghelp.SymInitialize(process, search, False))
    loaded = set()
    if have_symbols:
        for base, size, name, full in mods:
            if "trainer" in name.lower():
                dbghelp.SymLoadModuleEx(process, None, full.encode("mbcs"),
                                        name.encode("mbcs"), base, size, None, 0)
                loaded.add(name)
        print("symbol modules loaded: %s" % (", ".join(sorted(loaded)) or "none"))

    def symbolize(address):
        if not have_symbols:
            return ""
        info = SYMBOL_INFO()
        info.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 1024
        info.MaxNameLen = 1023
        disp = ctypes.c_ulonglong()
        if dbghelp.SymFromAddr(process, address, ctypes.byref(disp), ctypes.byref(info)):
            return " %s+0x%X" % (info.Name.decode("mbcs", "replace"), disp.value)
        return ""

    ranges = executable_ranges(process)
    print("executable ranges: %d" % len(ranges))

    snapshot = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    tids = []
    if k32.Thread32First(snapshot, ctypes.byref(entry)):
        while True:
            if entry.th32OwnerProcessID == args.pid:
                tids.append(entry.th32ThreadID)
            if not k32.Thread32Next(snapshot, ctypes.byref(entry)):
                break
    k32.CloseHandle(snapshot)
    if args.tid:
        tids = [t for t in tids if t in args.tid]
    print("threads: %d\n" % len(tids))

    for tid in tids:
        thread = k32.OpenThread(
            THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, False, tid)
        if not thread:
            continue
        k32.SuspendThread(thread)
        context = CONTEXT64()
        context.ContextFlags = CONTEXT_FULL
        ok = bool(k32.GetThreadContext(thread, ctypes.byref(context)))
        rip, rsp, rbp = (context.Rip, context.Rsp, context.Rbp) if ok else (0, 0, 0)
        regs = {}
        if ok:
            for name in ("Rax", "Rcx", "Rdx", "Rbx", "Rsi", "Rdi", "R8", "R9",
                         "R10", "R11", "R12", "R13", "R14", "R15"):
                regs[name] = getattr(context, name)
        k32.ResumeThread(thread)
        k32.CloseHandle(thread)
        if not ok:
            continue

        frames = []
        region = stack_region(process, rsp)
        if region:
            _, top = region
            span = min(top - rsp, 0x20000)
            data = read(process, rsp, span) if span > 8 else None
            if data:
                seen = set()
                for off in range(0, len(data) - 8, 8):
                    value = int.from_bytes(data[off:off + 8], "little")
                    if value < 0x10000 or value > 0x7FFFFFFFFFFF:
                        continue
                    if not in_ranges(ranges, value):
                        continue
                    name, moff = resolve(mods, value)
                    key = (name, moff)
                    if key in seen:
                        continue
                    seen.add(key)
                    frames.append((rsp + off, value, name, moff))
                    if len(frames) >= args.frames:
                        break

        if args.filter_module:
            needle = args.filter_module.lower()
            hit = needle in (resolve(mods, rip)[0] or "").lower() or any(
                needle in (n or "").lower() for _, _, n, _ in frames)
            if not hit:
                continue

        ripmod, ripoff = resolve(mods, rip)
        print("=== tid %d ===" % tid)
        print("  RIP=%016X  %s+0x%X%s" % (rip, ripmod or "<unmapped>", ripoff, symbolize(rip)))
        print("  RSP=%016X RBP=%016X" % (rsp, rbp))
        mapped = [(k, v) for k, v in regs.items() if v and resolve(mods, v)[0]]
        if mapped:
            print("  regs->modules: " + ", ".join(
                "%s=%s+0x%X" % (k, resolve(mods, v)[0], resolve(mods, v)[1])
                for k, v in mapped[:8]))
        for slot, value, name, moff in frames:
            print("    [%016X] %016X  %s+0x%X%s" % (slot, value, name or "?", moff,
                                                    symbolize(value)))
        print()

    if have_symbols:
        dbghelp.SymCleanup(process)
    k32.CloseHandle(process)
    return 0


if __name__ == "__main__":
    sys.exit(main())
