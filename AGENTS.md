- Use the Xilinx 2026.1 installation available in the PATH

- The verilator simulator is available in the path. This verilator version supports timing constructs for building and running system verilog test benches.

- The board is ypcb_00338_1p1 for which the board files have been installed in the Xilinx 2026.1 installation. The associated git cloned repository is at `../fpga-board-ypcb_00338_1p1`

- PCIe xdma loop back designs have been confirmed to work. The working designs are at `../pcie-xdma-codex` and `../pcie-xdma-claude`

- The FPGA board is in the PCIe slot and is connected to the Xilinx Platform Cable USB JTAG interface.

- You can run any command that requires sudo , with sudo -n
