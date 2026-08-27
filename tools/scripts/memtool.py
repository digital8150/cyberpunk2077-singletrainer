#!/usr/bin/env python3
"""memtool.py — 범용 Windows 프로세스 메모리 read / write / scan CLI.

Cheat Engine 설치 여부와 무관하게, Python 표준 라이브러리(ctypes)만으로 동작하는
경량 메모리 분석 도구. 리버스 엔지니어링 중 반복적으로 필요한 동작(값 스캔 →
다음 스캔으로 좁히기 → 주소 read/write → AOB 패턴 스캔)을 CLI 한 번으로 처리한다.

요구사항: Windows, Python 3.9+ (외부 패키지 불필요). 관리자 권한으로 실행해야
대부분의 게임 프로세스에 OpenProcess가 성공한다.

예시:
    python memtool.py list --filter Cyberpunk
    python memtool.py scan --pid 1234 --type int32 100
    python memtool.py rescan --pid 1234 --changed
    python memtool.py rescan --pid 1234 95
    python memtool.py read --pid 1234 --address 0x7FF6A0001000 --type float
    python memtool.py write --pid 1234 --address 0x7FF6A0001000 --type int32 999
    python memtool.py aobscan --pid 1234 --module GameCore.dll --pattern "48 8B 05 ?? ?? ?? ?? 89"

스캔/재스캔 세션 상태는 tools/scripts/.memtool_state/<pid>.json 에 저장되며,
동일 PID로 rescan을 반복할 때마다 후보가 좁혀진다. 새 scan을 실행하면 그 PID의
세션은 새로 시작된다(덮어씀).
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import struct
import sys
from pathlib import Path

STATE_DIR = Path(__file__).resolve().parent / ".memtool_state"

# ---------------------------------------------------------------------------
# WinAPI 상수 / 구조체
# ---------------------------------------------------------------------------

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_OPERATION = 0x0008
PROCESS_ALL_ACCESS = (
    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
)

MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
WRITABLE_PROTECT = {0x04, 0x08, 0x40, 0x80}  # RW, WRITECOPY, EXECUTE_RW, EXECUTE_WRITECOPY

TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_char * 260),
    ]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", wintypes.HMODULE),
        ("szModule", ctypes.c_char * 256),
        ("szExePath", ctypes.c_char * 260),
    ]


# ---------------------------------------------------------------------------
# 값 타입 <-> struct 포맷
# ---------------------------------------------------------------------------

TYPE_FORMATS = {
    "int8": "<b", "uint8": "<B",
    "int16": "<h", "uint16": "<H",
    "int32": "<i", "uint32": "<I",
    "int64": "<q", "uint64": "<Q",
    "float": "<f", "double": "<d",
}


def encode_value(type_name: str, value: str) -> bytes:
    if type_name == "string":
        return value.encode("utf-8")
    if type_name == "bytes":
        return bytes.fromhex(value.replace(" ", ""))
    fmt = TYPE_FORMATS[type_name]
    num = float(value) if type_name in ("float", "double") else int(value, 0)
    return struct.pack(fmt, num)


def decode_value(type_name: str, data: bytes):
    if type_name == "string":
        return data.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    if type_name == "bytes":
        return data.hex(" ")
    fmt = TYPE_FORMATS[type_name]
    return struct.unpack(fmt, data)[0]


# ---------------------------------------------------------------------------
# 프로세스 핸들 / 메모리 접근
# ---------------------------------------------------------------------------

class ProcessHandle:
    def __init__(self, pid: int):
        self.pid = pid
        self.handle = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
        if not self.handle:
            err = ctypes.get_last_error()
            raise OSError(
                f"OpenProcess({pid}) failed (err={err}). Try running as Administrator."
            )

    def close(self):
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()

    def read(self, address: int, size: int) -> bytes:
        buf = ctypes.create_string_buffer(size)
        n_read = ctypes.c_size_t(0)
        ok = kernel32.ReadProcessMemory(
            self.handle, ctypes.c_void_p(address), buf, size, ctypes.byref(n_read)
        )
        if not ok or n_read.value != size:
            raise OSError(f"ReadProcessMemory failed @0x{address:X} (err={ctypes.get_last_error()})")
        return buf.raw

    def write(self, address: int, data: bytes) -> None:
        n_written = ctypes.c_size_t(0)
        ok = kernel32.WriteProcessMemory(
            self.handle, ctypes.c_void_p(address), data, len(data), ctypes.byref(n_written)
        )
        if not ok or n_written.value != len(data):
            raise OSError(f"WriteProcessMemory failed @0x{address:X} (err={ctypes.get_last_error()})")

    def regions(self, writable_only: bool):
        address = 0
        mbi = MEMORY_BASIC_INFORMATION()
        max_addr = 0x7FFFFFFFFFFF  # 사용자 모드 가상주소 상한 (x64)
        while address < max_addr:
            ret = kernel32.VirtualQueryEx(
                self.handle, ctypes.c_void_p(address), ctypes.byref(mbi), ctypes.sizeof(mbi)
            )
            if ret == 0:
                break
            if mbi.State == MEM_COMMIT and not (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)):
                if not writable_only or (mbi.Protect & 0xFF) in WRITABLE_PROTECT:
                    yield mbi.BaseAddress or 0, mbi.RegionSize
            address = (mbi.BaseAddress or 0) + mbi.RegionSize
            if mbi.RegionSize == 0:
                break  # 무한루프 방지


def iter_processes():
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == -1:
        raise OSError("CreateToolhelp32Snapshot failed")
    try:
        entry = PROCESSENTRY32()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32)
        if not kernel32.Process32First(snapshot, ctypes.byref(entry)):
            return
        while True:
            yield entry.th32ProcessID, entry.szExeFile.decode(errors="replace")
            if not kernel32.Process32Next(snapshot, ctypes.byref(entry)):
                break
    finally:
        kernel32.CloseHandle(snapshot)


def iter_modules(pid: int):
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snapshot == -1:
        raise OSError(f"CreateToolhelp32Snapshot(module, pid={pid}) failed - process missing or access denied")
    try:
        entry = MODULEENTRY32()
        entry.dwSize = ctypes.sizeof(MODULEENTRY32)
        if not kernel32.Module32First(snapshot, ctypes.byref(entry)):
            return
        while True:
            base = ctypes.cast(entry.modBaseAddr, ctypes.c_void_p).value or 0
            yield entry.szModule.decode(errors="replace"), base, entry.modBaseSize
            if not kernel32.Module32Next(snapshot, ctypes.byref(entry)):
                break
    finally:
        kernel32.CloseHandle(snapshot)


# ---------------------------------------------------------------------------
# 스캔 세션 상태 저장/로드
# ---------------------------------------------------------------------------

def state_path(pid: int) -> Path:
    STATE_DIR.mkdir(exist_ok=True)
    return STATE_DIR / f"{pid}.json"


def save_state(pid: int, type_name: str, results: list[dict]) -> None:
    state_path(pid).write_text(
        json.dumps({"type": type_name, "results": results}, indent=2), encoding="utf-8"
    )


def load_state(pid: int) -> dict:
    path = state_path(pid)
    if not path.exists():
        raise SystemExit(f"no saved scan session for pid={pid}. Run `scan` first.")
    return json.loads(path.read_text(encoding="utf-8"))


def print_results(results: list[dict], limit: int = 50) -> None:
    # 콘솔 코드페이지(cp949 등)에서 한글이 깨지는 걸 피하려고 실행 출력은 영어로 고정한다.
    print(f"results: {len(results)}")
    for row in results[:limit]:
        print(f"  0x{row['addr']:X}  = {row['value']}")
    if len(results) > limit:
        print(f"  ... and {len(results) - limit} more (narrow down with rescan)")


# ---------------------------------------------------------------------------
# 서브커맨드 구현
# ---------------------------------------------------------------------------

def cmd_list(args):
    for pid, name in iter_processes():
        if args.filter and args.filter.lower() not in name.lower():
            continue
        print(f"{pid:>8}  {name}")


def cmd_modules(args):
    for name, base, size in iter_modules(args.pid):
        print(f"0x{base:016X}  {size:>10}  {name}")


def cmd_scan(args):
    needle = encode_value(args.type, args.value)
    results: list[dict] = []
    with ProcessHandle(args.pid) as proc:
        for base, size in proc.regions(writable_only=not args.all):
            try:
                data = proc.read(base, size)
            except OSError:
                continue
            start = 0
            while True:
                idx = data.find(needle, start)
                if idx == -1:
                    break
                addr = base + idx
                results.append({"addr": addr, "value": decode_value(args.type, needle)})
                start = idx + 1
    save_state(args.pid, args.type, results)
    print_results(results)


def cmd_rescan(args):
    state = load_state(args.pid)
    type_name = state["type"]
    prev_results = state["results"]
    new_results = []
    with ProcessHandle(args.pid) as proc:
        size = struct.calcsize(TYPE_FORMATS[type_name]) if type_name in TYPE_FORMATS else None
        for row in prev_results:
            addr = row["addr"]
            try:
                if type_name in ("string", "bytes"):
                    raw = proc.read(addr, len(encode_value(type_name, str(row["value"]))))
                else:
                    raw = proc.read(addr, size)
                current = decode_value(type_name, raw)
            except OSError:
                continue

            if args.value is not None:
                target = encode_value(type_name, args.value)
                keep = raw == target if type_name in ("string", "bytes") else current == decode_value(type_name, target)
            elif args.changed:
                keep = current != row["value"]
            elif args.unchanged:
                keep = current == row["value"]
            elif args.increased:
                keep = current > row["value"]
            elif args.decreased:
                keep = current < row["value"]
            else:
                keep = True  # 조건 없으면 전부 유지하고 값만 갱신

            if keep:
                new_results.append({"addr": addr, "value": current})

    save_state(args.pid, type_name, new_results)
    print_results(new_results)


def cmd_read(args):
    with ProcessHandle(args.pid) as proc:
        size = struct.calcsize(TYPE_FORMATS[args.type]) if args.type in TYPE_FORMATS else args.size
        data = proc.read(args.address, size)
        print(decode_value(args.type, data))


def cmd_write(args):
    data = encode_value(args.type, args.value)
    with ProcessHandle(args.pid) as proc:
        proc.write(args.address, data)
    print(f"0x{args.address:X} <- {args.value} ({args.type}, {len(data)} bytes) written")


def parse_pattern(pattern: str):
    """"48 8B 05 ?? ?? ?? ?? 89" -> (bytes, mask) — mask 바이트는 True면 반드시 일치."""
    tokens = pattern.split()
    values = bytearray()
    mask = []
    for tok in tokens:
        if tok in ("??", "?"):
            values.append(0)
            mask.append(False)
        else:
            values.append(int(tok, 16))
            mask.append(True)
    return bytes(values), mask


def find_pattern(data: bytes, values: bytes, mask: list[bool]):
    n = len(values)
    if n == 0 or len(data) < n:
        return
    first_fixed = next((i for i, m in enumerate(mask) if m), None)
    for start in range(0, len(data) - n + 1):
        if first_fixed is not None and data[start + first_fixed] != values[first_fixed]:
            continue
        ok = True
        for i in range(n):
            if mask[i] and data[start + i] != values[i]:
                ok = False
                break
        if ok:
            yield start


def cmd_aobscan(args):
    values, mask = parse_pattern(args.pattern)
    with ProcessHandle(args.pid) as proc:
        if args.module:
            modules = [m for m in iter_modules(args.pid) if m[0].lower() == args.module.lower()]
            if not modules:
                raise SystemExit(f"module not found: {args.module}")
            regions = [(base, size) for _, base, size in modules]
        else:
            regions = list(proc.regions(writable_only=False))

        found = []
        for base, size in regions:
            try:
                data = proc.read(base, size)
            except OSError:
                continue
            for offset in find_pattern(data, values, mask):
                found.append(base + offset)

    print(f"matches: {len(found)}")
    for addr in found[:50]:
        print(f"  0x{addr:X}")
    if len(found) > 50:
        print(f"  ... and {len(found) - 50} more")


# ---------------------------------------------------------------------------
# argparse 배선
# ---------------------------------------------------------------------------

def auto_int(text: str) -> int:
    return int(text, 0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="실행 중인 프로세스 목록")
    p_list.add_argument("--filter", help="이름에 포함된 문자열로 필터링")
    p_list.set_defaults(func=cmd_list)

    p_mod = sub.add_parser("modules", help="프로세스의 로드된 모듈 목록")
    p_mod.add_argument("--pid", type=int, required=True)
    p_mod.set_defaults(func=cmd_modules)

    p_scan = sub.add_parser("scan", help="값으로 첫 스캔 (세션 시작)")
    p_scan.add_argument("--pid", type=int, required=True)
    p_scan.add_argument("--type", choices=list(TYPE_FORMATS) + ["string", "bytes"], required=True)
    p_scan.add_argument("--all", action="store_true", help="쓰기 가능 영역뿐 아니라 전체(읽기전용 포함) 스캔")
    p_scan.add_argument("value")
    p_scan.set_defaults(func=cmd_scan)

    p_rescan = sub.add_parser("rescan", help="이전 스캔 결과를 조건으로 좁히기")
    p_rescan.add_argument("--pid", type=int, required=True)
    group = p_rescan.add_mutually_exclusive_group()
    group.add_argument("--changed", action="store_true")
    group.add_argument("--unchanged", action="store_true")
    group.add_argument("--increased", action="store_true")
    group.add_argument("--decreased", action="store_true")
    p_rescan.add_argument("value", nargs="?", default=None, help="정확한 새 값으로 좁히기 (조건 플래그 대신 사용)")
    p_rescan.set_defaults(func=cmd_rescan)

    p_read = sub.add_parser("read", help="주소 하나 읽기")
    p_read.add_argument("--pid", type=int, required=True)
    p_read.add_argument("--address", type=auto_int, required=True)
    p_read.add_argument("--type", choices=list(TYPE_FORMATS) + ["string", "bytes"], required=True)
    p_read.add_argument("--size", type=int, default=64, help="string/bytes 타입일 때 읽을 바이트 수")
    p_read.set_defaults(func=cmd_read)

    p_write = sub.add_parser("write", help="주소 하나 쓰기")
    p_write.add_argument("--pid", type=int, required=True)
    p_write.add_argument("--address", type=auto_int, required=True)
    p_write.add_argument("--type", choices=list(TYPE_FORMATS) + ["string", "bytes"], required=True)
    p_write.add_argument("value")
    p_write.set_defaults(func=cmd_write)

    p_aob = sub.add_parser("aobscan", help="AOB(바이트 패턴) 스캔, ??는 와일드카드")
    p_aob.add_argument("--pid", type=int, required=True)
    p_aob.add_argument("--module", help="특정 모듈로 범위 제한 (예: GameCore.dll)")
    p_aob.add_argument("--pattern", required=True, help='예: "48 8B 05 ?? ?? ?? ?? 89"')
    p_aob.set_defaults(func=cmd_aobscan)

    return parser


def main(argv=None) -> int:
    if sys.platform != "win32":
        print("this tool is Windows-only.", file=sys.stderr)
        return 1
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
