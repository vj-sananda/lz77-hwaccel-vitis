#include <ap_axi_sdata.h>
#include <hls_stream.h>
#include "../host/sequence_encoder.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using Axis = ap_axiu<128, 0, 0, 0>;
void zstd_accel(hls::stream<Axis>&, hls::stream<Axis>&);
static unsigned failures = 0;
static void check(const std::vector<unsigned char>& data, unsigned tag) {
    hls::stream<Axis> in, out;
    Axis a; a.keep = -1; a.strb = -1; a.last = 0; a.data = 0;
    a.data.range(31, 0) = 0x3254535a;
    a.data.range(63, 32) = data.size();
    a.data.range(95, 64) = tag;
    in.write(a);
    for (unsigned pos = 0; pos < 32768; pos += 16) {
        a.data = 0;
        for (unsigned k = 0; k < 16; ++k)
            if (pos + k < data.size()) a.data.range(k * 8 + 7, k * 8) = data[pos + k];
        a.last = pos == 32752; in.write(a);
    }
    zstd_accel(in, out);
    a = out.read();
    unsigned count = a.data.range(63, 32);
    if (a.data.range(31, 0) != 0x3251535a || a.data.range(95, 64) != tag ||
        a.data.range(127, 96) != 0 || count > 16384) std::abort();
    std::vector<ZSTD_Sequence> sequences;
    std::vector<unsigned char> decoded(data.size() + 1);
    for (unsigned pos = 0; pos < 16384; ++pos) {
        a = out.read();
        if (bool(a.last) != (pos == 16383) || a.keep != 65535) std::abort();
        if (pos < count) sequences.push_back({unsigned(a.data.range(31,0)), unsigned(a.data.range(63,32)),
                                             unsigned(a.data.range(95,64)), 0});
    }
    SequenceEncoder encoder;
    size_t matched;
    auto compressed = encoder.encode(data.data(), data.size(), sequences, matched);
    unsigned size = compressed.size();
    auto n = ZSTD_decompress(decoded.data(), decoded.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(n) || n != data.size() || !std::equal(data.begin(), data.end(), decoded.begin())) {
        std::fprintf(stderr, "FAIL tag=%u input=%zu compressed=%u decode=%s\n", tag, data.size(), size, ZSTD_getErrorName(n));
        char path[128];
        std::snprintf(path, sizeof(path), "/tmp/fpga_zstd_failure_%u.zst", tag);
        FILE* f = std::fopen(path, "wb");
        if (f) { std::fwrite(compressed.data(), 1, compressed.size(), f); std::fclose(f); }
        ++failures;
        return;
    }
    if (!in.empty() || !out.empty()) std::abort();
    std::printf("PASS tag=%u input=%zu compressed=%u matches=%u matched=%zu\n", tag, data.size(), size, count - 1, matched);
    std::fflush(stdout);
}
int main(int argc, char**) {
    std::mt19937 random(42);
    unsigned tag = 0;
    const char text[] = "FPGA accelerated Zstandard compression using Vitis L1. Lossless file compression.\n";
    for (unsigned size : {0, 1, 16, 63, 64, 65, 128, 129, 255, 256, 4096, 32767, 32768}) {
        std::vector<unsigned char> data(size);
        for (unsigned k = 0; k < size; ++k) data[k] = text[k % (sizeof(text) - 1)];
        check(data, tag++);
        for (auto& byte : data) byte = random() & 255;
        check(data, tag++);
        std::fill(data.begin(), data.end(), 'A');
        check(data, tag++);
    }
    if (argc == 1) {
        for (unsigned byte : {0, 1, 2, 3, 4, 127, 128, 254, 255})
            for (unsigned size : {129, 32768}) check(std::vector<unsigned char>(size, byte), tag++);
        // Exercise sparse alphabets, short matches, nonuniform histograms and
        // changing sequence tables across independent invocations.
        for (unsigned test = 0; test < 32; ++test) {
            unsigned size = 129 + random() % (32768 - 128);
            std::vector<unsigned char> data(size);
            for (unsigned k = 0; k < size; ++k) {
                unsigned value = random();
                if (test % 4 == 0) data[k] = value % 2;
                if (test % 4 == 1) data[k] = value % 8;
                if (test % 4 == 2) data[k] = value % 100 < 95 ? 'x' : value & 255;
                if (test % 4 == 3) data[k] = k > 100 && value % 4 ? data[k - 97] : value & 255;
            }
            check(data, tag++);
        }
    }
    return failures ? 1 : 0;
}
