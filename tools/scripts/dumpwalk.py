"""Minimal Windows x64 minidump reader for hang/crash triage.

No external dependencies and no debugger install required. Parses the module and
thread lists, then scans each thread stack for qwords that land inside a loaded
module image so a pseudo call stack can be attributed to a module even without
symbols.

Usage:
    python tools/scripts/dumpwalk.py <dump.dmp> [--thread TID] [--depth N]
"""

from __future__ import annotations

import argparse
import struct
import sys
from bisect import bisect_right

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_MEMORY_LIST = 5
STREAM_MEMORY64_LIST = 9

CTX_RSP = 0x98
CTX_RBP = 0xA0
CTX_RIP = 0xF8


class Dump:
    def __init__(self, data: bytes) -> None:
        self.data = data
        if data[:4] != b"MDMP":
            raise SystemExit("not a minidump (missing MDMP signature)")
        streams, dir_rva = struct.unpack_from("<II", data, 8)
        self.streams = {}
        for i in range(streams):
            stype, size, rva = struct.unpack_from("<III", data, dir_rva + i * 12)
            self.streams[stype] = (size, rva)
        self.modules = self._modules()
        self.mod_bases = [m[0] for m in self.modules]
        self.memory = self._memory()
        self.mem_starts = [m[0] for m in self.memory]

    def _string(self, rva: int) -> str:
        length = struct.unpack_from("<I", self.data, rva)[0]
        return self.data[rva + 4: rva + 4 + length].decode("utf-16-le", "replace")

    def _modules(self):
        size, rva = self.streams[STREAM_MODULE_LIST]
        count = struct.unpack_from("<I", self.data, rva)[0]
        out = []
        for i in range(count):
            off = rva + 4 + i * 108
            base, image_size, _chk, _ts, name_rva = struct.unpack_from("<QIIII", self.data, off)
            out.append((base, image_size, self._string(name_rva)))
        out.sort()
        return out

    def _memory(self):
        out = []
        if STREAM_MEMORY_LIST in self.streams:
            _size, rva = self.streams[STREAM_MEMORY_LIST]
            count = struct.unpack_from("<I", self.data, rva)[0]
            for i in range(count):
                start, dsize, drva = struct.unpack_from("<QII", self.data, rva + 4 + i * 16)
                out.append((start, dsize, drva))
        if STREAM_MEMORY64_LIST in self.streams:
            _size, rva = self.streams[STREAM_MEMORY64_LIST]
            count, base_rva = struct.unpack_from("<QQ", self.data, rva)
            cursor = base_rva
            for i in range(count):
                start, dsize = struct.unpack_from("<QQ", self.data, rva + 16 + i * 16)
                out.append((start, dsize, cursor))
                cursor += dsize
        out.sort()
        return out

    def module_of(self, address: int):
        if not address:
            return None
        i = bisect_right(self.mod_bases, address) - 1
        if i < 0:
            return None
        base, image_size, name = self.modules[i]
        if base <= address < base + image_size:
            return (name.rsplit("\\", 1)[-1], address - base)
        return None

    def threads(self):
        _size, rva = self.streams[STREAM_THREAD_LIST]
        count = struct.unpack_from("<I", self.data, rva)[0]
        out = []
        for i in range(count):
            off = rva + 4 + i * 48
            tid, suspend, _pcls, _prio, teb, stack_start, stack_size, stack_rva, ctx_size, ctx_rva = \
                struct.unpack_from("<IIIIQQIIII", self.data, off)
            ctx = self.data[ctx_rva: ctx_rva + ctx_size]
            rip = struct.unpack_from("<Q", ctx, CTX_RIP)[0] if len(ctx) > CTX_RIP else 0
            rsp = struct.unpack_from("<Q", ctx, CTX_RSP)[0] if len(ctx) > CTX_RSP else 0
            out.append({
                "tid": tid, "suspend": suspend, "teb": teb, "rip": rip, "rsp": rsp,
                "stack_start": stack_start, "stack_size": stack_size, "stack_rva": stack_rva,
            })
        return out

    def stack_trace(self, thread, depth: int):
        frames = []
        seen = set()
        start, size, rva = thread["stack_start"], thread["stack_size"], thread["stack_rva"]
        rsp = thread["rsp"]
        begin = max(0, rsp - start) if start <= rsp < start + size else 0
        blob = self.data[rva: rva + size]
        for off in range(begin & ~7, len(blob) - 7, 8):
            value = struct.unpack_from("<Q", blob, off)[0]
            hit = self.module_of(value)
            if not hit:
                continue
            key = (hit[0], hit[1])
            if key in seen:
                continue
            seen.add(key)
            frames.append((start + off, value, hit))
            if len(frames) >= depth:
                break
        return frames


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--thread", type=int, default=None, help="only this thread id")
    ap.add_argument("--depth", type=int, default=24, help="max pseudo frames per thread")
    ap.add_argument("--modules", action="store_true", help="list loaded modules and exit")
    args = ap.parse_args()

    with open(args.dump, "rb") as handle:
        dump = Dump(handle.read())

    if args.modules:
        for base, size, name in dump.modules:
            print(f"{base:016X} {size:9d} {name}")
        return 0

    for thread in dump.threads():
        if args.thread is not None and thread["tid"] != args.thread:
            continue
        hit = dump.module_of(thread["rip"])
        where = f"{hit[0]}+0x{hit[1]:X}" if hit else "<unknown>"
        print(f"\n=== tid={thread['tid']} rip={thread['rip']:016X} ({where}) rsp={thread['rsp']:016X}")
        for slot, value, frame in dump.stack_trace(thread, args.depth):
            print(f"    {slot:016X}  {value:016X}  {frame[0]}+0x{frame[1]:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
