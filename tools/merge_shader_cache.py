#!/usr/bin/env python3
"""Merge a newly recompiled shader_cache.cpp (partial or extra captures) into
app/out/renut_switch/shader_cache_switch.cpp cleanly and idempotently.

Replaced entries (hashes present in --extra) take the new metadata, vertex
inputs, and literals; entries only in the base table keep their existing
metadata and their sliced vertex inputs / literals, compacted tightly with no
stale holes. Output keeps DXIL/SPIR-V offsets zeroed for Switch (deko3d reads
GLSL from deko3d_glsl.bin).
"""
import argparse
import os
import re
import sys

ENTRY_RE = re.compile(
    r'^\t\{ (0x[0-9A-Fa-f]+), (?:0x[0-9A-Fa-f]+|\d+), (?:0x[0-9A-Fa-f]+|\d+), '
    r'(?:0x[0-9A-Fa-f]+|\d+), (?:0x[0-9A-Fa-f]+|\d+), '
    r'(0x[0-9A-Fa-f]+|\d+), (0x[0-9A-Fa-f]+|\d+), (0x[0-9A-Fa-f]+|\d+), '
    r'(\{ [^}]+ \}), (\{ [^}]+ \}), (\d+), (\d+), (\d+), (\d+), (0x[0-9A-Fa-f]+|\d+) \},$',
    re.MULTILINE,
)


def parse_cache(text, path):
    entries = {}
    for m in ENTRY_RE.finditer(text):
        h = int(m.group(1), 16)
        entries[h] = (
            m.group(1),
            m.group(2),
            m.group(3),
            m.group(4),
            m.group(5),
            m.group(6),
            int(m.group(7)),
            int(m.group(8)),
            int(m.group(9)),
            int(m.group(10)),
            m.group(11),
        )
    if not entries:
        sys.exit(f'merge_shader_cache: no entries parsed from {path}')

    vi_m = re.search(r'ShaderVertexInput g_shaderVertexInputs\[\] = \{(.*?)\};', text, re.DOTALL)
    lit_m = re.search(r'ShaderLiteral g_shaderLiterals\[\] = \{(.*?)\};', text, re.DOTALL)
    vi_lines = [l.strip() for l in vi_m.group(1).strip().splitlines() if l.strip()]
    lit_lines = [l.strip() for l in lit_m.group(1).strip().splitlines() if l.strip()]
    if vi_lines[-1] != '{ 0, 0, 0, 0, 0, 0, 0 },' or lit_lines[-1] != '{ 0, { 0, 0, 0, 0 } },':
        sys.exit(f'merge_shader_cache: missing sentinel in {path}')

    layout_m = re.search(r'g_shaderCacheLayout\s*=\s*(\d+);', text)
    layout = int(layout_m.group(1)) if layout_m else None
    return entries, vi_lines[:-1], lit_lines[:-1], layout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='app/out/renut_switch/shader_cache_switch.cpp')
    ap.add_argument('--extra', default='out/deko3d_shaders_v2/recompiled_2628.cpp')
    ap.add_argument('--out', default='app/out/renut_switch/shader_cache_switch.cpp')
    a = ap.parse_args()

    base_text = open(a.base, 'r', encoding='utf-8', errors='ignore').read()
    extra_text = open(a.extra, 'r', encoding='utf-8', errors='ignore').read()

    base_entries, base_vi, base_lit, base_layout = parse_cache(base_text, a.base)
    extra_entries, extra_vi, extra_lit, extra_layout = parse_cache(extra_text, a.extra)
    if base_layout != extra_layout:
        sys.exit(f'merge_shader_cache: layout mismatch {base_layout} vs {extra_layout}')

    all_hashes = sorted(set(base_entries) | set(extra_entries))
    out_vi = []
    out_lit = []
    out_entry_lines = []
    updated = 0
    added = 0
    kept = 0

    for h in all_hashes:
        if h in extra_entries:
            if h in base_entries:
                updated += 1
            else:
                added += 1
            rec = extra_entries[h]
            src_vi, src_lit = extra_vi, extra_lit
        else:
            kept += 1
            rec = base_entries[h]
            src_vi, src_lit = base_vi, base_lit

        h_str, spec, vfm, tfm, me, fc, vi_off, vi_cnt, lit_off, lit_cnt, flags = rec
        if vi_off + vi_cnt > len(src_vi) or lit_off + lit_cnt > len(src_lit):
            sys.exit(f'merge_shader_cache: slice out of bounds for {h_str}')

        new_vi_off = len(out_vi) if vi_cnt > 0 else 0
        new_lit_off = len(out_lit) if lit_cnt > 0 else 0
        out_vi.extend(src_vi[vi_off:vi_off + vi_cnt])
        out_lit.extend(src_lit[lit_off:lit_off + lit_cnt])

        out_entry_lines.append(
            f'\t{{ {h_str}, 0, 0, 0, 0, {spec}, {vfm}, {tfm}, {me}, {fc}, '
            f'{new_vi_off}, {vi_cnt}, {new_lit_off}, {lit_cnt}, {flags} }},\n'
        )

    out_vi.append('{ 0, 0, 0, 0, 0, 0, 0 },')
    out_lit.append('{ 0, { 0, 0, 0, 0 } },')

    tail_match = re.search(r'(const uint8_t g_compressedDxilCache\[\].*)$', base_text, re.DOTALL)
    if not tail_match:
        sys.exit(f'merge_shader_cache: missing tail block in {a.base}')
    tail_text = re.sub(
        r'const size_t g_shaderCacheEntryCount = \d+;',
        f'const size_t g_shaderCacheEntryCount = {len(all_hashes)};',
        tail_match.group(1),
    )

    tmp_path = a.out + '.tmp'
    with open(tmp_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('#include "shader_cache.h"\n')
        f.write('ShaderCacheEntry g_shaderCacheEntries[] = {\n')
        f.writelines(out_entry_lines)
        f.write('};\n')
        f.write('ShaderVertexInput g_shaderVertexInputs[] = {\n')
        for l in out_vi:
            f.write(f'\t{l}\n')
        f.write('};\n')
        f.write('ShaderLiteral g_shaderLiterals[] = {\n')
        for l in out_lit:
            f.write(f'\t{l}\n')
        f.write('};\n')
        f.write(tail_text)
    os.replace(tmp_path, a.out)

    print(
        f'merge_shader_cache: {len(all_hashes)} total entries '
        f'(kept {kept}, updated {updated}, added {added}); '
        f'{len(out_vi) - 1} vertex inputs, {len(out_lit) - 1} literals -> {a.out}'
    )


if __name__ == '__main__':
    main()
