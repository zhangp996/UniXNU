#!/usr/bin/env python3
"""Run the imported protocol models without a device."""
from pathlib import Path
import shutil, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='qcom-baseband-') as name:
    for source in sorted((root / 'tests').glob('*model_test.c')):
        binary = Path(name) / source.stem
        subprocess.run([shutil.which('cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(root / 'baseband'), '-I', str(root / 'ipa'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
