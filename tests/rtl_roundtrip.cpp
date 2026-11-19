// Cycle-level test of synthesized HLS RTL with randomized AXI backpressure.
#include "Vzstd_accel.h"
#include <verilated.h>
#include "../host/sequence_encoder.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using Word = std::array<uint32_t, 4>;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "RTL FAIL: %s\n", message); std::exit(1); }
}
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vzstd_accel dut;
    std::mt19937 random(12345);
    std::vector<std::vector<unsigned char>> cases;
    const char text[] = "Zstandard compression in the PCIe FPGA. Vitis L1 lossless data compression.\n";
    for (unsigned size : {0, 1, 128, 129, 255, 256, 4096, 32768}) {
        std::vector<unsigned char> data(size);
        for (unsigned k = 0; k < size; ++k) data[k] = text[k % (sizeof(text) - 1)];
        cases.push_back(data);
    }
    cases.emplace_back(32768, 0);
    cases.emplace_back(32768, 3);
    cases.emplace_back(32768, 255);
    cases.emplace_back(32768);
    for (auto& byte : cases.back()) byte = random() & 255;
    cases.emplace_back(32768);
    for (auto& byte : cases.back()) byte = random() & 7;
    std::vector<Word> requests;
    for (unsigned tag = 0; tag < cases.size(); ++tag) {
        requests.push_back({0x3254535a, uint32_t(cases[tag].size()), tag, 0});
        for (unsigned pos = 0; pos < 32768; pos += 16) {
            Word word{};
            for (unsigned k = 0; k < 16; ++k)
                if (pos + k < cases[tag].size()) word[k / 4] |= uint32_t(cases[tag][pos + k]) << (8 * (k % 4));
            requests.push_back(word);
        }
    }
    dut.ap_clk = 0; dut.ap_rst_n = 0; dut.ap_start = 0;
    dut.input_r_TVALID = 0; dut.output_r_TREADY = 0;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        dut.ap_clk = 0; dut.eval(); dut.ap_clk = 1; dut.eval();
    }
    dut.ap_rst_n = 1; dut.ap_start = 1;
    size_t sent = 0, received = 0;
    unsigned tag = 0;
    bool holding = false;
    std::vector<unsigned char> response;
    unsigned char decoded[32768];
    SequenceEncoder encoder;
    for (uint64_t cycle = 0; cycle < 20000000; ++cycle) {
        dut.ap_clk = 0;
        if (!holding && sent < requests.size() && random() % 5 != 0) holding = true;
        dut.input_r_TVALID = holding;
        dut.input_r_TKEEP = 0xffff; dut.input_r_TSTRB = 0xffff;
        dut.input_r_TLAST = sent % 2049 == 2048;
        if (holding) for (unsigned lane = 0; lane < 4; ++lane) dut.input_r_TDATA[lane] = requests[sent][lane];
        dut.output_r_TREADY = random() % 7 != 0;
        dut.eval();
        bool tx = dut.input_r_TVALID && dut.input_r_TREADY;
        if (dut.output_r_TVALID && dut.output_r_TREADY) {
            check(dut.output_r_TKEEP == 0xffff, "TKEEP");
            check(bool(dut.output_r_TLAST) == (received % 16385 == 16384), "TLAST");
            for (unsigned k = 0; k < 16; ++k) response.push_back(dut.output_r_TDATA[k / 4] >> (8 * (k % 4)));
            ++received;
            if (received % 16385 == 0) {
                uint32_t header[4]; std::memcpy(header, response.data(), 16);
                check(header[0] == 0x3251535a && header[2] == tag && header[3] == 0, "response header");
                check(header[1] <= 16384 && header[1] > 0, "sequence count");
                std::vector<ZSTD_Sequence> sequences;
                for (unsigned i = 0; i < header[1]; ++i) {
                    uint32_t seq[4]; std::memcpy(seq, response.data() + 16 + i * 16, 16);
                    sequences.push_back({seq[0], seq[1], seq[2], 0});
                }
                size_t matched;
                auto compressed = encoder.encode(cases[tag].data(), cases[tag].size(), sequences, matched);
                size_t size = ZSTD_decompress(decoded, sizeof(decoded), compressed.data(), compressed.size());
                check(!ZSTD_isError(size), ZSTD_getErrorName(size));
                check(size == cases[tag].size() && (!size || !std::memcmp(decoded, cases[tag].data(), size)), "decompressed bytes");
                std::printf("RTL PASS tag=%u input=%zu compressed=%zu matched=%zu cycle=%llu\n", tag, size, compressed.size(), matched, (unsigned long long)cycle);
                std::fflush(stdout);
                response.clear(); ++tag;
            }
        }
        dut.ap_clk = 1; dut.eval();
        if (tx) { ++sent; holding = false; }
        if (tag == cases.size()) {
            check(sent == requests.size(), "unconsumed request data");
            dut.final();
            std::puts("RTL_ROUNDTRIP_PASS");
            return 0;
        }
    }
    std::fprintf(stderr, "Timeout: sent=%zu/%zu words, received=%zu, tag=%u\n", sent, requests.size(), received, tag);
    return 1;
}
