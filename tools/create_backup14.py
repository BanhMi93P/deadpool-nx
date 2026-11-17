import os
import zipfile
import time
import hashlib

def make_backup():
    start_time = time.time()
    os.makedirs('backup', exist_ok=True)
    zip_path = 'backup/backup14.zip'

    exclude_root = {'.git', 'build', 'out', 'temp', 'backup', 'game', '.gemini', '.superpowers', '.vscode'}
    exclude_names = {'CMakeFiles', '.ninja_deps', '.ninja_log', 'CMakeCache.txt'}
    exclude_exts = {'.obj', '.o', '.a', '.tlog', '.idb', '.pdb'}

    files_to_zip = []
    total_bytes = 0

    print("Scanning files for backup14...")
    for root, dirs, files in os.walk('.'):
        rel_root = os.path.relpath(root, '.').replace('\\', '/')
        parts = rel_root.split('/')
        if parts[0] in exclude_root:
            dirs[:] = []
            continue
        if rel_root == 'sdk/out' or rel_root.startswith('sdk/out/') or rel_root == 'app/out' or rel_root.startswith('app/out/'):
            dirs[:] = []
            continue
        dirs[:] = [d for d in dirs if d not in exclude_names and not d.endswith('.dir') and d != '.git']

        for f in files:
            if f in exclude_names:
                continue
            _, ext = os.path.splitext(f)
            if ext.lower() in exclude_exts:
                continue
            fp = os.path.join(root, f)
            arcname = os.path.relpath(fp, '.').replace('\\', '/')
            try:
                sz = os.path.getsize(fp)
                files_to_zip.append((fp, arcname))
                total_bytes += sz
            except OSError:
                pass

    print(f"Total files to zip: {len(files_to_zip)} ({total_bytes / (1024 * 1024):.2f} MB uncompressed)")
    print(f"Writing to {zip_path}...")

    with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for idx, (fp, arcname) in enumerate(files_to_zip):
            if os.path.exists(fp):
                try:
                    zf.write(fp, arcname)
                except OSError:
                    pass
            if (idx + 1) % 5000 == 0 or idx + 1 == len(files_to_zip):
                print(f"  Processed {idx + 1}/{len(files_to_zip)} files...")

    duration = time.time() - start_time
    zip_size = os.path.getsize(zip_path)
    print(f"Backup14 created successfully in {duration:.1f}s: {zip_path} ({zip_size / (1024 * 1024):.2f} MB)")

    print("Testing zip integrity...")
    with zipfile.ZipFile(zip_path, 'r') as zf:
        test_res = zf.testzip()
        if test_res is None:
            print("Integrity check PASSED!")
        else:
            print(f"Integrity check FAILED on: {test_res}")

    print("Computing SHA-256...")
    h = hashlib.sha256()
    with open(zip_path, 'rb') as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    sha256 = h.hexdigest().upper()
    print(f"SHA-256: {sha256}")
    print(f"File count in zip: {len(zipfile.ZipFile(zip_path).namelist())}")
    print(f"Final size: {zip_size} bytes")

if __name__ == '__main__':
    make_backup()
