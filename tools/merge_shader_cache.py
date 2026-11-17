import re

with open('app/out/renut_switch/shader_cache_switch.cpp', 'r', encoding='utf-8', errors='ignore') as f:
    orig_cpp = f.read()

with open('out/test_capture/test.cpp', 'r', encoding='utf-8', errors='ignore') as f:
    test_cpp = f.read()

# 1. Parse test entries first so merge is idempotent against a previously merged file
entry_re = re.compile(r'^\t\{ (0x[0-9A-Fa-f]+), (\d+), (\d+), (\d+), (\d+), (0x[0-9A-Fa-f]+|\d+), (0x[0-9A-Fa-f]+|\d+), (0x[0-9A-Fa-f]+|\d+), (\{ [^}]+ \}), (\{ [^}]+ \}), (\d+), (\d+), (\d+), (\d+), (0x[0-9A-Fa-f]+|\d+) \},$', re.MULTILINE)
base_vi = 2397
base_lit = 25579

new_entries = []
new_hashes = set()
for m in entry_re.finditer(test_cpp):
    h = int(m.group(1), 16)
    vi_offset = int(m.group(11))
    vi_count = int(m.group(12))
    lit_offset = int(m.group(13))
    lit_count = int(m.group(14))

    new_vi_offset = (vi_offset + base_vi) if vi_count > 0 else 0
    new_lit_offset = (lit_offset + base_lit) if lit_count > 0 else 0

    new_line = f'\t{{ {m.group(1)}, 0, 0, 0, 0, {m.group(6)}, {m.group(7)}, {m.group(8)}, {m.group(9)}, {m.group(10)}, {new_vi_offset}, {vi_count}, {new_lit_offset}, {lit_count}, {m.group(15)} }},\n'
    new_entries.append((h, new_line))
    new_hashes.add(h)
assert len(new_entries) == 50

# 2. Parse base entries (excluding test hashes if already merged)
entry_line_re = re.compile(r'^\t\{ (0x[0-9A-Fa-f]+),.*?\n', re.MULTILINE)
existing_entries = []
for m in entry_line_re.finditer(orig_cpp):
    h = int(m.group(1), 16)
    if h not in new_hashes:
        existing_entries.append((h, m.group(0)))
assert len(existing_entries) == 5849

all_entries = existing_entries + new_entries
all_entries.sort(key=lambda x: x[0])
assert len(all_entries) == 5899

# Verify strictly increasing
for i in range(len(all_entries) - 1):
    assert all_entries[i][0] < all_entries[i+1][0]

# 3. Parse VI
orig_vi_match = re.search(r'ShaderVertexInput g_shaderVertexInputs\[\] = \{(.*?)\};', orig_cpp, re.DOTALL)
orig_vi_lines = [l.strip() for l in orig_vi_match.group(1).strip().splitlines() if l.strip()]
assert orig_vi_lines[-1] == '{ 0, 0, 0, 0, 0, 0, 0 },'
active_orig_vi = orig_vi_lines[:-1]
assert len(active_orig_vi) in (base_vi, base_vi + 60)
active_orig_vi = active_orig_vi[:base_vi]

test_vi_match = re.search(r'ShaderVertexInput g_shaderVertexInputs\[\] = \{(.*?)\};', test_cpp, re.DOTALL)
test_vi_lines = [l.strip() for l in test_vi_match.group(1).strip().splitlines() if l.strip()]
assert test_vi_lines[-1] == '{ 0, 0, 0, 0, 0, 0, 0 },'
active_test_vi = test_vi_lines[:-1]
assert len(active_test_vi) == 60

all_vi_lines = active_orig_vi + active_test_vi + ['{ 0, 0, 0, 0, 0, 0, 0 },']

# 4. Parse Literals
orig_lit_match = re.search(r'ShaderLiteral g_shaderLiterals\[\] = \{(.*?)\};', orig_cpp, re.DOTALL)
orig_lit_lines = [l.strip() for l in orig_lit_match.group(1).strip().splitlines() if l.strip()]
assert orig_lit_lines[-1] == '{ 0, { 0, 0, 0, 0 } },'
active_orig_lit = orig_lit_lines[:-1]
assert len(active_orig_lit) in (base_lit, base_lit + 268)
active_orig_lit = active_orig_lit[:base_lit]

test_lit_match = re.search(r'ShaderLiteral g_shaderLiterals\[\] = \{(.*?)\};', test_cpp, re.DOTALL)
test_lit_lines = [l.strip() for l in test_lit_match.group(1).strip().splitlines() if l.strip()]
assert test_lit_lines[-1] == '{ 0, { 0, 0, 0, 0 } },'
active_test_lit = test_lit_lines[:-1]
assert len(active_test_lit) == 268

all_lit_lines = active_orig_lit + active_test_lit + ['{ 0, { 0, 0, 0, 0 } },']

# Assemble new file
out_lines = []
out_lines.append('#include "shader_cache.h"\n')
out_lines.append('ShaderCacheEntry g_shaderCacheEntries[] = {\n')
for _, line in all_entries:
    out_lines.append(line)
out_lines.append('};\n')

out_lines.append('ShaderVertexInput g_shaderVertexInputs[] = {\n')
for l in all_vi_lines:
    out_lines.append('\t' + l + '\n')
out_lines.append('};\n')

out_lines.append('ShaderLiteral g_shaderLiterals[] = {\n')
for l in all_lit_lines:
    out_lines.append('\t' + l + '\n')
out_lines.append('};\n')

tail_match = re.search(r'(const uint8_t g_compressedDxilCache\[\].*)$', orig_cpp, re.DOTALL)
tail_text = tail_match.group(1)
tail_text = re.sub(r'const size_t g_shaderCacheEntryCount = \d+;', 'const size_t g_shaderCacheEntryCount = 5899;', tail_text)
out_lines.append(tail_text)

full_output = ''.join(out_lines)
with open('app/out/renut_switch/shader_cache_switch.cpp', 'w', encoding='utf-8') as f:
    f.write(full_output)

print('Successfully written merged shader_cache_switch.cpp with 5899 entries!')
