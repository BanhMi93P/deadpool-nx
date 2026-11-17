import os
import zipfile
import time


def make_backup():
    start_time = time.time()
    os.makedirs('backup', exist_ok=True)
    zip_path = 'backup/backup9.zip'

    exclude_root = {'.git', 'build', 'out', 'temp', 'backup', 'game', '.gemini', '.superpowers', '.vscode'}
    exclude_names = {'CMakeFiles', '.ninja_deps', '.ninja_log', 'CMakeCache.txt'}
    exclude_exts = {'.obj', '.o', '.a', '.tlog', '.idb', '.pdb'}

    files_to_zip = []
    total_bytes = 0

    print('Scanning files for backup9...')
    for root, dirs, files in os.walk('.'):
        rel_root = os.path.relpath(root, '.').replace('\\', '/')
        parts = rel_root.split('/')
        if parts[0] in exclude_root:
            dirs[:] = []
            continue
        if rel_root == 'sdk/out' or rel_root.startswith('sdk/out/') or rel_root == 'app/out' or rel_root.startswith('app/out/'):
            dirs[:] = []
            continue
        dirs[:] = [
            d for d in dirs
            if d not in exclude_names and not d.endswith('.dir') and d != '.git'
        ]

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

    print(f'Total files to zip: {len(files_to_zip)} ({total_bytes / (1024 * 1024):.2f} MB uncompressed)')
    print(f'Writing to {zip_path}...')

    with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for idx, (fp, arcname) in enumerate(files_to_zip):
            zf.write(fp, arcname)
            if (idx + 1) % 5000 == 0 or idx + 1 == len(files_to_zip):
                print(f'  Processed {idx + 1}/{len(files_to_zip)} files...')

    duration = time.time() - start_time
    zip_size = os.path.getsize(zip_path) / (1024 * 1024)
    print(f'Backup9 created successfully in {duration:.1f}s: {zip_path} ({zip_size:.2f} MB)')


if __name__ == '__main__':
    make_backup()
