AMD’s **Vitis Data Compression Library** provides synthesizable HLS C++ for **lossless compression and decompression on FPGAs**, from individual algorithm blocks to complete accelerator examples.

| Algorithm / format | What it offers |
|---|---|
| **LZ4** | Compression and decompression, including streaming implementations and parallel-byte decompression. |
| **Snappy** | Compression and decompression, with streaming implementations. |
| **DEFLATE, gzip, zlib** | Fixed and dynamic Huffman compression; decompression supports dynamic, fixed, and stored blocks. |
| **Zstandard (Zstd)** | Compression and decompression, including multicore compression. The documented decompressor supports windows up to **128 KB**. |

These capabilities are documented in the [2025.1 library overview](https://github.com/Xilinx/Vitis_Libraries/blob/2025.1/data_compression/README.rst).

You can use it at three levels:

- **L1 — reusable HLS blocks:** LZ matching and dictionary reconstruction, Huffman encoding/decoding, format-specific encoding, and packing. Interfaces mostly use `hls::stream`, allowing blocks to run concurrently within an HLS dataflow pipeline.
- **L2 — complete accelerator kernels:** Streaming and memory-mapped examples, host code, and flows for emulation and hardware execution.
- **L3 — application integration:** Examples that include host/device management and software-facing APIs for offloading compression. See the [L3 documentation](https://github.com/Xilinx/Vitis_Libraries/blob/2025.1/data_compression/L3/README.rst).

For an RTL project, **L1 is the useful starting point**: integrate the C++ blocks into your HLS design, synthesize them, and export RTL/IP for Vivado. Parameters such as history size, block size, parallel-byte width, and core count let you trade FPGA resources against throughput and compression ratio. The source is available under Apache 2.0. [Overview and build flow](https://github.com/Xilinx/Vitis_Libraries/blob/2025.1/data_compression/README.rst)

**Current status:** AMD stopped maintaining this library starting with **Vitis 2025.2**. Earlier releases remain available, including [2025.1](https://github.com/Xilinx/Vitis_Libraries/tree/2025.1/data_compression), so it is an available source base for a new design, but compatibility with newer tools needs verification. [AMD maintenance notice](https://github.com/Xilinx/Vitis_Libraries#vitis-accelerated-libraries)