# Reproduce the FPGA-assisted Zstandard accelerator

Run commands from this project's root directory. This procedure builds the host
utility and FPGA image, installs the driver, programs the board, and checks that
software decompression reproduces the input. For everyday command-line usage,
see [README.md](README.md); for design details, see
[ZSTD_ACCELERATOR_ARCH.md](ZSTD_ACCELERATOR_ARCH.md).

The commands below describe the delivered build flow. The earlier investigation
and timing experiments in [WORKFLOW.md](WORKFLOW.md) do not need to be replayed.
Generated projects and reports can be overwritten by these steps. Archive any
results you want to retain before rebuilding or rerunning tests.

## 1. Prepare the host and board

The validated environment was:

| Component | Validated configuration |
| --- | --- |
| Host | Ubuntu 24.04, kernel `6.8.0-149-generic` |
| FPGA board | YPCB-00338-1P1, `xc7k480tffg1156-2`, in a PCIe slot |
| Programming cable | Xilinx Platform Cable USB JTAG |
| Xilinx tools | Vivado, Vitis HLS, and packaging tools from **2026.1** |
| C++ / Python | C++17 compiler; Python 3.12 (tests require at least 3.9) |
| Software Zstandard | libzstd and `zstd` CLI 1.5.5 |
| RTL simulator | Verilator 5.054, 2026-11-01 |
| Board definition | `tiferking.cn:ypcb003381p1:part0:1.0` |

The board and cable must be powered/connected, and Xilinx licenses and cable
drivers must be available. The supplied programming helpers expect one JTAG
target, one matching XC7K480T, and at most one enumerated Xilinx PCIe device.
The automated tests use `/dev/xdma0`; the runtime CLI can select another prefix.

Install the host packages if missing:

```bash
sudo -n apt-get update
sudo -n apt-get install build-essential git python3 libzstd-dev zstd \
  pciutils psmisc ripgrep mokutil "linux-headers-$(uname -r)"
```

Use the Xilinx 2026.1 tools already in `PATH`. On the validated machine, the
environment can be initialized with:

```bash
source /opt/2026.1/2026.1/Vitis/settings64.sh
export PATH="/opt/verilator-5.054/bin:$PATH"
command -v vivado v++ vitis-run verilator
vivado -version
v++ --version
verilator --version
zstd --version
python3 --version
uname -r
```

The Vitis settings script also sources the Vivado environment. Adapt installation
paths on another machine while keeping the same tool versions. The supplied RTL
test uses a C++ cycle-driven test bench and Verilator's `--no-timing` mode, even
though the installed simulator also supports SystemVerilog timing constructs.

The board files are already installed on the validated machine at
`/opt/2026.1/2026.1/data/boards/board_files/ypcb003381p1/1.0`.
For a fresh installation with the board repository at the neighboring path:

```bash
sudo -n mkdir -p /opt/2026.1/2026.1/data/boards/board_files
sudo -n cp -a ../fpga-board-ypcb_00338_1p1/ypcb003381p1 \
  /opt/2026.1/2026.1/data/boards/board_files/
```

Vivado's `data` directory resolves to this shared installation directory. The
design script searches for `*ypcb003381p1*` and fails if no board definition is
found. The board repository supplies the definition and pinout; the delivered
project does not require either neighboring PCIe loopback project to build.

## 2. Obtain the pinned L1 library

For a new workspace without `vendor/Vitis_Libraries`:

```bash
mkdir -p vendor
git clone --filter=blob:none --sparse --branch 2025.1 \
  https://github.com/Xilinx/Vitis_Libraries.git vendor/Vitis_Libraries
git -C vendor/Vitis_Libraries checkout --detach \
  b2c657d254ead080de401ee33133702f1577424a
git -C vendor/Vitis_Libraries sparse-checkout set data_compression/L1
git -C vendor/Vitis_Libraries rev-parse HEAD
```

Expected revision:

```text
b2c657d254ead080de401ee33133702f1577424a
```

If the checkout already exists, check its revision instead of cloning over it.
[scripts/prepare_library.py](scripts/prepare_library.py) refuses another revision.
It copies the L1 headers to `build/vitis_include` and maps UltraRAM storage
directives to BRAM for Kintex-7. It leaves the upstream checkout unchanged.
The library is Apache-2.0 licensed. Tool version 2026.1 and library branch
2025.1 are intentional, separate choices.

## 3. Build the host utility

```bash
make
./build/fpga-zstd --help
```

This compiles [host/fpga_zstd.cpp](host/fpga_zstd.cpp) with C++17 and `-lzstd`.
Help does not access the FPGA. The advanced sequence API must be exported by
the installed libzstd; the tested version is 1.5.5. A different version needs
validation even if it compiles successfully.

## 4. Simulate, synthesize, and package the HLS core

```bash
make hls
```

[scripts/build_hls.sh](scripts/build_hls.sh) prepares the headers and generates
`build/hls.cfg` with absolute source/include paths, then runs these commands:

```bash
vitis-run --mode hls --csim --config build/hls.cfg --work_dir build/hls_cfg
v++ --compile --mode hls --config build/hls.cfg --work_dir build/hls_cfg
vitis-run --mode hls --package --config build/hls.cfg --work_dir build/hls_cfg
```

Use the `make` target for normal reproduction: it performs preparation and saves
the command output in `reports/hls_csim.log`, `reports/hls_synthesis.log`, and
`reports/hls_package.log`. Failure stops the script before the next stage.

The validated C simulation passed 89 cases. Useful checks and output paths:

```bash
rg -c '^PASS tag=' reports/hls_csim.log
test -f build/hls_cfg/hls/syn/verilog/zstd_accel.v
test -f build/hls_cfg/hls/impl/ip/component.xml
```

The packaged IP is `xilinx.com:hls:zstd_accel:1.0`. Its HLS clock target is
10 ns (100 MHz); the board integration supplies **50 MHz**. An HLS timing
estimate is not the board's operating frequency.

## 5. Validate the synthesized RTL

```bash
make rtl-test
```

[scripts/simulate_rtl.sh](scripts/simulate_rtl.sh) compiles the generated Verilog
with Verilator and [tests/rtl_roundtrip.cpp](tests/rtl_roundtrip.cpp). The test
introduces input gaps and output backpressure, holds `ap_start` high as the
board does, and checks repeated requests through software Zstandard encoding
and decoding.

Expected result: 13 cases and `RTL_ROUNDTRIP_PASS`. Logs are
`reports/verilator_build.log` and `reports/rtl_roundtrip.log`. Run this after
HLS synthesis; the target does not automatically invoke `make hls`.

## 6. Build and check the complete FPGA image

```bash
make bitstream
```

This creates `build/accelerator/accelerator.xpr`, integrating the HLS IP with
XDMA, asynchronous stream FIFOs, board clocks/resets, GPIO, I2C, and JTAG AXI.
It runs synthesis and implementation with four jobs. Routing uses `Explore`,
followed by `AggressiveExplore` post-route physical optimization.

[scripts/check_implementation.tcl](scripts/check_implementation.tcl) reports
final timing, utilization, DRC, methodology, and bus skew. It requires passing
timing summary, nonnegative setup/hold slack, and passing reported bus skew
before copying the routed bitstream and exporting the hardware platform.
Successful completion prints:

```text
BUILD_PASS: .../build/accelerator.bit
```

| Artifact | Purpose |
| --- | --- |
| `build/accelerator.bit` | Image consumed by the programming helper |
| `build/accelerator.xsa` | Fixed hardware platform export including the bitstream |
| `reports/vivado.log` | Vivado build console log |
| `reports/timing_summary.rpt` | Final timing summary, including setup/hold checks |
| `reports/bus_skew.rpt` | Clock-crossing bus-skew checks |
| `reports/utilization.rpt` | Final whole-design resource usage |
| `reports/drc.rpt`, `reports/methodology.rpt` | Design-rule and methodology findings |
| `reports/accelerator_bd.tcl` | Export of the generated block design |

The recorded build closed timing at +0.025 ns setup slack and +0.009 ns hold
slack, with minimum reported bus-skew slack +3.342 ns. DRC reported zero errors;
vendor IP warnings remain in the saved reports. Assess the newly generated
reports rather than assuming the old margins apply to a rebuild.

## 7. Build the XDMA driver

```bash
bash scripts/build_driver.sh
```

The script obtains `https://github.com/Xilinx/dma_ip_drivers.git` when absent,
checks out commit `a918c5196a09f95756af244a607d543d9dce5bfb`, and builds both the
XDMA kernel module and tools. It refuses an existing checkout at another
revision. Logs are `reports/driver_build.log` and
`reports/driver_tools_build.log`; the module is
`vendor/dma_ip_drivers/XDMA/linux-kernel/xdma/xdma.ko`.

Build against the running kernel's headers. The tested driver reports version
2025.2.0 on kernel `6.8.0-149-generic`. The setup helper loads it with
`poll_mode=1`; this revision defaults to 10-second H2C and C2H transfer timeouts.

If Secure Boot is enabled, the setup helper signs the module using the existing
key and certificate at `/var/lib/shim-signed/mok/MOK.priv` and
`/var/lib/shim-signed/mok/MOK.der`. That certificate must already be enrolled.
The helper does not create keys or perform MOK enrollment; provision them on a
new Secure Boot host before proceeding.

The helper also leaves an already-loaded XDMA module in place. After changing
the driver or kernel, close all DMA clients and unload the old module before
programming/setup if it is loaded:

```bash
sudo -n rmmod xdma
```

## 8. Program the FPGA and attach the driver

Close programs using XDMA devices, then run as your ordinary user:

```bash
make program
```

[scripts/program.sh](scripts/program.sh) performs the following sequence:

1. Enumerate Xilinx PCIe endpoints, rejecting ambiguous multiple-card setups.
2. Use `sudo -n` to check open DMA nodes and detach the intended endpoint from
   Linux before reconfiguration.
3. Program `build/accelerator.bit` through Platform Cable USB at a 6 MHz JTAG
   clock; verify `DONE=1` and `CRC_ERROR=0`.
4. Rescan PCIe, require device `10ee:7021`, and load the driver if needed.
5. Assign XDMA device nodes to the invoking user with mode `0600` and print the
   link/device information.

Expected programming marker:

```text
PROGRAM_PASS: DONE=1 CRC_ERROR=0
```

The helper uses noninteractive sudo, so `sudo -n` must work for its privileged
steps. Programming updates volatile FPGA SRAM; it does not write boot flash.
If another image loads after power cycling, repeat this step.

Inspect the endpoint and required nodes:

```bash
lspci -Dnn -d 10ee:7021
lspci -vv -d 10ee:7021
ls -l /dev/xdma0_h2c_0 /dev/xdma0_c2h_0 /dev/xdma0_user
```

The validated link negotiated **5 GT/s x8**. `reports/program.log` records the
programmed image hash and configuration status; `reports/detach.log` records
the prior endpoint removal when applicable. BAR0 offset `0x8` must read
`0x5a535432`; the utility checks this automatically before any DMA.

## 9. Run the hardware regression and a manual round trip

```bash
set -o pipefail
make test | tee reports/hardware_test_console.log
```

`make test` builds the utility if necessary and runs both suites against the
programmed `/dev/xdma0`:

- [tests/hardware_roundtrip.py](tests/hardware_roundtrip.py): 57 file cases;
  `--verify` plus independent `zstd -d -c` and exact byte comparison. It writes
  `reports/hardware_roundtrip.json` and the demo files in `reports/demo/`.
- [tests/overwrite.py](tests/overwrite.py): default overwrite refusal, `-f`
  replacement, input alias protection, symlink handling, and preservation on
  device-open and output-write failures.

Expected final markers are `HARDWARE_ROUNDTRIP_PASS: 57 cases` and
`OVERWRITE_TEST_PASS`. A failure exits the target unsuccessfully. Tests cover
empty input, small-input boundaries, 32 KiB frame boundaries, larger files,
text, repeated symbols, reduced alphabets, and random bytes.

A repeatable manual example uses `-f` for its output:

```bash
mkdir -p build/manual-demo
./build/fpga-zstd -f --verify PROMPT.md build/manual-demo/prompt.zst
zstd -d -c build/manual-demo/prompt.zst > build/manual-demo/recovered.md
cmp PROMPT.md build/manual-demo/recovered.md
sha256sum PROMPT.md build/manual-demo/recovered.md
```

`cmp` succeeds silently and the two hashes must match. For a 623-byte unchanged
`PROMPT.md`, the recorded output was 382 bytes (61.32% of input). Different
input or software versions can change the compressed size.

## Incremental changes and troubleshooting

The Makefile's FPGA targets are explicit stages, not a complete dependency
graph. Run the required stages in order:

| Changed component | Required follow-up |
| --- | --- |
| CLI source | `make`, then relevant hardware/overwrite checks |
| Shared sequence encoder | `make`, `make hls`, `make rtl-test`, and `make test`; assess whether any HLS source/configuration also changed |
| HLS core, configuration, or prepared library rules | `make hls`, `make rtl-test`, `make bitstream`, `make program`, `make test` |
| Board integration Tcl, RTL, or constraints | `make bitstream`, `make program`, `make test` |
| Driver or running kernel | Rebuild the module, unload the old module when unused, then run programming/setup and hardware tests |
| Documentation | Check commands and links against sources; no hardware rebuild is needed |

`make clean` removes only `build/fpga-zstd`. It does not remove generated HLS or
Vivado products. `make bitstream` recreates the generated Vivado project with
`-force`; preserve any manual project changes outside that directory first.

| Failure | Where to look / recovery |
| --- | --- |
| Unexpected library or driver revision | Verify the pinned commits; scripts intentionally refuse other revisions. |
| HLS compile or C-model error | `reports/hls_csim.log`, then synthesis/package logs; confirm the generated absolute-path `build/hls.cfg`. |
| Missing HLS IP or RTL | Complete `make hls` before the RTL or bitstream targets. |
| Board definition missing | Install `ypcb003381p1/1.0` into the 2026.1 board directory. |
| Timing failure | Review final timing and bus-skew reports. The build must reach `BUILD_PASS`. |
| Multiple PCIe devices or JTAG targets | The automated helper requires an unambiguous target; explicitly adapt target selection before using a multi-card setup. |
| Driver insertion failure | Check running-kernel headers/module version and, with Secure Boot, the signing key and enrolled certificate. |
| Device permissions missing after rescan | With this bitstream already loaded and the module built, rerun `sudo -n bash scripts/host_setup.sh` from the ordinary user's shell. |
| Identity mismatch | Program the supplied image; an XDMA loopback image is insufficient. |
| DMA timeout, short transfer, or stale response | Close DMA clients and rerun `make program` to reset the stream pipeline before retrying. |

[reports/validation_summary.json](reports/validation_summary.json) is the
original validation snapshot, not an automatically regenerated build manifest.
Its host/source hashes predate the output-percentage, `-f`, and documentation
updates. The FPGA bitstream was unchanged by those updates. New test runs can
overwrite logs, JSON results, and demo files; archive them together with tool
versions and fresh hashes when retaining a new validation record.
