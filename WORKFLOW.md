# Development workflow and validation record

This document records how the working FPGA-assisted Zstandard compressor was
developed and how changes should be validated. [HOWTO.md](HOWTO.md) contains
the reproducible commands; [README.md](README.md) covers running the utility;
[ZSTD_ACCELERATOR_ARCH.md](ZSTD_ACCELERATOR_ARCH.md) describes the delivered
hardware and its host interface.

## Objective and completion criteria

[PROMPT.md](PROMPT.md) requested a terminal utility using the PCIe FPGA and
Vitis Data Compression **L1** Zstandard building blocks, producing valid
compressed output and reporting compression achieved. The acceptance check was
software decompression followed by exact comparison with the original input.

The completed implementation performs LZ77 match finding and match extension on
the FPGA, then entropy coding and Zstandard framing on the CPU using those
matches. It was built, programmed, and validated on the physical board. This
establishes working offload and lossless interoperability; it does not establish
a speed advantage over a CPU-only compressor.

The work followed these gates:

| Stage | Required evidence before proceeding |
| --- | --- |
| Algorithm selection | Sequences or frames that software can decode exactly across boundary and binary inputs |
| HLS C model | Valid sequences, complete input coverage, and successful software round trips |
| HLS RTL | Correct transfers under stalls and repeated invocations |
| Board implementation | Correct clocks/resets, final timing closure, and passing bus skew |
| Programming | Intended endpoint detached, correct JTAG part selected, DONE high and configuration CRC clear |
| Physical-board utility | Matching identity, valid DMA responses, and independent software decompression |
| CLI refinements | Accurate reporting and preservation of input/previous output on relevant failure paths |

## 1. Establish a working board and dependency baseline

The target is YPCB-00338-1P1 with `xc7k480tffg1156-2`. Xilinx **2026.1** tools
and the installed board definition were used. The neighboring
`../fpga-board-ypcb_00338_1p1` repository supplied board information. Previously
working XDMA loopback projects at `../pcie-xdma-codex` and
`../pcie-xdma-claude` established that PCIe, JTAG, and the host connection worked.
The delivered PCIe integration and programming/driver helpers derive from the
`pcie-xdma-codex` baseline.

Dependencies were pinned so that algorithm and driver changes would not be
confused with project changes:

| Dependency | Revision |
| --- | --- |
| Vitis Libraries, branch `2025.1` | `b2c657d254ead080de401ee33133702f1577424a` |
| Xilinx DMA IP drivers | `a918c5196a09f95756af244a607d543d9dce5bfb` |
| Host libzstd / zstd CLI | 1.5.5 |

Only `data_compression/L1` is needed from Vitis Libraries. Kintex-7 has no
UltraRAM, so [scripts/prepare_library.py](scripts/prepare_library.py) copies
the headers to a generated include tree and changes UltraRAM storage bindings
to BRAM, including the older dictionary `RESOURCE` pragma. The upstream tree
is preserved, and preparation refuses an unexpected revision.

## 2. Evaluate the complete L1 Zstandard encoder

The initial investigation exercised the complete L1 `zstdCompressCore` in C
simulation before committing to a hardware build. For the pinned library and
tested configuration, small-input edge cases, corrupt binary frames, and
stream underflow/stalls prevented broad lossless round trips. Exploratory
library fixes did not resolve the complete set of failures.

The investigation is retained in:

- [expanded_csim.log](reports/full_core_investigation/expanded_csim.log)
- [attempted_library_fixes.py](reports/full_core_investigation/attempted_library_fixes.py)

These results describe that experiment, not every version or configuration of
the upstream library. The exploratory fixes are not part of the delivered
build. In particular, reproduction does not run the investigation's patch file.

The resulting partition keeps the L1 Zstandard LZ77 stages in hardware and
uses libzstd's `ZSTD_compressSequences` for the remaining encoding. The host
supplies the FPGA's sequences directly; it does not substitute a CPU match
finder. This partition gives a standard software decoder a complete, valid
Zstandard frame while preserving the intended match-finding offload.

## 3. Define a bounded transaction and a shared correctness check

[hls/zstd_accel.cpp](hls/zstd_accel.cpp) wraps `lzCompress` and `lzBooster` with
fixed-size 128-bit AXI Stream packets. Each request carries at most 32 KiB of
input. Each response contains literal-length/match-length/offset records and a
final block delimiter. Fixed transfer sizes make DMA completion predictable.
The FPGA identity and request/response signatures identify protocol version 2.

Important implementation choices were:

- Bypass the match pipeline for input lengths of 0 through 128 bytes, returning
  a literal-only delimiter to avoid upstream lookahead underflow.
- Define `AVOID_STATIC_MODE` so dictionary state is reset between requests.
- Define `DISABLE_DEPENDENCE` so the upstream unsafe dependence assertions do
  not hide real dictionary hazards during synthesis.
- Preserve the original block on the CPU so it can supply literal bytes and
  independently check every reported match.
- Use one independent checksummed Zstandard frame per input block, bounding
  memory and preventing cross-request dictionary dependencies.

[host/sequence_encoder.hpp](host/sequence_encoder.hpp) is shared by the CLI,
HLS C-model test, and RTL test. It checks literal and match bounds, match bytes,
the final delimiter, and total input coverage before calling libzstd. This
makes sequence interpretation consistent across the three environments.

## 4. Validate C behavior, then generated RTL

`make hls` first runs the C model, then synthesizes and packages the IP. The
validated C model passed **89 cases**, covering boundary lengths, repeated
symbols, random input, and binary distributions. Its log is
[reports/hls_csim.log](reports/hls_csim.log).

The build script generates an absolute-path configuration because Xilinx 2026.1
resolves the relevant paths relative to the HLS work directory's parent. The
HLS target is 10 ns with 12.5% uncertainty. That target guides synthesis; it
does not set the board clock.

`make rtl-test` runs the generated RTL with Verilator. The C++ harness applies
randomized input gaps and output backpressure, holds `ap_start` high, checks
repeated transactions, and performs software round trips. **13 cases passed**;
see [reports/rtl_roundtrip.log](reports/rtl_roundtrip.log). Its explicit cycle
stepping uses `--no-timing`; generated `#0` initializers do not need event
scheduling in this harness.

## 5. Integrate PCIe, clocks, and resets

[scripts/create_design.tcl](scripts/create_design.tcl) connects the HLS core to
one H2C and one C2H XDMA stream. Each direction uses a 1024-beat asynchronous
FIFO between XDMA's 250 MHz domain and the compressor's **50 MHz** board-clock
domain. GPIO, LM73 I2C, and JTAG AXI access remain in the design. No external
DDR controller is instantiated.

Reset polarity required explicit attention. XDMA provides active-low reset;
`proc_sys_reset` propagates active-low external and auxiliary reset settings
after block-design validation. The unused auxiliary reset must therefore be
tied high. The script checks the propagated settings and derives the
compressor/output-FIFO reset in the 50 MHz domain. Tying the unused active-low
reset low would leave the core held in reset.

The HLS start input is permanently high. Requests advance by stream handshakes;
the host does not write a start register. A constant at GPIO channel 2,
BAR0 offset `0x8`, identifies the image as `0x5a535432` before DMA is allowed.

## 6. Close timing on the routed whole design

The first routed implementation missed setup timing by **34 ps** in the PCIe
logic. Post-route physical optimization alone did not resolve it. Routing with
`Explore`, followed by `AggressiveExplore` post-route physical optimization,
closed timing. This strategy is now part of
[scripts/build.tcl](scripts/build.tcl), so normal builds include it.

The limiting clock domain in that timing investigation was PCIe's internal
500 MHz `userclk1`, not the 50 MHz match finder. Evidence is retained in
[reports/initial_route_timing.rpt](reports/initial_route_timing.rpt) and
[reports/routing_explore_console.log](reports/routing_explore_console.log).

Final implementation results:

| Check | Recorded result |
| --- | ---: |
| Worst setup slack | +0.025 ns |
| Worst hold slack | +0.009 ns |
| Minimum reported bus-skew slack | +3.342 ns |
| LUTs | 21,377 (7.16%) |
| Registers | 24,634 (4.12%) |
| Block-RAM tiles | 118 (12.36%) |
| DSPs | 0 |

The final reports contain zero DRC errors and no unclocked or unconstrained
internal max-delay endpoints. Vendor IP warnings remain documented in DRC and
methodology reports. [scripts/check_implementation.tcl](scripts/check_implementation.tcl)
checks final setup/hold, timing-summary status, and reported bus skew before
publishing `build/accelerator.bit` and `build/accelerator.xsa`.

## 7. Program deliberately and validate on the physical board

The programming helper removes the intended PCIe endpoint before JTAG
reconfiguration, checks that DMA nodes are unused, programs volatile FPGA SRAM,
checks DONE/CRC, rescans PCIe, and loads the pinned XDMA driver if needed. The
PCIe enumeration was corrected to use `lspci -Dn -d 10ee:` with enumeration
failure treated as fatal. Boot flash is left unchanged.

The physical board reported **5 GT/s x8**, `DONE=1`, and `CRC_ERROR=0`. The
host utility then checked the protocol identity and exercised the actual DMA
path. **57 hardware file cases**, totaling **3,075,210 input bytes**, passed
both per-frame libzstd verification and separate `zstd -d -c` decoding with
exact byte comparison.

The hardware cases include empty input; lengths around the small-input bypass
and 32 KiB frame boundaries; repeated text; random bytes; symbols 0, 1, 3, 4,
127, 128, and 255; reduced alphabets; source files; and 1 MiB inputs. The demo
source, compressed file, recovered file, and original checksums are retained in
[reports/demo](reports/demo/).

## 8. Refine the CLI and check output handling

The utility was extended to report compressed output as a percentage of input:
`100 × output_bytes / input_bytes`, equivalent to `100 − reduction_percent`.
The smoke checks include ordinary, one-byte, and empty inputs; see
[reports/output_percentage.log](reports/output_percentage.log).

The `-f` option adds replacement of an existing output after successful
compression and optional verification. The utility writes a sibling temporary
file, flushes it, and publishes it atomically. The default path uses `link()`
to preserve no-overwrite behavior even if a destination appears during the
run; `-f` uses `rename()`. Input/output inode aliases are rejected.

[tests/overwrite.py](tests/overwrite.py) checks default refusal, explicit and
default output replacement, symlink-target preservation, input aliases, missing
devices, and a file-size-limit-induced write failure. The latter verifies that
the previous output survives and the temporary file is removed. These CLI
changes did not require a new FPGA image.

## Evidence and future changes

| Evidence | Location |
| --- | --- |
| C model / HLS build | [C simulation](reports/hls_csim.log), [synthesis](reports/hls_synthesis.log), [package](reports/hls_package.log) |
| RTL model | [Build log](reports/verilator_build.log), [round trips](reports/rtl_roundtrip.log) |
| Final physical implementation | [Timing](reports/timing_summary.rpt), [bus skew](reports/bus_skew.rpt), [utilization](reports/utilization.rpt), [DRC](reports/drc.rpt), [methodology](reports/methodology.rpt) |
| Device programming | [Programming log](reports/program.log), [detach log](reports/detach.log) |
| File round trips | [Hardware JSON](reports/hardware_roundtrip.json), [original console log](reports/hardware_roundtrip.log), [demo](reports/demo/) |
| Original validation metadata | [validation_summary.json](reports/validation_summary.json) |

The original validation snapshot is dated **2026-11-13** and predates the
output-percentage, `-f`, and documentation updates. Its source and executable
hashes describe that earlier state. It is not regenerated automatically by
`make`, and should not be treated as a manifest of all current files. The
recorded bitstream SHA-256 is:

```text
b7a0c2a6ad0b49af2ab555dd00e596bf41f5777b7afc4be634e0e8f68615c5ff
```

For another change, identify its affected stages and follow the incremental
build table in [HOWTO.md](HOWTO.md). Preserve earlier reports before rerunning
targets that overwrite them. Retain tool/dependency versions, new artifact
hashes, implementation reports, and software-decoding evidence together.
Compare compression ratios or throughput only for identified inputs and
equivalent verification/timing settings; the existing measurements contain no
CPU-only baseline.
