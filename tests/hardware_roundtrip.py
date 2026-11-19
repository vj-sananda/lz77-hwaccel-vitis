#!/usr/bin/env python3
"""End-to-end PCIe compression, independent zstd CLI decode, and exact comparison."""
import hashlib
import json
from pathlib import Path
import random
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
REPORT = ROOT / 'reports/hardware_roundtrip.json'
REPORT.parent.mkdir(parents=True, exist_ok=True)
random_bytes = random.Random(42)
text = (b'FPGA accelerated Zstandard compression with AMD Vitis L1.\n'
        b'The PCIe card compresses these bytes; software verifies lossless recovery.\n')
cases = [('empty', b'')]
for size in (1, 16, 63, 64, 65, 128, 129, 255, 256, 4096, 32767, 32768, 32769, 65536):
    cases.extend([(f'text_{size}', (text * (size // len(text) + 1))[:size]),
                  (f'random_{size}', random_bytes.randbytes(size)),
                  (f'repeated_{size}', b'A' * size)])
cases += [('prompt', (ROOT / 'PROMPT.md').read_bytes()),
          ('library_source', (ROOT / 'vendor/Vitis_Libraries/data_compression/L1/include/hw/zstd_compress.hpp').read_bytes()),
          ('text_1MiB', (text * (1048576 // len(text) + 1))[:1048576]),
          ('random_1MiB', random_bytes.randbytes(1048576))]
for byte in (0, 1, 3, 4, 127, 128, 255):
    cases.append((f'single_symbol_{byte}', bytes([byte]) * 32768))
for alphabet in (2, 8, 32):
    cases.append((f'alphabet_{alphabet}', bytes(random_bytes.randrange(alphabet) for _ in range(65536))))
records = []
with tempfile.TemporaryDirectory(prefix='fpga-zstd-test-') as folder:
    folder = Path(folder)
    for name, data in cases:
        original = folder / name
        compressed = folder / (name + '.zst')
        original.write_bytes(data)
        start = time.monotonic()
        run = subprocess.run([str(ROOT / 'build/fpga-zstd'), '--verify', str(original), str(compressed)],
                             capture_output=True, text=True, timeout=120)
        if run.returncode:
            raise RuntimeError(f'{name}: compressor failed\n{run.stdout}{run.stderr}')
        decoded = subprocess.run(['zstd', '-q', '-d', '-c', str(compressed)],
                                 check=True, capture_output=True, timeout=30).stdout
        assert decoded == data, name
        record = dict(name=name, input_bytes=len(data), compressed_bytes=compressed.stat().st_size,
                      sha256=hashlib.sha256(decoded).hexdigest(),
                      compressed_sha256=hashlib.sha256(compressed.read_bytes()).hexdigest(),
                      seconds=time.monotonic() - start,
                      result='PASS', utility_output=run.stdout)
        records.append(record)
        REPORT.write_text(json.dumps(records, indent=2) + '\n')
        print(f'PASS {name}: {len(data)} -> {compressed.stat().st_size} bytes', flush=True)
        if name == 'library_source':
            demo = ROOT / 'reports/demo'
            demo.mkdir(exist_ok=True)
            (demo / 'original.hpp').write_bytes(data)
            (demo / 'original.hpp.zst').write_bytes(compressed.read_bytes())
            (demo / 'recovered.hpp').write_bytes(decoded)
    # Failed invocations must not overwrite an existing destination.
    preserved = compressed.read_bytes()
    run = subprocess.run([str(ROOT / 'build/fpga-zstd'), str(original), str(compressed)], capture_output=True)
    assert run.returncode != 0 and compressed.read_bytes() == preserved
print(f'HARDWARE_ROUNDTRIP_PASS: {len(records)} cases; report {REPORT}')
