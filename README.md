# FPGA-assisted Zstandard file compressor

`fpga-zstd` compresses regular files using the YPCB-00338-1P1 PCIe FPGA card.
The FPGA runs the **Vitis Data Compression L1 Zstandard LZ77 match finder and
match extension stages**. The host CPU validates those matches and uses libzstd
for entropy coding, checksums, and standard Zstandard frames. The CPU does not
run a replacement match finder. This is partial hardware offload; no speedup
over CPU-only Zstandard has been established.

The delivered design targets `xc7k480tffg1156-2`, uses PCIe Gen2 x8, and runs the
compression logic at **50 MHz**. It was built with Xilinx **2026.1** and validated
on the physical board on **2026-11-13**.

- [HOWTO.md](HOWTO.md): reproduce the environment, build, programming, and tests.
- [WORKFLOW.md](WORKFLOW.md): development stages, decisions, and validation evidence.
- [ZSTD_ACCELERATOR_ARCH.md](ZSTD_ACCELERATOR_ARCH.md): FPGA design, wire protocol,
  clocks, and interaction with the CPU.

## Run the program

The card must be programmed with `build/accelerator.bit`, the XDMA driver must
be loaded, and your user must have access to its device nodes. Follow
[HOWTO.md](HOWTO.md) on a new machine. On an already configured machine:

```bash
make
./build/fpga-zstd --verify input.txt input.txt.zst
zstd -d -c input.txt.zst > recovered.txt
cmp input.txt recovered.txt
```

`make` builds the host utility only. It requires a C++17 compiler and
`libzstd-dev`; the separate decode command requires `zstd`. The tested libzstd
version is 1.5.5. After a power cycle, run `make program` if the board has loaded
a different design from boot flash. Normal compression does not require sudo
once the programming helper has granted device access to your user.

```text
Usage: fpga-zstd [-f] [--verify] [--device /dev/xdma0] INPUT [OUTPUT.zst]
```

| Argument | Behavior |
| --- | --- |
| `INPUT` | Regular file to compress. The input is preserved. |
| `OUTPUT.zst` | Destination path; defaults to `INPUT.zst`. Its parent directory must exist and be writable. |
| `-f` | Replace an existing output after successful compression and any requested verification. |
| `--verify` | Decompress each completed frame with libzstd and compare it byte for byte with the original block before publishing the output. |
| `--device PREFIX` | XDMA device prefix, default `/dev/xdma0`. For example, `/dev/xdma1`; do not supply a channel suffix. |
| `-h`, `--help` | Print usage and exit. |

Examples:

```bash
# Default output: input.txt.zst
./build/fpga-zstd input.txt

# Replace that output and verify every frame
./build/fpga-zstd -f --verify input.txt

# Choose an output name
./build/fpga-zstd --verify input.txt archive.zst

# Select another XDMA device index carrying the same accelerator design
./build/fpga-zstd --device /dev/xdma1 --verify input.txt alternate.zst
```

Each example assumes the named input exists; destinations must be absent unless
`-f` is supplied. The program returns 0 on success or help, and 1 on a caught
error. It supports neither stdin/stdout streaming nor decompression mode. There
is no adjustable compression-level option, `--force` alias, or `--` option
separator. Refer to a filename beginning with `-` through a path such as
`./-filename`.

## Existing outputs and failures

By default, an existing output is refused. The final publication also refuses
to overwrite a file that appeared while compression was running.

With `-f`, output is written to a temporary file beside the destination,
flushed, and atomically renamed over the destination after successful
processing. The program rejects an output referring to the input's inode,
including hardlink and symlink aliases. If an existing output is a symlink to
another file, replacement removes that symlink itself; its target is untouched.
Existing directories and device files are rejected.

Caught failures remove the temporary file and leave the previous output in
place. Abrupt termination can leave a temporary file or queued DMA data. After
an aborted or failed DMA transfer, reprogram with `make program` before retrying.

## Read the result

For the validated Vitis source-file example, the utility reports:

```text
Compression achieved: 3.81:1; output size: 26.23% of input; size reduction: 73.77%
```

The calculations use the final `.zst` file size, including frame overhead:

```text
compression ratio = input bytes / output bytes
output size (%)   = 100 × output bytes / input bytes
size reduction (%) = 100 − output size (%)
```

The report also gives input/output byte counts, frame count, FPGA match count,
bytes covered by matches, elapsed time, and throughput in MiB/s. The timer
covers block processing, output flushing, and publication; it excludes initial
file/device setup and context allocation. `--verify` includes decompression and
comparison in the elapsed time.

Empty input produces a valid empty frame and reports ratio/percentages as
`n/a`. Small or incompressible input can expand: a one-byte input produced a
14-byte frame, or 1400% of input and a −1300% reduction.

| Validated input | Input bytes | Output bytes | Output / input | Reduction |
| --- | ---: | ---: | ---: | ---: |
| `PROMPT.md` | 623 | 382 | 61.32% | 38.68% |
| Vitis `zstd_compress.hpp` | 44,712 | 11,727 | 26.23% | 73.77% |
| Repeated text, 1 MiB (synthetic) | 1,048,576 | 17,373 | 1.66% | 98.34% |
| Random bytes, 1 MiB | 1,048,576 | 1,049,024 | 100.04% | −0.04% |

The retained source-file demonstration is in [reports/demo](reports/demo/).
The original hardware run passed 57 file cases, including a separate `zstd`
CLI decode and exact byte comparison. Later overwrite tests cover `-f`, input
alias protection, and preservation of existing output on failures. See
[WORKFLOW.md](WORKFLOW.md) for the evidence and its provenance.

## Operating limits and troubleshooting

Files are processed in bounded memory, in blocks of up to 32 KiB. Each block
becomes an independent, checksummed Zstandard frame; the output is their
concatenation. Standard `zstd -d -c` decodes all frames. There is no history
across frames. Blocks of at most 128 bytes take a literal-only hardware path.

| Symptom | Action |
| --- | --- |
| Device missing or permission denied | Check `/dev/xdma0_h2c_0`, `/dev/xdma0_c2h_0`, and `/dev/xdma0_user`; follow the driver/programming steps in [HOWTO.md](HOWTO.md). |
| FPGA identity mismatch | Load `build/accelerator.bit` with `make program`. Loopback and unrelated bitstreams are rejected before DMA. |
| FPGA is in use | Wait for the other compressor to finish. Only one cooperating client can use the channels at a time. |
| Short DMA, timeout, or response mismatch | Close clients, reprogram, and retry to clear stream state. |
| Output already exists | Choose another output path or use `-f`. |

Every run validates the FPGA's match ranges and actual match bytes, even without
`--verify`. A hardware or validation error stops compression; there is no CPU
match-finding fallback. The channel lock is advisory, so other applications
must also avoid concurrent access to the same DMA channels.
