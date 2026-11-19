#!/usr/bin/env python3
"""Check overwrite behavior, including preservation after an output-write failure."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
UTILITY = ROOT / 'build/fpga-zstd'


def invoke(*arguments, fails=False, **kwargs):
    result = subprocess.run([str(UTILITY), *map(str, arguments)],
                            capture_output=True, text=True, timeout=30, **kwargs)
    assert (result.returncode != 0) == fails, result.stdout + result.stderr
    return result


def decode(path):
    return subprocess.run(['zstd', '-q', '-d', '-c', str(path)],
                          check=True, capture_output=True, timeout=30).stdout


def limit_output_file():
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (1, 1))


with tempfile.TemporaryDirectory(prefix='fpga-zstd-overwrite-') as directory:
    folder = Path(directory)
    source = folder / 'input.txt'
    output = folder / 'output.zst'
    data = b'FPGA Zstandard overwrite regression test.\n' * 2000
    sentinel = b'Preserve the previous output until compression succeeds.\n' * 1000
    source.write_bytes(data)
    output.write_bytes(sentinel)

    result = invoke(source, output, fails=True)
    assert 'Output already exists' in result.stderr
    assert output.read_bytes() == sentinel
    print('PASS default overwrite refusal')

    invoke('-f', '--verify', source, output)
    assert decode(output) == data and source.read_bytes() == data
    print('PASS explicit output replacement and software round-trip')

    default_output = Path(str(source) + '.zst')
    default_output.write_bytes(sentinel)
    invoke('-f', '--verify', source)
    assert decode(default_output) == data
    print('PASS default output replacement')

    target = folder / 'symlink-target'
    target.write_bytes(sentinel)
    linked_output = folder / 'linked.zst'
    linked_output.symlink_to(target)
    invoke('-f', '--verify', source, linked_output)
    assert not linked_output.is_symlink() and decode(linked_output) == data
    assert target.read_bytes() == sentinel
    print('PASS symlink replacement preserves its target')

    hardlink = folder / 'input-hardlink'
    hardlink.hardlink_to(source)
    symlink = folder / 'input-symlink'
    symlink.symlink_to(source)
    for destination in (source, hardlink, symlink):
        result = invoke('-f', source, destination, fails=True)
        assert 'same file' in result.stderr
        assert source.read_bytes() == data and destination.read_bytes() == data
    print('PASS input protection for identical paths, hardlinks, and symlinks')

    output.write_bytes(sentinel)
    invoke('-f', '--device', folder / 'missing-device', source, output, fails=True)
    assert output.read_bytes() == sentinel
    print('PASS device-open failure preserves existing output')

    result = invoke('-f', '--verify', source, output, fails=True,
                    preexec_fn=limit_output_file)
    assert 'Output write:' in result.stderr
    assert output.read_bytes() == sentinel and not list(folder.glob('*.tmp.*'))
    print('PASS output-write failure preserves existing output and cleans temporary file')

print('OVERWRITE_TEST_PASS')
