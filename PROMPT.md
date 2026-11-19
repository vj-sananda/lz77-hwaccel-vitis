- Generate a software file compression utility program that uses the PCIe FPGA card for hardware acceleration of the compression algorithm.

- Use the Vitis HLS compression libraries for Zstandard. See `VITIS_COMPRESSION_LIBRARIES_DESCRIPTION.md`
Use at Level 1 , L1

- Test the hardware accelerated compression utility by decompressing it in software and ensuring it matches the original text

- /goal build hardware accelerated compression utility program that can be invoked on the terminal command line to produce valid compressed output that can be decompressed successfully. It should report the compression achieved.