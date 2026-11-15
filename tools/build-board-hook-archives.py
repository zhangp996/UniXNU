#!/usr/bin/env python3
"""Build the two board hook archives the kernel link force-loads."""
import argparse
import platform
import shutil
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('tree', type=Path, help='layout tree (the directory holding scripts/ and build/)')
args = parser.parse_args()
tree = args.tree.resolve()
repo = Path(__file__).resolve().parents[1]


def tool(name):
    if platform.system() == 'Darwin':
        return ['xcrun', name]
    if name == 'ar' and shutil.which('llvm-ar'):
        return ['llvm-ar']
    return [name]


def compile_and_archive(source, outdir, archive):
    if archive.is_file():
        print(f'{archive} (prebuilt)')
        return
    outdir.mkdir(parents=True, exist_ok=True)
    obj = outdir / (source.stem + '.o')
    subprocess.run(tool('clang') + ['-target', 'arm64-apple-ios17.0', '-c', str(source), '-o', str(obj)], check=True)
    archive.unlink(missing_ok=True)
    subprocess.run(tool('ar') + ['rcs', str(archive), str(obj)], check=True)
    print(f'{archive} ({archive.stat().st_size} bytes)')


# This runs before the kernel step assembles the tree; the assembly lives in the core checkout.
search = [tree / 'scripts/data/native-storage-21H461-original-bindings.S',
          repo / 'scripts/data/native-storage-21H461-original-bindings.S',
          repo / 'r2/core/scripts/data/native-storage-21H461-original-bindings.S']
assembly = next((p for p in search if p.is_file()), None)
if assembly is None:
    raise SystemExit('native-storage-21H461-original-bindings.S is missing')
compile_and_archive(assembly, tree / 'build/io-storage-bindings', tree / 'build/io-storage-bindings/libIOStorageBindings.a')

# Board compatibility shims for components this baseline does not link: the touch HID device comes
# from the real ShengHidInput build and the S6SY791 report path is carried by SyntheticTouch glue,
# so only the button observer has no provider here.
compat = tree / 'build/board-compat/compat.c'
compat.parent.mkdir(parents=True, exist_ok=True)
compat.write_text("""/* Board compatibility shims for components this baseline does not link. */
void native_hid_buttons_observed(unsigned mask, int result)
{
    (void)mask;
    (void)result;
}
""")
compile_and_archive(compat, tree / 'build/board-compat', tree / 'build/board-compat/libBoardCompat.a')
