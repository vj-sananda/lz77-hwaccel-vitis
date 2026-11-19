CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
.PHONY: all hls rtl-test bitstream program test clean
all: build/fpga-zstd
build/fpga-zstd: host/fpga_zstd.cpp host/sequence_encoder.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -lzstd
hls:
	bash scripts/build_hls.sh
rtl-test:
	bash scripts/simulate_rtl.sh
bitstream:
	mkdir -p reports
	vivado -mode batch -nojournal -log reports/vivado.log -source scripts/build.tcl
program:
	bash scripts/program.sh
test: all
	python3 tests/hardware_roundtrip.py
	python3 tests/overwrite.py
clean:
	rm -f build/fpga-zstd
