#!/usr/bin/env python3
"""
Deadpool Switch Unified CPU / GPU / RAM Performance Log Analyzer
================================================================

Merges and analyzes:
  - renut_*.log        ([renut], [renut perf], [renut workload], deko3d, GPU profile, Memory:)
  - rex/rex_profile.log (10s Switch CPU/clocks/faults/libnx/per-thread PC samples & hitch stacks)
  - rex/banjo_profiler.log (optional game-side CPU profiler output)
  - out/switch-ult/renut.map (or --elf) for AArch64 symbolization of guest/host hot functions & stacks

Usage:
  python tools/deadpool_perf_report.py [logs_dir_or_files...]
  python tools/deadpool_perf_report.py output/switch/deadpool-nx/logs
  python tools/deadpool_perf_report.py logs/renut_001.log logs/rex/rex_profile.log --top 25
  python tools/deadpool_perf_report.py logs --drop-fps 25 --csv out_timeline.csv
"""

from __future__ import annotations

import argparse
import bisect
import collections
import csv
import glob
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


ROOT = Path(__file__).resolve().parent.parent


# ---------------------------------------------------------------------------
# Symbolization (linker .map preferred, ELF nm fallback)
# ---------------------------------------------------------------------------

class SymbolResolver:
    def __init__(self, map_path: Optional[Path] = None, elf_path: Optional[Path] = None):
        self.starts: List[int] = []
        self.ends: List[int] = []
        self.names: List[str] = []
        self.modules: List[str] = []
        self.source: str = "none"

        if map_path and map_path.is_file():
            self._load_map(map_path)
            self.source = f"map ({map_path})"
        elif elf_path and elf_path.is_file():
            self._load_elf(elf_path)
            self.source = f"elf ({elf_path})"

    @staticmethod
    def _clean_symbol_name(name: str) -> str:
        # Strip noisy template args / parameters for readability
        if name.startswith("sub_") and "(" in name:
            name = name[:name.index("(")]
        elif "(" in name:
            # Keep short signature prefix
            prefix = name[:name.index("(")]
            if len(prefix) > 72:
                prefix = prefix[:69] + "..."
            return prefix
        return name[:80] if len(name) > 80 else name

    @staticmethod
    def _classify_object(obj: str) -> str:
        o = obj.replace("\\", "/")
        low = o.lower()
        if "renut_recomp" in low or "_recomp.dir" in low or "generated/" in low:
            return "guest"
        if "banjo_renut_gpu" in low or "/renut_engine/" in low:
            return "renderer"
        if "librex" in low or "/rexglue-sdk/" in low or "/sdk/" in low:
            return "sdk"
        if "libplume" in low or "libdeko3d" in low or "libuam" in low or "libvulkan" in low or "mesa" in low:
            return "driver"
        if "libnx" in low or "libc" in low or "libstdc++" in low:
            return "syslib"
        if "renut.dir/" in low:
            return "app"
        m = re.search(r"([^/]+\.a)\(", o)
        if m:
            return m.group(1)
        return os.path.basename(o) or "?"

    def _load_map(self, path: Path) -> None:
        sec_re = re.compile(r"^ \.(text(?:\.[^\s]+)?)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+))?\s*$")
        cont_re = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)\s*$")
        sym_re = re.compile(r"^\s+0x000000([0-9a-f]+)\s+(\S[^\r\n]*)$")

        sections: List[Tuple[int, int, str, str]] = []
        sym_by_addr: Dict[int, str] = {}
        in_memory_map = False
        pending: Optional[str] = None
        last_obj = "?"

        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if not in_memory_map:
                    in_memory_map = line.startswith("Linker script and memory map")
                    continue
                if pending is not None:
                    m = cont_re.match(line)
                    if m:
                        start, size = int(m.group(1), 16), int(m.group(2), 16)
                        last_obj = m.group(3)
                        if size:
                            sections.append((start, start + size, pending, self._classify_object(last_obj)))
                    pending = None
                    continue
                m = sec_re.match(line)
                if m:
                    sec_name = m.group(1)
                    if sec_name.startswith("text.unlikely."):
                        sec_name = sec_name[len("text.unlikely."):]
                    elif sec_name.startswith("text.hot."):
                        sec_name = sec_name[len("text.hot."):]
                    elif sec_name.startswith("text."):
                        sec_name = sec_name[len("text."):]
                    if m.group(2):
                        start, size = int(m.group(2), 16), int(m.group(3), 16)
                        last_obj = m.group(4)
                        if size:
                            sections.append((start, start + size, sec_name, self._classify_object(last_obj)))
                    else:
                        pending = sec_name
                    continue
                mc = cont_re.match(line)
                if mc:
                    last_obj = mc.group(3)
                    continue
                ms = sym_re.match(line)
                if ms:
                    addr = int(ms.group(1), 16)
                    sym = ms.group(2).strip()
                    if sym and not sym.startswith(".") and "PROVIDE" not in sym and "=" not in sym:
                        sym_by_addr[addr] = sym
                        # Also capture non-split .text symbols inside big objects
                        if sections and sections[-1][0] <= addr < sections[-1][1] and sections[-1][2] == "text":
                            sections.append((addr, sections[-1][1], sym, self._classify_object(last_obj)))

        sections.sort(key=lambda x: (x[0], -(x[1] - x[0])))
        if sections:
            # Prefer exact symbol lines when multiple entries share the same start address
            merged: List[Tuple[int, int, str, str]] = []
            for s, e, n, mod in sections:
                pretty = sym_by_addr.get(s, n)
                if merged and merged[-1][0] == s:
                    # Keep the tighter range or better (non-generic) name
                    if pretty != "text" and merged[-1][2] in ("text", n):
                        merged[-1] = (s, min(e, merged[-1][1]), pretty, mod)
                    continue
                if pretty == "text" and sym_by_addr.get(s):
                    pretty = sym_by_addr[s]
                merged.append((s, e, pretty, mod))
            for s, e, n, mod in merged:
                self.starts.append(s)
                self.ends.append(e)
                self.names.append(self._clean_symbol_name(n))
                self.modules.append(mod)

    def _load_elf(self, path: Path) -> None:
        nm_candidates = [
            "aarch64-none-elf-nm",
            r"C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe",
        ]
        for nm in nm_candidates:
            try:
                out = subprocess.run(
                    [nm, "-n", "--defined-only", "-C", str(path)],
                    capture_output=True, text=True, errors="replace", check=False
                )
                if out.returncode == 0 and out.stdout:
                    for line in out.stdout.splitlines():
                        parts = line.split(" ", 2)
                        if len(parts) == 3 and parts[1] in "tTwW":
                            addr = int(parts[0], 16)
                            self.starts.append(addr)
                            self.ends.append(addr + 0x10000)
                            self.names.append(self._clean_symbol_name(parts[2]))
                            mod = "guest" if parts[2].startswith("sub_") else "host"
                            self.modules.append(mod)
                    return
            except OSError:
                continue

    def resolve(self, offset: int) -> Tuple[str, str]:
        if not self.starts:
            return f"0x{offset:x}", "?"
        i = bisect.bisect_right(self.starts, offset) - 1
        if i >= 0 and offset < self.ends[i]:
            return self.names[i], self.modules[i]
        return f"0x{offset:x}", "?"

    def resolve_pc_bucket(self, offset: int) -> Tuple[str, str]:
        name, mod = self.resolve(offset)
        if not self.starts:
            return name, mod
        i = bisect.bisect_right(self.starts, offset + 15) - 1
        if i >= 0 and self.starts[i] > offset and self.names[i] != name:
            return f"{name} / {self.names[i]}", mod
        return name, mod

    def resolve_return(self, offset: int) -> Tuple[str, str]:
        return self.resolve(max(0, offset - 1))


# ---------------------------------------------------------------------------
# Log parsers
# ---------------------------------------------------------------------------

RE_RENUT_TS = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\]\s+\[(\w+)\]\s+\[(\w+)\]\s+(?:\[t\d+\]\s+)?(.*)$")
RE_RENUT_MAIN = re.compile(
    r"\[renut\]\s+([0-9.]+)\s+s:\s+(\d+)\s+game frames\s+\(([0-9.]+)\s+fps\),\s+"
    r"render thread busy\s+([0-9.]+)\s+ms a frame\s+\((\d+)\s+frames\),\s+"
    r"GPU\s+([0-9.]+)\s+ms a frame\s+\((\d+)\s+frames\);"
)
RE_RENUT_WAITS = re.compile(
    r"fences\s+([0-9.]+)\s+\(([0-9.]+)\s+ms\),\s+"
    r"Swap's frames-ahead limit\s+([0-9.]+)\s+\(([0-9.]+)\s+ms\),\s+"
    r"the display's refresh\s+([0-9.]+)\s+\(([0-9.]+)\s+ms\)"
)
RE_RENUT_PERF = re.compile(
    r"\[renut perf\]\s+window\s+([0-9.]+)\s+s\s+\|\s+"
    r"game\((\d+)\):\s+avg\s+([0-9.]+)\s+p50\s+([0-9.]+)\s+p95\s+([0-9.]+)\s+p99\s+([0-9.]+)\s+max\s+([0-9.]+)\s+ms\s+"
    r"\(>33\.3ms:(\d+)\s+>50ms:(\d+)\s+>100ms:(\d+)\)\s+\|\s+"
    r"render\((\d+)\):\s+avg\s+([0-9.]+)\s+p50\s+([0-9.]+)\s+p95\s+([0-9.]+)\s+p99\s+([0-9.]+)\s+max\s+([0-9.]+)\s+ms\s+"
    r"\(>33\.3ms:(\d+)\)\s+\|\s+"
    r"gpu\((\d+)\):\s+avg\s+([0-9.]+)\s+p50\s+([0-9.]+)\s+p95\s+([0-9.]+)\s+p99\s+([0-9.]+)\s+max\s+([0-9.]+)\s+ms\s+"
    r"\(>33\.3ms:(\d+)\)\s+\|\s+hitches\(>35ms\):\s+(\d+)"
)
RE_RENUT_WORKLOAD = re.compile(
    r"\[renut workload\]\s+frames\s+(\d+)\s+\|\s+"
    r"draws/f\s+avg\s+([0-9.]+)\s+max\s+(\d+)\s+\|\s+"
    r"verts/f\s+avg\s+([0-9.]+)k\s+max\s+(\d+)\s+\|\s+"
    r"memexport/f\s+avg\s+([0-9.]+)\s+max\s+(\d+)\s+\|\s+"
    r"stream_cv/f\s+avg\s+([0-9.]+)\s+\(([0-9.]+)\s+MB/f\)\s+max\s+(\d+)\s+\|\s+"
    r"submits/f\s+avg\s+([0-9.]+)\s+max\s+(\d+)\s+\|\s+"
    r"resolves/f\s+avg\s+([0-9.]+)\s+max\s+(\d+)\s+\|\s+"
    r"tex_up/f\s+avg\s+([0-9.]+)\s+\(([0-9.]+)\s+MB/f,\s+peak\s+([0-9.]+)\s+MB\)\s+\|\s+"
    r"pipelines_built\s+(\d+)"
)
RE_RENUT_STREAMS = re.compile(
    r"cached streams:\s+(\d+)\s+checks of\s+([0-9.]+)\s+MB\s+"
    r"\((\d+)\s+of them,\s+([0-9.]+)\s+MB,\s+of a copy already checked in the frame;\s+"
    r"(\d+)\s+answered by the interval memo,\s+([0-9.]+)\s+MB not read again;\s+"
    r"(\d+)\s+kicks,\s+(\d+)\s+fence queries\),\s+(\d+)\s+stale caught.*?,\s+(\d+)\s+found stale late"
)
RE_RENUT_MEM_RELEASED = re.compile(r"(\d+)\s+textures released under the reserve")
RE_DEKO3D = re.compile(
    r"\[renut\]\s+deko3d:\s+(\d+)\s+draws.*?(\d+)\s+pipeline binds\s+\((\d+)\s+state binds\);\s+"
    r"uam\s+(\d+)\s+compiles\s+\(avg\s+(\d+)\s+ms,\s+(\d+)\s+failed\),\s+DKSH cache\s+(\d+)\s+hits,\s+(\d+)\s+added;\s+"
    r"memory:\s+images\s+(\d+)\s+MB,\s+buffers\s+(\d+)\s+MB,\s+code\s+(\d+)\s+KB,\s+commands\s+(\d+)\s+KB;\s+"
    r"barriers:\s+(\d+)\s+primitives,\s+(\d+)\s+fragments,\s+(\d+)\s+full\s+\((\d+)\s+invalidating textures\);\s+"
    r"copy and 2D engines:\s+(\d+)\s+buffer copies,\s+(\d+)\s+texture uploads,\s+(\d+)\s+readbacks,\s+"
    r"(\d+)\s+image copies,\s+(\d+)\s+resolves,\s+(\d+)\s+blits"
)
RE_GPU_PROF_HEADER = re.compile(r"^GPU profile:\s+([0-9.]+)\s+ms per frame over\s+(\d+)\s+frames")
RE_GPU_PROF_LINE = re.compile(
    r"^\s+([0-9.]+)\s+ms\s+(.+?)"
    r"(?:\s+\(([0-9.]+)\s+draws a frame\))?"
    r"(?:\s+\[([0-9.]+)\s+M pixels shaded;\s+zcull\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\])?$"
)
RE_MEM_HEAP = re.compile(r"Memory:\s+heap\s+(\d+)\s+MB in use of\s+(\d+)\s+MB.*?(\d+)\s+MB of that free in .*?largest\s+(\d+)\s+MB")
RE_MEM_HEAP2 = re.compile(r"Memory:\s+heap\s+(\d+)\s+MB taken from it so far of\s+(\d+)\s+MB,\s+(\d+)\s+MB never handed out")
RE_MEM_GUEST = re.compile(r"Memory:\s+guest:\s+(\d+)\s+MB committed in\s+(\d+)\s+chunks")
RE_MEM_GPU = re.compile(r"Memory:\s+GPU:\s+(\d+)\s+render targets\s+(\d+)\s+MB.*?\s+(\d+)\s+textures\s+(\d+)\s+MB")
RE_MEM_HOST = re.compile(r"Memory:\s+host:\s+upload rings\s+(\d+)\s+MB,\s+stream cache\s+(\d+)\s+MB in a\s+(\d+)\s+MB pool")
RE_MEM_GPUREG = re.compile(r"Memory:\s+GPU region\s+(\d+)\s+MB.*?(\d+)\s+blocks\s+(\d+)\s+MB\s+\(peak\s+(\d+)\)")


def parse_timestamp_sec(ts_str: str, t0: Optional[float]) -> Tuple[float, float]:
    # "2026-10-30 01:41:07.786" -> seconds since first log line
    hh, mm, ss = ts_str.split(" ")[1].split(":")
    day_sec = int(hh) * 3600 + int(mm) * 60 + float(ss)
    if t0 is None:
        return 0.0, day_sec
    rel = day_sec - t0
    if rel < -43200:
        rel += 86400.0
    return rel, t0


def parse_renut_log(path: Path) -> Dict[str, Any]:
    windows: List[Dict[str, Any]] = []
    boot_notes: List[str] = []
    warnings = collections.Counter()
    first_warns: List[str] = []

    t0: Optional[float] = None
    cur: Dict[str, Any] = {}
    in_gpu_profile = False
    gpu_passes_cur: List[Dict[str, Any]] = []
    prev_deko: Optional[Dict[str, int]] = None

    def ensure_cur(rel_t: float) -> Dict[str, Any]:
        nonlocal cur
        if not cur:
            cur = {"t_rel": rel_t}
        return cur

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()

    i = 0
    while i < len(lines):
        raw = lines[i]
        m_ts = RE_RENUT_TS.match(raw)
        if m_ts:
            in_gpu_profile = False
            ts_str, level, cat, msg = m_ts.groups()
            rel_t, t0 = parse_timestamp_sec(ts_str, t0)
            if level in ("warning", "error", "critical"):
                warnings[f"{level}:{cat}"] += 1
                if len(first_warns) < 20:
                    first_warns.append(f"[{rel_t:6.1f}s] [{level}] [{cat}] {msg[:180]}")

            if len(boot_notes) < 25 and not RE_DEKO3D.search(msg) and not RE_RENUT_MAIN.search(msg) and (
                "Renderer:" in msg or "Swapchain:" in msg or "deko3d] device ready" in msg
                or "GLSL package" in msg or "DKSH cache" in msg or "core_layout" in msg
                or "memory_policy" in msg or "content hashes:" in msg
            ):
                boot_notes.append(msg)

            m = RE_RENUT_MAIN.search(msg)
            if m:
                if cur and "fps" in cur:
                    windows.append(cur)
                    cur = {}
                w = ensure_cur(rel_t)
                w["idx"] = len(windows) + 1
                w["t_rel"] = rel_t
                w["dt"] = float(m.group(1))
                w["game_frames"] = int(m.group(2))
                w["fps"] = float(m.group(3))
                w["render_avg_ms"] = float(m.group(4))
                w["render_frames"] = int(m.group(5))
                w["gpu_avg_ms"] = float(m.group(6))
                w["gpu_frames"] = int(m.group(7))
                # Parse fallback draws count in case older logs lack [renut workload]
                m_dr = re.search(r";\s+(\d+)\s+draws,", msg)
                if m_dr and w["render_frames"] > 0:
                    w.setdefault("draws_per_frame", int(m_dr.group(1)) / w["render_frames"])
                ms = RE_RENUT_STREAMS.search(msg)
                if ms:
                    w["stream_checks"] = int(ms.group(1))
                    w["stream_mb"] = float(ms.group(2))
                    w["stream_same_frame_mb"] = float(ms.group(4))
                    w["stream_memo_saved_mb"] = float(ms.group(6))
                    w["kicks"] = int(ms.group(7))
                    w["fence_queries"] = int(ms.group(8))
                    w["stale_caught"] = int(ms.group(9))
                mr = RE_RENUT_MEM_RELEASED.search(msg)
                if mr:
                    w["tex_evicted"] = int(mr.group(1))
                i += 1
                continue

            mw = RE_RENUT_WAITS.search(msg)
            if mw:
                w = ensure_cur(rel_t)
                w["wait_fences_ms_s"] = float(mw.group(2))
                w["wait_swap_ms_s"] = float(mw.group(4))
                w["wait_vsync_ms_s"] = float(mw.group(6))
                w["wait_total_ms_s"] = w["wait_fences_ms_s"] + w["wait_swap_ms_s"] + w["wait_vsync_ms_s"]
                i += 1
                continue

            mp = RE_RENUT_PERF.search(msg)
            if mp:
                w = ensure_cur(rel_t)
                w["game_avg_ms"] = float(mp.group(3))
                w["game_p50_ms"] = float(mp.group(4))
                w["game_p95_ms"] = float(mp.group(5))
                w["game_p99_ms"] = float(mp.group(6))
                w["game_max_ms"] = float(mp.group(7))
                w["game_over33"] = int(mp.group(8))
                w["game_over50"] = int(mp.group(9))
                w["game_over100"] = int(mp.group(10))
                w["render_avg_ms"] = float(mp.group(12))
                w["render_p50_ms"] = float(mp.group(13))
                w["render_p95_ms"] = float(mp.group(14))
                w["render_p99_ms"] = float(mp.group(15))
                w["render_max_ms"] = float(mp.group(16))
                w["render_over33"] = int(mp.group(17))
                w["gpu_avg_ms"] = float(mp.group(19))
                w["gpu_p50_ms"] = float(mp.group(20))
                w["gpu_p95_ms"] = float(mp.group(21))
                w["gpu_p99_ms"] = float(mp.group(22))
                w["gpu_max_ms"] = float(mp.group(23))
                w["gpu_over33"] = int(mp.group(24))
                w["hitches_35ms"] = int(mp.group(25))
                i += 1
                continue

            mwl = RE_RENUT_WORKLOAD.search(msg)
            if mwl:
                w = ensure_cur(rel_t)
                w["draws_per_frame"] = float(mwl.group(2))
                w["draws_max"] = int(mwl.group(3))
                w["verts_k_per_frame"] = float(mwl.group(4))
                w["verts_max"] = int(mwl.group(5))
                w["memexport_per_frame"] = float(mwl.group(6))
                w["memexport_max"] = int(mwl.group(7))
                w["stream_cv_per_frame"] = float(mwl.group(8))
                w["stream_cv_mb_per_frame"] = float(mwl.group(9))
                w["submits_per_frame"] = float(mwl.group(11))
                w["resolves_per_frame"] = float(mwl.group(13))
                w["tex_up_per_frame"] = float(mwl.group(15))
                w["tex_up_mb_per_frame"] = float(mwl.group(16))
                w["tex_up_peak_mb"] = float(mwl.group(17))
                w["pipelines_built"] = int(mwl.group(18))
                i += 1
                continue

            md = RE_DEKO3D.search(msg)
            if md:
                w = ensure_cur(rel_t)
                cum = {
                    "draws": int(md.group(1)),
                    "binds": int(md.group(2)),
                    "state_binds": int(md.group(3)),
                    "uam_compiles": int(md.group(4)),
                    "dksh_hits": int(md.group(7)),
                    "dksh_added": int(md.group(8)),
                    "mem_images_mb": int(md.group(9)),
                    "mem_buffers_mb": int(md.group(10)),
                    "mem_code_kb": int(md.group(11)),
                    "mem_cmds_kb": int(md.group(12)),
                    "bar_frag": int(md.group(14)),
                    "bar_full": int(md.group(15)),
                    "bar_tex_inv": int(md.group(16)),
                    "tex_uploads": int(md.group(18)),
                    "img_copies": int(md.group(20)),
                    "resolves": int(md.group(21)),
                }
                w["deko_images_mb"] = cum["mem_images_mb"]
                w["deko_buffers_mb"] = cum["mem_buffers_mb"]
                w["deko_total_mb"] = cum["mem_images_mb"] + cum["mem_buffers_mb"] + (cum["mem_code_kb"] + cum["mem_cmds_kb"]) / 1024.0
                if prev_deko is not None:
                    w["deko_uam_delta"] = max(0, cum["uam_compiles"] - prev_deko["uam_compiles"])
                    w["deko_full_barriers_delta"] = max(0, cum["bar_full"] - prev_deko["bar_full"])
                    w["deko_tex_inv_delta"] = max(0, cum["bar_tex_inv"] - prev_deko["bar_tex_inv"])
                    w["deko_tex_uploads_delta"] = max(0, cum["tex_uploads"] - prev_deko["tex_uploads"])
                    w["deko_img_copies_delta"] = max(0, cum["img_copies"] - prev_deko["img_copies"])
                    w["deko_resolves_delta"] = max(0, cum["resolves"] - prev_deko["resolves"])
                else:
                    w["deko_uam_delta"] = cum["uam_compiles"]
                    w["deko_full_barriers_delta"] = cum["bar_full"]
                    w["deko_tex_inv_delta"] = cum["bar_tex_inv"]
                    w["deko_tex_uploads_delta"] = cum["tex_uploads"]
                    w["deko_img_copies_delta"] = cum["img_copies"]
                    w["deko_resolves_delta"] = cum["resolves"]
                prev_deko = cum
                i += 1
                continue

            mgp = RE_GPU_PROF_HEADER.match(msg)
            if mgp:
                w = ensure_cur(rel_t)
                w["gpu_prof_total_ms"] = float(mgp.group(1))
                w["gpu_prof_frames"] = int(mgp.group(2))
                gpu_passes_cur = []
                w["gpu_passes"] = gpu_passes_cur
                in_gpu_profile = True
                i += 1
                continue

            mh = RE_MEM_HEAP.search(msg)
            if mh:
                w = ensure_cur(rel_t)
                w["heap_used_mb"] = int(mh.group(1))
                w["heap_total_mb"] = int(mh.group(2))
                w["heap_largest_free_mb"] = int(mh.group(4))
            else:
                mh2 = RE_MEM_HEAP2.search(msg)
                if mh2:
                    w = ensure_cur(rel_t)
                    w["heap_used_mb"] = int(mh2.group(1))
                    w["heap_total_mb"] = int(mh2.group(2))
            mg = RE_MEM_GUEST.search(msg)
            if mg:
                ensure_cur(rel_t)["guest_committed_mb"] = int(mg.group(1))
            mgg = RE_MEM_GPU.search(msg)
            if mgg:
                w = ensure_cur(rel_t)
                w["rt_count"] = int(mgg.group(1))
                w["rt_mb"] = int(mgg.group(2))
                w["tex_count"] = int(mgg.group(3))
                w["tex_mb"] = int(mgg.group(4))
            mgh = RE_MEM_HOST.search(msg)
            if mgh:
                w = ensure_cur(rel_t)
                w["upload_rings_mb"] = int(mgh.group(1))
                w["stream_cache_mb"] = int(mgh.group(2))
                w["stream_pool_mb"] = int(mgh.group(3))
            mgr = RE_MEM_GPUREG.search(msg)
            if mgr:
                w = ensure_cur(rel_t)
                w["gpu_region_mb"] = int(mgr.group(1))
                w["gpu_region_used_mb"] = int(mgr.group(3))
                w["gpu_region_peak_mb"] = int(mgr.group(4))

        elif in_gpu_profile:
            mp_line = RE_GPU_PROF_LINE.match(raw)
            if mp_line:
                entry: Dict[str, Any] = {
                    "ms": float(mp_line.group(1)),
                    "label": " ".join(mp_line.group(2).split()),
                    "draws": float(mp_line.group(3)) if mp_line.group(3) else 0.0,
                    "mpix": float(mp_line.group(4)) if mp_line.group(4) else None,
                }
                if mp_line.group(5) is not None:
                    # 8 Zcull words (0..7)
                    entry["zcull"] = [int(mp_line.group(g)) for g in range(5, 13)]
                gpu_passes_cur.append(entry)
            elif raw.strip() == "" or raw.startswith("["):
                in_gpu_profile = False
        i += 1

    if cur and "fps" in cur:
        windows.append(cur)

    # Derive game_cpu_active_ms and wait_ms_per_frame
    for w in windows:
        if "game_avg_ms" not in w and w.get("fps", 0) > 0:
            w["game_avg_ms"] = 1000.0 / w["fps"]
        if "wait_total_ms_s" in w and w.get("fps", 0) > 0:
            w["wait_per_frame_ms"] = w["wait_total_ms_s"] / w["fps"]
            w["wait_fence_per_frame_ms"] = w["wait_fences_ms_s"] / w["fps"]
            w["wait_swap_per_frame_ms"] = w["wait_swap_ms_s"] / w["fps"]
            w["wait_vsync_per_frame_ms"] = w["wait_vsync_ms_s"] / w["fps"]
            w["game_cpu_active_ms"] = max(0.0, w["game_avg_ms"] - w["wait_per_frame_ms"])
        else:
            w["game_cpu_active_ms"] = None

    return {
        "path": str(path),
        "windows": windows,
        "boot_notes": boot_notes,
        "warnings": warnings,
        "first_warns": first_warns,
    }


RE_REX_HEADER = re.compile(
    r"^====\s+([0-9.]+)\s+s\s+\|\s+mode:\s+([^|]+)\|\s+game\s+([0-9.]+)\s+fps\s+\|\s+"
    r"CPU total\s+(\d+)%\s+\((\d+)% = \d+ cores\).*?\|\s+guest\s+(\d+)/(\d+)\s+MB.*?"
    r"process\s+(\d+)/(\d+)\s+MB\s+\|\s+samples\s+(\d+)"
)
RE_REX_FAULTS = re.compile(
    r"faults/s:\s+emulated read\s+(\d+),\s+SDK handler\s+(\d+),\s+emulated retry\s+(\d+),\s+SEH\s+(\d+),\s+physical committed\s+(\d+)"
)
RE_REX_CLOCKS = re.compile(
    r"^\s+clocks:\s+CPU\s+([0-9.]+)\s+MHz,\s+GPU\s+([0-9.]+)\s+MHz,\s+memory\s+([0-9.]+)\s+MHz\s+\|.*?SoC\s+([0-9.]+)\s+C"
)
RE_REX_PERFRAME = re.compile(
    r"^\s+per frame:\s+([0-9.]+)\s+draws.*?\|\s+([0-9.]+)\s+submits\s+\|\s+([0-9.]+)\s+resolves.*?\|.*?\s+"
    r"([0-9.]+)\s+textures loaded\s+\|\s+([0-9.]+)\s+MB of shared memory uploaded.*?pipelines created in the interval:\s+(\d+)"
)
RE_REX_LIBNX = re.compile(
    r"^\s+libnx per second.*?fences queried\s+(\d+)\s+\(([0-9.]+)\s+ms\),\s+waited\s+(\d+)\s+\(([0-9.]+)\s+ms\).*?"
    r"svcSleepThread:\s+0\s+(\d+),\s+yield\s+(\d+),\s+up to 1 ms\s+(\d+)\s+\(([0-9.]+)\s+ms\)"
)
RE_REX_THREAD = re.compile(r'^--\s+thread\s+\d+\s+"([^"]*)":\s+CPU\s+([0-9.]+)%')
RE_REX_PC = re.compile(r"^\s+[0-9.]+%\s+pc image\+0x([0-9a-f]+)\s+x(\d+)")
RE_REX_HITCH_HDR = re.compile(r"^==\s+during the hitches:\s+(\d+)\s+frames over 45 ms\s+\(([0-9.]+)\s+ms in total\)")
RE_REX_HITCH_THREAD = re.compile(r'^--\s+thread\s+"([^"]*)":\s+(\d+)\s+samples during the hitches,\s+([0-9.]+)% waiting')
RE_REX_HITCH_STACK = re.compile(r"^\s+([0-9.]+)%\s+stack\s+(.*)$")


def parse_rex_profile(path: Path) -> Dict[str, Any]:
    reports: List[Dict[str, Any]] = []
    cur: Optional[Dict[str, Any]] = None
    cur_thread: Optional[str] = None
    hitch_thread: Optional[str] = None
    in_hitches = False

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            mh = RE_REX_HEADER.match(line)
            if mh:
                cur = {
                    "idx": len(reports) + 1,
                    "t_s": float(mh.group(1)),
                    "mode": mh.group(2).strip(),
                    "fps": float(mh.group(3)),
                    "cpu_total_pct": int(mh.group(4)),
                    "cpu_cap_pct": int(mh.group(5)),
                    "guest_backing_mb": int(mh.group(6)),
                    "guest_mapped_mb": int(mh.group(7)),
                    "proc_used_mb": int(mh.group(8)),
                    "proc_total_mb": int(mh.group(9)),
                    "samples": int(mh.group(10)),
                    "threads": {},
                    "pc_samples": collections.defaultdict(collections.Counter),
                    "hitch_frames": 0,
                    "hitch_total_ms": 0.0,
                    "hitch_stacks": collections.defaultdict(list),
                }
                mf = RE_REX_FAULTS.search(line)
                if mf:
                    cur["fault_emulated"] = int(mf.group(1))
                    cur["fault_seh"] = int(mf.group(4))
                reports.append(cur)
                cur_thread = None
                hitch_thread = None
                in_hitches = False
                continue

            if cur is None:
                continue

            mhh = RE_REX_HITCH_HDR.match(line)
            if mhh:
                in_hitches = True
                cur_thread = None
                hitch_thread = None
                cur["hitch_frames"] = int(mhh.group(1))
                cur["hitch_total_ms"] = float(mhh.group(2))
                continue

            if in_hitches:
                mht = RE_REX_HITCH_THREAD.match(line)
                if mht:
                    hitch_thread = mht.group(1)
                    cur["hitch_stacks"][hitch_thread]  # ensure key
                    continue
                mhs = RE_REX_HITCH_STACK.match(line)
                if mhs and hitch_thread:
                    pct = float(mhs.group(1))
                    pcs = [int(x, 16) for x in re.findall(r"image\+0x([0-9a-f]+)", mhs.group(2))]
                    cur["hitch_stacks"][hitch_thread].append((pct, pcs))
                    continue
                if line.startswith("-- thread "):
                    in_hitches = False

            mc = RE_REX_CLOCKS.match(line)
            if mc:
                cur["cpu_mhz"] = float(mc.group(1))
                cur["gpu_mhz"] = float(mc.group(2))
                cur["mem_mhz"] = float(mc.group(3))
                cur["soc_c"] = float(mc.group(4))
                continue

            mpf = RE_REX_PERFRAME.match(line)
            if mpf:
                cur["draws_pf"] = float(mpf.group(1))
                cur["submits_pf"] = float(mpf.group(2))
                cur["resolves_pf"] = float(mpf.group(3))
                cur["tex_pf"] = float(mpf.group(4))
                cur["up_mb_pf"] = float(mpf.group(5))
                cur["pipelines_pf"] = int(mpf.group(6))
                continue

            ml = RE_REX_LIBNX.match(line)
            if ml:
                cur["fences_waited_s"] = int(ml.group(3))
                cur["fences_waited_ms_s"] = float(ml.group(4))
                cur["sleep_yield_s"] = int(ml.group(6))
                continue

            mt = RE_REX_THREAD.match(line)
            if mt:
                cur_thread = mt.group(1)
                cur["threads"][cur_thread] = cur["threads"].get(cur_thread, 0.0) + float(mt.group(2))
                continue

            mpc = RE_REX_PC.match(line)
            if mpc and cur_thread is not None:
                cur["pc_samples"][cur_thread][int(mpc.group(1), 16)] += int(mpc.group(2))
                continue

    return {"path": str(path), "reports": reports}


# ---------------------------------------------------------------------------
# Classification & reporting helpers
# ---------------------------------------------------------------------------

def classify_window(w: Dict[str, Any], target_fps: float = 29.0) -> str:
    fps = w.get("fps", 0.0)
    if fps <= 0.5:
        return "IDLE/LOAD"
    g_ms = w.get("game_avg_ms", 1000.0 / max(fps, 0.1))
    cpu_act = w.get("game_cpu_active_ms")
    rt_ms = w.get("render_avg_ms", 0.0)
    gpu_ms = w.get("gpu_avg_ms", 0.0)
    uam_d = w.get("deko_uam_delta", 0)
    pipes = w.get("pipelines_built", 0)

    if fps >= target_fps and w.get("game_p95_ms", g_ms) < 38.0:
        return "OK"
    reasons = []
    if uam_d >= 5 or pipes >= 15:
        reasons.append("SHADER/PSO-COMPILE")
    if cpu_act is not None and cpu_act >= max(rt_ms, gpu_ms) * 0.90 and cpu_act > 25.0:
        reasons.append("GAME-CPU")
    elif rt_ms >= gpu_ms * 1.15 and rt_ms > 22.0:
        reasons.append("RENDER-THREAD")
    elif gpu_ms > 28.0:
        reasons.append("GPU-BOUND")
    elif cpu_act is not None and (w.get("wait_per_frame_ms", 0.0) > cpu_act):
        reasons.append("WAIT/SYNC")
    if not reasons:
        # fallback compare render vs gpu
        if gpu_ms >= rt_ms and gpu_ms > 25.0:
            reasons.append("GPU")
        elif rt_ms > 25.0:
            reasons.append("RENDER-THREAD")
        else:
            reasons.append("MIXED/SPIKE")
    return "+".join(reasons[:2])


def discover_inputs(paths: List[str]) -> Tuple[List[Path], Optional[Path]]:
    search_dirs: List[Path] = []
    explicit_renut: List[Path] = []
    explicit_rex: Optional[Path] = None

    if not paths:
        for cand in [ROOT / "logs", ROOT / "output" / "switch" / "deadpool-nx" / "logs"]:
            if cand.is_dir():
                search_dirs.append(cand)
    else:
        for p_str in paths:
            p = Path(p_str)
            if p.is_dir():
                search_dirs.append(p)
            elif p.is_file():
                if "rex_profile" in p.name:
                    explicit_rex = p
                else:
                    explicit_renut.append(p)

    for d in search_dirs:
        if not explicit_renut:
            renut_logs = sorted(d.glob("renut_*.log"), key=lambda x: x.stat().st_mtime, reverse=True)
            if not renut_logs:
                renut_logs = sorted(d.glob("deadpool_*.log"), key=lambda x: x.stat().st_mtime, reverse=True)
            if renut_logs:
                explicit_renut.append(renut_logs[0])
        if explicit_rex is None:
            for cand in [d / "rex" / "rex_profile.log", d / "rex_profile.log"]:
                if cand.is_file():
                    explicit_rex = cand
                    break

    return explicit_renut, explicit_rex


def safe_mean(vals: List[float]) -> float:
    return sum(vals) / len(vals) if vals else 0.0


def match_rex_report(w: Dict[str, Any], rex_reports: List[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if not rex_reports:
        return None
    idx = w.get("idx", 1)
    best = None
    best_diff = 1e9
    # Match by similar game fps + approximate report index (after startup report)
    nonzero = [r for r in rex_reports if r["idx"] > 1]
    if not nonzero:
        return None
    if idx <= len(nonzero):
        cand = nonzero[idx - 1]
        if abs(cand["fps"] - w.get("fps", 0.0)) < 8.0 or len(nonzero) == 1:
            return cand
    for r in nonzero:
        d = abs(r["fps"] - w.get("fps", 0.0)) + abs(r["idx"] - idx) * 1.5
        if d < best_diff:
            best_diff = d
            best = r
    return best


def aggregate_pc_samples(reports: List[Dict[str, Any]], resolver: SymbolResolver,
                         top_n: int, thread_filter: str = "") -> Tuple[int, collections.Counter, Dict[str, collections.Counter]]:
    total = 0
    combined = collections.Counter()
    per_mod = collections.Counter()
    per_thread: Dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    for r in reports:
        for th, counts in r.get("pc_samples", {}).items():
            if thread_filter and thread_filter.lower() not in th.lower():
                continue
            for pc, cnt in counts.items():
                name, mod = resolver.resolve_pc_bucket(pc)
                combined[(name, mod)] += cnt
                per_mod[mod] += cnt
                per_thread[th][(name, mod)] += cnt
                total += cnt
    return total, combined, {"_modules": per_mod, **per_thread}


def print_report(renut_data: Optional[Dict[str, Any]], rex_data: Optional[Dict[str, Any]],
                 resolver: SymbolResolver, args: argparse.Namespace) -> None:
    print("=" * 108)
    print("DEADPOOL SWITCH UNIFIED CPU / GPU / RAM PERFORMANCE REPORT")
    print("=" * 108)
    if renut_data:
        print(f"Renut Log : {renut_data['path']}")
    if rex_data:
        print(f"Rex Log   : {rex_data['path']}")
    print(f"Symbols   : {resolver.source} ({len(resolver.starts)} entries)")

    if renut_data and renut_data["boot_notes"]:
        print("\n--- Environment & Config ---")
        for note in renut_data["boot_notes"][:12]:
            print(f"  {note}")

    windows = renut_data["windows"] if renut_data else []
    rex_reports = rex_data["reports"] if rex_data else []

    if windows:
        has_perf_dist = any("game_p95_ms" in w for w in windows)
        print("\n--- 10s Performance Timeline ---")
        hdr = (
            f"{'#':>2} {'Time':>6} {'FPS':>5} | "
            f"{'Game ms (avg/p95/max)':>21} | "
            f"{'CPU_Act':>7} {'Wait':>6} | "
            f"{'Rend(avg/p95)':>13} {'GPU(avg/p95)':>13} | "
            f"{'Draws/f':>7} {'Verts':>7} {'TexMB/f':>7} | "
            f"{'RAM(G/D)':>9} | {'Verdict'}"
        )
        print(hdr)
        print("-" * len(hdr))
        for w in windows:
            fps = w.get("fps", 0.0)
            g_avg = w.get("game_avg_ms", 0.0)
            g_p95 = w.get("game_p95_ms")
            g_max = w.get("game_max_ms")
            g_str = f"{g_avg:5.1f}/{g_p95:5.1f}/{g_max:5.1f}" if g_p95 is not None else f"{g_avg:5.1f}/  -  /  -  "

            c_act = w.get("game_cpu_active_ms")
            c_act_s = f"{c_act:7.2f}" if c_act is not None else "      -"
            wait_ms = w.get("wait_per_frame_ms")
            wait_s = f"{wait_ms:6.2f}" if wait_ms is not None else "     -"

            rt_avg = w.get("render_avg_ms", 0.0)
            rt_p95 = w.get("render_p95_ms")
            rt_s = f"{rt_avg:5.1f}/{rt_p95:5.1f}" if rt_p95 is not None else f"{rt_avg:5.1f}/  -  "

            gp_avg = w.get("gpu_avg_ms", 0.0)
            gp_p95 = w.get("gpu_p95_ms")
            gp_s = f"{gp_avg:5.1f}/{gp_p95:5.1f}" if gp_p95 is not None else f"{gp_avg:5.1f}/  -  "

            dr = w.get("draws_per_frame", 0.0)
            vk = w.get("verts_k_per_frame")
            vk_s = f"{vk:6.0f}k" if vk is not None else "      -"
            tx_mb = w.get("tex_up_mb_per_frame")
            tx_s = f"{tx_mb:7.2f}" if tx_mb is not None else "      -"

            g_mb = w.get("guest_committed_mb")
            d_mb = w.get("deko_total_mb")
            ram_s = f"{g_mb or 0:4.0f}/{d_mb or 0:4.0f}" if (g_mb is not None or d_mb is not None) else "    -    "

            verdict = classify_window(w, args.drop_fps)
            mark = " *DROP*" if fps > 0.5 and fps < args.drop_fps else ""
            print(
                f"{w['idx']:>2} {w['t_rel']:5.1f}s {fps:5.1f} | "
                f"{g_str:>21} | {c_act_s} {wait_s} | {rt_s:>13} {gp_s:>13} | "
                f"{dr:7.0f} {vk_s:>7} {tx_s} | {ram_s:>9} | {verdict}{mark}"
            )

        if not has_perf_dist:
            print("\n  [Note] This log came from an older build without [renut perf]/[renut workload]. Rebuild NRO + set deadpool_perf_log = true for percentiles & workload peaks.")

        # Calm vs Drop comparison
        active_wins = [w for w in windows if w.get("fps", 0.0) > 1.0]
        if len(active_wins) >= 2:
            drop_wins = [w for w in active_wins if w["fps"] < args.drop_fps]
            calm_wins = [w for w in active_wins if w["fps"] >= args.drop_fps]
            if not drop_wins or not calm_wins:
                sorted_w = sorted(active_wins, key=lambda x: x["fps"])
                mid = max(1, len(sorted_w) // 3)
                drop_wins = sorted_w[:mid]
                calm_wins = sorted_w[-mid:]

            print(f"\n--- Calm ({len(calm_wins)} windows, >= {args.drop_fps:.0f} FPS) vs NPC/Heavy Drop ({len(drop_wins)} windows) Comparison ---")
            metrics = [
                ("FPS", "fps", ".2f"),
                ("Game frame ms (avg)", "game_avg_ms", ".2f"),
                ("Game frame p95 ms", "game_p95_ms", ".2f"),
                ("Game CPU active ms", "game_cpu_active_ms", ".2f"),
                ("Game Wait total ms/f", "wait_per_frame_ms", ".2f"),
                ("  Wait: Render fences ms/f", "wait_fence_per_frame_ms", ".2f"),
                ("  Wait: Swap limit ms/f", "wait_swap_per_frame_ms", ".2f"),
                ("Render thread busy ms", "render_avg_ms", ".2f"),
                ("Render thread p95 ms", "render_p95_ms", ".2f"),
                ("GPU frame ms", "gpu_avg_ms", ".2f"),
                ("GPU frame p95 ms", "gpu_p95_ms", ".2f"),
                ("Draws / frame", "draws_per_frame", ".1f"),
                ("Vertices (k) / frame", "verts_k_per_frame", ".1f"),
                ("MemExport skinning draws/f", "memexport_per_frame", ".1f"),
                ("Stream converts MB / frame", "stream_cv_mb_per_frame", ".2f"),
                ("Submits / frame", "submits_per_frame", ".1f"),
                ("Resolves / frame", "resolves_per_frame", ".1f"),
                ("Texture uploads MB / frame", "tex_up_mb_per_frame", ".2f"),
                ("Stream checks / 10s", "stream_checks", ".0f"),
                ("Stream checks MB / 10s", "stream_mb", ".1f"),
                ("Full GPU barriers / 10s", "deko_full_barriers_delta", ".0f"),
                ("Texture invalidations / 10s", "deko_tex_inv_delta", ".0f"),
                ("Deko3D images+buffers MB", "deko_total_mb", ".0f"),
                ("Guest committed MB", "guest_committed_mb", ".0f"),
            ]
            for label, key, fmt in metrics:
                cv = [w[key] for w in calm_wins if w.get(key) is not None]
                dv = [w[key] for w in drop_wins if w.get(key) is not None]
                if not cv and not dv:
                    continue
                cm = safe_mean(cv)
                dm = safe_mean(dv)
                delta = dm - cm
                ratio_s = f"({dm / cm:.2f}x)" if cm > 0.001 else ""
                print(f"  {label:<32} Calm: {cm:8{fmt}}   |  Drop: {dm:8{fmt}}   |  Delta: {delta:+8{fmt}} {ratio_s}")

            # Automatic bottleneck diagnosis
            print("\n--- Bottleneck Diagnosis Summary ---")
            drop_rt = safe_mean([w.get("render_avg_ms", 0.0) for w in drop_wins])
            drop_gpu = safe_mean([w.get("gpu_avg_ms", 0.0) for w in drop_wins])
            drop_act = safe_mean([w["game_cpu_active_ms"] for w in drop_wins if w.get("game_cpu_active_ms") is not None])
            drop_wait = safe_mean([w["wait_per_frame_ms"] for w in drop_wins if w.get("wait_per_frame_ms") is not None])
            drop_dr = safe_mean([w.get("draws_per_frame", 0.0) for w in drop_wins])
            drop_uam = sum(w.get("deko_uam_delta", 0) for w in drop_wins)
            drop_evict = sum(w.get("tex_evicted", 0) for w in drop_wins)

            if drop_act > 25.0 and drop_act >= max(drop_rt, drop_gpu) * 0.9:
                print(f"  [PRIMARY] GAME-CPU BOUND during NPC combat (game_cpu_active avg {drop_act:.1f} ms/f).")
                print("            Check top guest functions (sub_*) in rex_profile.log during drop windows.")
            if drop_rt > 25.0 and drop_rt >= drop_gpu * 0.95:
                print(f"  [PRIMARY] RENDER-THREAD BOUND (avg {drop_rt:.1f} ms/f, ~{drop_dr:.0f} draws/f).")
                print("            Per-draw CPU work (stream hash/delta/descriptor setup on A57) limits FPS as draw count climbs.")
            if drop_gpu > 25.0:
                pct_msg = "PRIMARY" if drop_gpu > drop_rt else "CO-BOTTLENECK"
                print(f"  [{pct_msg}] GPU BOUND (avg {drop_gpu:.1f} ms/f). Check Top GPU Passes/Shaders & Zcull stats below.")
            if drop_wait > 8.0 and drop_wait > drop_act:
                print(f"  [SYNC] Game thread spending {drop_wait:.1f} ms/f waiting on renderer/fences/Swap limit.")
            if drop_uam > 5:
                print(f"  [STUTTER] {drop_uam} runtime UAM shader compiles during drop windows -> hitch source.")
            if drop_evict > 0:
                print(f"  [MEMORY] {drop_evict} textures evicted under reserve -> causes repeated upload churn during NPC fights.")

        # Top GPU passes / shaders in worst GPU windows
        gpu_prof_wins = [w for w in windows if w.get("gpu_passes")]
        if gpu_prof_wins:
            worst_w = max(gpu_prof_wins, key=lambda x: x.get("gpu_prof_total_ms", 0.0))
            print(f"\n--- GPU Pass / Shader Breakdown (Worst Window #{worst_w['idx']} @ {worst_w['t_rel']:.1f}s: "
                  f"{worst_w['gpu_prof_total_ms']:.2f} ms over {worst_w['gpu_prof_frames']} frames) ---")
            for p in worst_w["gpu_passes"][:args.top]:
                z_str = ""
                if "zcull" in p:
                    z = p["zcull"]
                    # Present Zcull counters raw (useful for comparing pass pixel pressure)
                    z_str = f" | zcull={z[0]} {z[1]} {z[2]} {z[3]}"
                mp_s = f" | {p['mpix']:.3f} Mpx" if p.get("mpix") is not None else ""
                print(f"  {p['ms']:7.3f} ms  ({p['draws']:6.1f} dr/f){mp_s}{z_str}  {p['label']}")

        # Memory & fragmentation summary
        mem_wins = [w for w in windows if "heap_used_mb" in w or "guest_committed_mb" in w or "deko_total_mb" in w]
        if mem_wins:
            first_m, peak_m, last_m = mem_wins[0], max(mem_wins, key=lambda w: w.get("guest_committed_mb", 0) + w.get("deko_total_mb", 0)), mem_wins[-1]
            print("\n--- Memory Summary (Start -> Peak -> End) ---")
            for label, key in [
                ("Guest committed (MB)", "guest_committed_mb"),
                ("Heap used (MB)", "heap_used_mb"),
                ("Heap largest free block (MB)", "heap_largest_free_mb"),
                ("Deko3D images (MB)", "deko_images_mb"),
                ("Deko3D buffers (MB)", "deko_buffers_mb"),
                ("Upload rings (MB)", "upload_rings_mb"),
                ("Stream cache (MB)", "stream_cache_mb"),
                ("Render targets (MB)", "rt_mb"),
                ("Host textures (MB)", "tex_mb"),
            ]:
                vals = [w[key] for w in mem_wins if key in w]
                if vals:
                    print(f"  {label:<30} start={vals[0]:<5.0f}  min={min(vals):<5.0f}  max={max(vals):<5.0f}  end={vals[-1]:<5.0f}")

    # Rex CPU & Hitch Stack Profile section
    if rex_reports:
        nonzero = [r for r in rex_reports if r["idx"] > 1 or r["fps"] > 0.1]
        print(f"\n--- RexSwitchPerf Windows ({len(nonzero)} active windows) ---")
        print(f"{'Win':>3} {'Time':>6} {'Mode':>9} {'FPS':>5} {'CPU%':>5} {'Clocks(CPU/GPU/MEM MHz)':>25} {'SoC':>5} {'GuestMB':>7} {'Draws/f':>7} {'Hitches':>7}")
        for r in nonzero:
            clk = f"{r.get('cpu_mhz', 0):.0f}/{r.get('gpu_mhz', 0):.0f}/{r.get('mem_mhz', 0):.0f}"
            hstr = f"{r['hitch_frames']} ({r['hitch_total_ms']:.0f}ms)" if r["hitch_frames"] else "0"
            print(
                f"{r['idx']:>3} {r['t_s']:5.1f}s {r['mode'][:9]:>9} {r['fps']:5.1f} {r['cpu_total_pct']:4d}% "
                f"{clk:>25} {r.get('soc_c', 0):4.1f}C {r['guest_backing_mb']:>7d} {r.get('draws_pf', 0):7.1f} {hstr:>7}"
            )

        # Split active rex_reports into drop vs calm or all non-startup
        drop_rex = [r for r in nonzero if r["fps"] > 0.5 and r["fps"] < args.drop_fps]
        target_rex = drop_rex if drop_rex else nonzero[1:] if len(nonzero) > 1 else nonzero
        scope_label = f"Drop windows (<{args.drop_fps:.0f} FPS, n={len(target_rex)})" if drop_rex else f"Active windows (n={len(target_rex)})"

        # Average thread CPU utilization
        th_totals = collections.defaultdict(list)
        for r in target_rex:
            for th, pct in r["threads"].items():
                th_totals[th].append(pct)
        if th_totals:
            print(f"\n--- Per-Thread CPU Usage [{scope_label}] ---")
            ranked_th = sorted(th_totals.items(), key=lambda kv: safe_mean(kv[1]), reverse=True)
            for th, vals in ranked_th[:15]:
                print(f"  {safe_mean(vals):5.1f}% avg (max {max(vals):5.1f}%)  {th}")

        # Hot PCs symbolized
        total_pcs, combined, extra = aggregate_pc_samples(target_rex, resolver, args.top, args.thread)
        if total_pcs:
            print(f"\n--- Top CPU Functions by Self-Time [{scope_label}, {total_pcs} samples] ---")
            per_mod = extra.pop("_modules")
            mod_str = ", ".join(f"{m}:{100.0*c/total_pcs:.1f}%" for m, c in per_mod.most_common(6))
            print(f"  Module split: {mod_str}")
            for (name, mod), cnt in combined.most_common(args.top):
                print(f"  {100.0 * cnt / total_pcs:5.1f}%  ({cnt:5d})  [{mod:8s}]  {name}")

            # Also show top Guest vs Renderer functions separately when available
            guest_funcs = [(k, c) for k, c in combined.most_common() if k[1] == "guest"]
            rend_funcs = [(k, c) for k, c in combined.most_common() if k[1] == "renderer"]
            if guest_funcs:
                print(f"\n  Top Guest Functions (sub_*) [{sum(c for _, c in guest_funcs)} samples]:")
                for (name, _), cnt in guest_funcs[:12]:
                    print(f"    {100.0 * cnt / total_pcs:5.1f}%  ({cnt:5d})  {name}")
            if rend_funcs:
                print(f"\n  Top Renderer Functions [{sum(c for _, c in rend_funcs)} samples]:")
                for (name, _), cnt in rend_funcs[:12]:
                    print(f"    {100.0 * cnt / total_pcs:5.1f}%  ({cnt:5d})  {name}")

        # Hitch call stacks
        hitch_reports = [r for r in nonzero if r.get("hitch_frames", 0) > 0]
        if hitch_reports:
            print(f"\n--- Hitch (>45ms frame) Call Stacks [{len(hitch_reports)} windows] ---")
            by_thread = collections.defaultdict(collections.Counter)
            for r in hitch_reports:
                for th, chains in r["hitch_stacks"].items():
                    for pct, pcs in chains:
                        names = [resolver.resolve_pc_bucket(pcs[0])[0]] if pcs else []
                        for pc in pcs[1:4]:
                            names.append(resolver.resolve_return(pc)[0])
                        by_thread[th][" <- ".join(names)] += pct
            for th, cntr in sorted(by_thread.items(), key=lambda kv: sum(kv[1].values()), reverse=True)[:5]:
                tot = sum(cntr.values()) or 1.0
                print(f"  Thread: {th}")
                for chain, wt in cntr.most_common(8):
                    print(f"    {100.0 * wt / tot:5.1f}%  {chain}")

    if renut_data and renut_data["warnings"]:
        print("\n--- Warnings / Errors Summary ---")
        print(f"  Counts: {dict(renut_data['warnings'].most_common(10))}")
        for line in renut_data["first_warns"][:10]:
            print(f"  {line}")

    if args.csv and windows:
        fieldnames = sorted({k for w in windows for k in w.keys() if k != "gpu_passes"})
        with open(args.csv, "w", newline="", encoding="utf-8") as cf:
            writer = csv.DictWriter(cf, fieldnames=fieldnames)
            writer.writeheader()
            for w in windows:
                writer.writerow({k: v for k, v in w.items() if k != "gpu_passes"})
        print(f"\nSaved timeline CSV -> {args.csv}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", help="Log directory or log files (renut_*.log, rex_profile.log)")
    ap.add_argument("--map", default=str(ROOT / "out" / "switch-ult" / "renut.map"), help="Path to renut.map")
    ap.add_argument("--elf", default="", help="Optional ELF fallback if renut.map is missing")
    ap.add_argument("--drop-fps", type=float, default=27.0, help="Threshold below which a window counts as an FPS drop")
    ap.add_argument("--top", type=int, default=20, help="Top N functions / GPU passes to display")
    ap.add_argument("--thread", default="", help="Filter CPU profile symbols to threads matching substring")
    ap.add_argument("--csv", default="", help="Export merged 10s metrics to CSV")
    args = ap.parse_args()

    renut_paths, rex_path = discover_inputs(args.inputs)
    if not renut_paths and rex_path is None:
        sys.exit("No logs found. Pass a log directory or explicit renut_*.log / rex_profile.log paths.")

    map_path = Path(args.map)
    if not map_path.is_file():
        alt = ROOT / "app" / "out" / "switch" / "renut.map"
        if alt.is_file():
            map_path = alt

    elf_path = Path(args.elf) if args.elf else None
    resolver = SymbolResolver(map_path if map_path.is_file() else None, elf_path)

    renut_data = parse_renut_log(renut_paths[0]) if renut_paths else None
    rex_data = parse_rex_profile(rex_path) if rex_path else None
    print_report(renut_data, rex_data, resolver, args)


if __name__ == "__main__":
    main()
