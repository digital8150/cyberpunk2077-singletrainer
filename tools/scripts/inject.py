#!/usr/bin/env python3
"""inject.py — CreateRemoteThread + LoadLibraryW 방식의 고전적인 DLL 인젝터.

트레이너 DLL(예: build/bin/Release/cp2077_trainer.dll)을 실행 중인 Cyberpunk2077.exe에 넣어 테스트하기
위한 재사용 가능한 도구. memtool.py와 마찬가지로 ctypes만 사용, 외부 의존성 없음. 같은 디렉토리의
memtool.py를 import해서 프로세스/모듈 열거 로직을 재사용한다(중복 구현 안 함).

한계: x64에서는 원격 스레드의 종료 코드(32비트)로 LoadLibraryW의 반환값(64비트 HMODULE)을 온전히 받을
수 없다 — 그래서 성공 여부는 종료 코드가 아니라 "주입 후 대상 프로세스의 모듈 목록에 그 DLL이 실제로
나타났는가"로 판정한다.

사용법:
    python inject.py --pid 1234 --dll ..\\..\\build\\bin\\Release\\cp2077_trainer.dll
    python inject.py --name Cyberpunk2077.exe --dll ..\\..\\build\\bin\\Release\\cp2077_trainer.dll
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import os
import sys
import time

import memtool  # 같은 디렉토리 — 프로세스/모듈 열거, PROCESS_ALL_ACCESS 재사용

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# ctypes는 선언 안 된 함수의 반환값을 기본으로 32비트 c_int로 취급한다 — 포인터/HANDLE을 반환하는 함수는
# 이걸 명시적으로 지정 안 하면 64비트 주소의 상위 32비트가 잘려나간다(WriteProcessMemory가 ERROR_NOACCESS
# 로 실패하는 원인이 이거였음). 아래처럼 restype/argtypes를 명시해야 x64에서 안전하다.
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.VirtualAllocEx.restype = wintypes.LPVOID
kernel32.VirtualAllocEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
kernel32.WriteProcessMemory.restype = wintypes.BOOL
kernel32.WriteProcessMemory.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, wintypes.LPCVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
]
kernel32.VirtualFreeEx.restype = wintypes.BOOL
kernel32.VirtualFreeEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD]
kernel32.CreateRemoteThread.restype = wintypes.HANDLE
kernel32.CreateRemoteThread.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.LPVOID, wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD)
]
kernel32.GetModuleHandleW.restype = wintypes.HMODULE
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel32.GetProcAddress.restype = wintypes.LPVOID
kernel32.GetProcAddress.argtypes = [wintypes.HMODULE, ctypes.c_char_p]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.GetExitCodeThread.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]


def resolve_pid(args) -> int:
    if args.pid:
        return args.pid
    matches = [pid for pid, name in memtool.iter_processes() if name.lower() == args.name.lower()]
    if not matches:
        raise SystemExit(f"process not found: {args.name}")
    if len(matches) > 1:
        raise SystemExit(f"multiple processes named {args.name}: {matches} - pass --pid instead")
    return matches[0]


def is_dll_loaded(pid: int, dll_basename: str) -> bool:
    return any(name.lower() == dll_basename.lower() for name, _, _ in memtool.iter_modules(pid))


def inject(pid: int, dll_path: str) -> bool:
    dll_path = os.path.abspath(dll_path)
    if not os.path.isfile(dll_path):
        raise SystemExit(f"dll not found: {dll_path}")
    dll_basename = os.path.basename(dll_path)

    if is_dll_loaded(pid, dll_basename):
        print(f"{dll_basename} is already loaded in pid={pid}, nothing to do")
        return True

    h_process = kernel32.OpenProcess(memtool.PROCESS_ALL_ACCESS, False, pid)
    if not h_process:
        raise OSError(
            f"OpenProcess({pid}) failed (err={ctypes.get_last_error()}). Try running as Administrator."
        )

    try:
        path_bytes = dll_path.encode("utf-16-le") + b"\x00\x00"
        size = len(path_bytes)

        remote_mem = kernel32.VirtualAllocEx(h_process, None, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
        if not remote_mem:
            raise OSError(f"VirtualAllocEx failed (err={ctypes.get_last_error()})")

        try:
            written = ctypes.c_size_t(0)
            ok = kernel32.WriteProcessMemory(
                h_process, ctypes.c_void_p(remote_mem), path_bytes, size, ctypes.byref(written)
            )
            if not ok:
                raise OSError(f"WriteProcessMemory failed (err={ctypes.get_last_error()})")

            h_kernel32 = kernel32.GetModuleHandleW("kernel32.dll")
            load_library_addr = kernel32.GetProcAddress(h_kernel32, b"LoadLibraryW")
            if not load_library_addr:
                raise OSError("GetProcAddress(LoadLibraryW) failed")

            thread_id = wintypes.DWORD(0)
            h_thread = kernel32.CreateRemoteThread(
                h_process, None, 0, load_library_addr, ctypes.c_void_p(remote_mem), 0, ctypes.byref(thread_id)
            )
            if not h_thread:
                raise OSError(f"CreateRemoteThread failed (err={ctypes.get_last_error()})")

            try:
                kernel32.WaitForSingleObject(h_thread, 5000)
            finally:
                kernel32.CloseHandle(h_thread)
        finally:
            kernel32.VirtualFreeEx(h_process, ctypes.c_void_p(remote_mem), 0, MEM_RELEASE)
    finally:
        kernel32.CloseHandle(h_process)

    # 원격 스레드 종료 코드는 x64에서 신뢰 못 함 (위 docstring 참고) - 모듈 목록 재확인으로 판정.
    for _ in range(10):
        if is_dll_loaded(pid, dll_basename):
            return True
        time.sleep(0.2)
    return False


def main(argv=None) -> int:
    if sys.platform != "win32":
        print("this tool is Windows-only.", file=sys.stderr)
        return 1

    # Keep runtime/help output ASCII-only so this remains usable from cp949 and other legacy Windows consoles.
    parser = argparse.ArgumentParser(
        description="Inject a DLL with LoadLibraryW and CreateRemoteThread."
    )
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--pid", type=int)
    target.add_argument("--name", help="process executable name, e.g. Cyberpunk2077.exe")
    parser.add_argument("--dll", required=True, help="path to the DLL to inject")
    args = parser.parse_args(argv)

    pid = resolve_pid(args)
    try:
        ok = inject(pid, args.dll)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if ok:
        print(f"success: {os.path.basename(args.dll)} loaded into pid={pid}")
        return 0
    print(f"failed: {os.path.basename(args.dll)} did not appear in pid={pid}'s module list", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
