#!/usr/bin/env python3
"""Map Vitis L1 UltraRAM directives to Kintex-7 block RAM."""
from pathlib import Path
import re
import shutil
import subprocess
root = Path(__file__).resolve().parent.parent
repo = root / 'vendor/Vitis_Libraries'
revision = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
if revision != 'b2c657d254ead080de401ee33133702f1577424a':
    raise RuntimeError('Unexpected Vitis library revision: ' + revision)
src = repo / 'data_compression/L1/include/hw'
dst = root / 'build/vitis_include'
shutil.copytree(src, dst, dirs_exist_ok=True)
for path in dst.glob('*.hpp'):
    text = path.read_text()
    text = re.sub(r'(impl\s*=\s*)URAM\b', r'\1BRAM', text, flags=re.I)
    text = text.replace('#pragma HLS RESOURCE variable = dict core = XPM_MEMORY uram',
                        '#pragma HLS BIND_STORAGE variable = dict type = ram_t2p impl = bram')
    path.write_text(text)
print('Prepared Kintex-7 BRAM include tree:', dst)
