#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build reports
python3 scripts/prepare_library.py
# 2026.1 resolves input paths relative to the work directory's parent.
python3 - <<'PY'
from pathlib import Path
root = Path.cwd()
config = (root / 'hls/config.cfg').read_text()
config = config.replace('=hls/', '=' + str(root) + '/hls/')
config = config.replace('-Ibuild/', '-I' + str(root) + '/build/')
(root / 'build/hls.cfg').write_text(config)
PY
vitis-run --mode hls --csim --config build/hls.cfg --work_dir build/hls_cfg > reports/hls_csim.log 2>&1
v++ --compile --mode hls --config build/hls.cfg --work_dir build/hls_cfg > reports/hls_synthesis.log 2>&1
vitis-run --mode hls --package --config build/hls.cfg --work_dir build/hls_cfg > reports/hls_package.log 2>&1
