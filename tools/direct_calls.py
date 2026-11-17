#!/usr/bin/env python3
# Direct calls to game functions that have no hook (post-codegen step, run after every codegen).
#
# (From masseffect-nx via sa2-nx; banjo-nx: Banjo's hook sources and its flat app/generated tree.)
# With GCC every recompiled function sub_X is a weak alias of __imp__sub_X (DEFINE_REX_FUNC, <project>_pch.h),
# so that one of our REX_HOOK_RAW(sub_X) replaces it at link time. The codegen always calls sub_X, and the
# compiler cannot inline a weak function into another (a hook could replace it): not within the same file
# and not with LTO. This step changes `sub_X(ctx, base);` into `__imp__sub_X(ctx, base);` when sub_X has no
# hook, which is exactly the same function, but can now be inlined.
#
# Any 82xxxxxx address that appears in the app or SDK sources or in the hook tomls (banjo-nx: not
# config/renut_funcs.toml, the list of every function boundary) counts as hooked (the
# D3D trace builds its hooks with sub_##addr). That is slightly more than needed and safe.
#
# It does not touch masseffect_init.cpp or masseffect_register.cpp: the dispatch table for indirect calls still
# points to sub_X and sees the hooks. It has to be run again after every codegen (tools/codegen.sh does).
#
# Usage: python3 tools/direct_calls.py [--undo] [--dry-run] [--check] [--gen <generated folder>] [--hooked-list <file>]
#   --dry-run: count only, write nothing. --check: exit 1 if unhooked sub_X calls are still not direct, or if the
#   tree has no direct calls at all (a codegen that lost the step). Exit 1 also if no recomp files are found.
#   --undo: turn every __imp__sub_X call back into sub_X.
#   --redo: undo + convert in one pass, writing only files whose text really changes (post_codegen.py uses it).
#   --hooked-list: a file with the sub_X names to treat as hooked, instead of scanning the sources.
#   Environment: REXSDK_DIR = SDK folder to scan for hooks (default <repo>/sdk).
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # repository root
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
SDK = os.environ.get('REXSDK_DIR') or os.path.join(ROOT, 'sdk')
SOURCES = [os.path.join(APP, 'src'), os.path.join(SDK, 'src'), os.path.join(SDK, 'include'),
           os.path.join(APP, 'config', 'renut_hooks.toml'),
           os.path.join(APP, 'config', 'renut_gpu_funcs.toml'), os.path.join(APP, 'renut_config.toml'), os.path.join(APP, 'renut_manifest.toml'),
           os.path.join(APP, 'CMakeLists.txt')]


# In C/C++ files only the code counts (from peggle2-nx, 2026-10-15; futari-nx found it first). Comments name addresses
# for the reader, and a native calls the original through __imp__sub_X: neither defines a hook. Before, 128 of the 592
# "hooked" names came only from them, and 208 calls to 28 functions stayed weak (not inlinable; 124 to sub_823E4A38
# because of a comment in cvar_menu.cpp). The .txt files (localized.txt among them) are still read whole; the tomls and CMakeLists.txt
# without their # comments (2026-10-19).
CODE_SUFFIXES = ('.cpp', '.cc', '.c', '.h', '.hpp', '.inc')


def CodeOnly(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    return re.sub(r'__imp__sub_[0-9A-Fa-f]{8}', ' ', text)


def _skip_legacy_source(path):
    norm = path.replace('\\', '/')
    skip_dirs = ('/app/src/me/', '/app/src/native/')
    if any(d in norm for d in skip_dirs):
        return True
    skip_files = (
        '/app/src/cpu/banjo_hot_guest.cpp', '/app/src/cpu/banjo_hot_guest.h',
        '/app/src/cpu/hot/banjo_hand_natives.h', '/app/src/cpu/katamari_stage_diag.cpp',
        '/app/src/cpu/banjo_pool_lock.cpp', '/app/src/cpu/banjo_reverb_native.cpp',
        '/app/src/cpu/banjo_xaudio_native.cpp', '/app/src/cpu/banjo_forms_selfcheck.cpp',
        '/app/src/cpu/blur_microbench.cpp',
        '/app/src/renut_engine/gpu/banjo_shared_hooks.h',
        '/app/src/renut_engine/gpu/d3d/hooks/state.cpp',
        '/app/src/renut_engine/gpu/guest/widescreen.cpp',
        '/app/src/renut_engine/gpu/guest/widescreen.h',
        '/app/src/renut_engine/gpu/guest/photo_capture.cpp',
        '/app/src/renut_engine/StopNSwap.cpp', '/app/src/renut_engine/cvar_menu.cpp',
        '/app/src/renut_engine/env_map_pool.cpp', '/app/src/renut_engine/frameHooks.cpp',
        '/app/src/renut_engine/frontend_debug_menu.cpp', '/app/src/renut_engine/hooks.cpp',
        '/app/src/renut_engine/mnk_controls.cpp', '/app/src/renut_engine/mnk_controls.h',
        '/app/src/renut_engine/shader_dump.cpp', '/app/src/renut_engine/sleep.h',
        '/app/src/renut_engine/thread_slots.cpp', '/app/src/renut_engine/townsfolk.cpp',
    )
    return any(norm.endswith(f) for f in skip_files)


def hooked():
    names = set()
    for root in SOURCES:
        if os.path.isfile(root):
            paths = [root]
        else:
            paths = [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs
                     if not f.endswith(('.a', '.obj', '.o', '.png', '.jpg', '.bin', '_all.inc'))
                     and not _skip_legacy_source(os.path.join(d, f))]
        for path in paths:
            try:
                text = open(path, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            if path.endswith(CODE_SUFFIXES):
                text = CodeOnly(text)
                # Only real Deadpool/SDK hook macros declare hooks in C/C++ sources.
                text = '\n'.join(re.findall(r'(?:DEADPOOL_RENUT_HOOK|PPC_HOOK|REX_HOOK)(?:_RAW)?\s*\([^)]*\)', text))
            elif path.endswith('.toml') or os.path.basename(path) == 'CMakeLists.txt':
                # banjo-nx (2026-10-19, CPU proposal 7): a # comment names addresses for the reader, not hooks.
                # renut_config.toml's comment on the scalar/vector alternators kept 99 calls to 24 functions on the
                # weak alias (not inlinable; 56 of them to sub_82CE9250).
                text = re.sub(r'(?m)(^|[ \t])#[^\n]*', ' ', text)
            if os.path.basename(path) == 'renut_config.toml':
                # non_argument_as_local's keep list names functions that keep r11/r12 in ctx, not hooks
                text = re.sub(r'non_argument_as_local_keep\s*=\s*\[.*?\]', ' ', text, flags=re.S)
            for m in re.findall(r'(?<![0-9A-Fa-f])(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])', text):
                names.add('sub_' + m.upper())
    return names


def main():
    undo = '--undo' in sys.argv
    # --redo: --undo and then the conversion, in memory, writing only the files whose final text differs. A new hook
    # then rewrites only the files that call it (the build recompiles those, not all ~138: mtime-based).
    redo = '--redo' in sys.argv
    dry = '--dry-run' in sys.argv or '--check' in sys.argv
    check = '--check' in sys.argv
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    if '--hooked-list' in sys.argv:
        with_hook = set(open(sys.argv[sys.argv.index('--hooked-list') + 1], encoding='utf-8').read().split())
    else:
        with_hook = hooked()
    call = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    direct = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    changed = 0
    files = 0
    seen = 0
    already_direct = 0
    for f in sorted(os.listdir(gen)):
        if not re.match(r'\w+_recomp\.\d+\.cpp$', f):  # sa2-nx: <project>_recomp.N.cpp
            continue
        path = os.path.join(gen, f)
        text = open(path, encoding='utf-8').read()
        seen += 1
        already_direct += len(direct.findall(text))
        if undo:
            new_value, n = direct.subn(lambda m: 'sub_%s(ctx, base);' % m.group(1), text)
        else:
            def change(m):
                return m.group(0) if 'sub_' + m.group(1) in with_hook else '__imp__sub_%s(ctx, base);' % m.group(1)
            source = direct.sub(lambda m: 'sub_%s(ctx, base);' % m.group(1), text) if redo else text
            new_value = call.sub(change, source)
            n = len(call.findall(source)) - len(call.findall(new_value))
        if new_value != text:
            files += 1
            if not dry:
                open(path, 'w', encoding='utf-8', newline='\n').write(new_value)
        changed += n
    if seen == 0:
        sys.exit('ERROR (post-codegen patch): direct_calls: no *_recomp.N.cpp in %s' % gen)
    if not undo and changed == 0 and already_direct == 0:
        sys.exit('ERROR (post-codegen patch): direct_calls: neither sub_X calls to convert nor __imp__ direct '
                 'calls in %s; the generated call shape changed' % gen)
    if check and not undo:
        print('direct_calls --check: %d direct calls present, %d still convertible' % (already_direct, changed))
        if changed or already_direct == 0:
            sys.exit(1)
        return
    print('%s: %d calls in %d files; %d hooked addresses respected' %
          ('undone' if undo else 'direct' + (' (dry-run)' if dry else ''), changed, files, len(with_hook)))


if __name__ == '__main__':
    main()
