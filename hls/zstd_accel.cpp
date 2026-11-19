// Hardware match finder from the Vitis L1 Zstandard compression pipeline.
#define DISABLE_DEPENDENCE
#define AVOID_STATIC_MODE
#include <ap_axi_sdata.h>
#include "zstd_compress_internal.hpp"

using Axis = ap_axiu<128, 0, 0, 0>;
constexpr unsigned BLOCK = 32768;
constexpr unsigned MAX_SEQUENCES = 16384;
constexpr unsigned MIN_BLOCK = 128;

static void receive(hls::stream<Axis>& input,
                    hls::stream<IntVectorStream_dt<8, 1>>& bytes,
                    hls::stream<ap_uint<96>>& metadata) {
    Axis header = input.read();
    unsigned size = header.data.range(63, 32);
    unsigned status = (header.data.range(31, 0) != 0x3254535a || size > BLOCK) ? 1 : 0;
    if (status) size = 0;
    ap_uint<96> meta = 0;
    meta.range(31, 0) = size;
    meta.range(63, 32) = header.data.range(95, 64);
    meta.range(95, 64) = status;
    metadata.write(meta);
    for (unsigned word = 0; word < BLOCK / 16; ++word) {
        Axis value = input.read();
        for (unsigned lane = 0; lane < 16; ++lane) {
#pragma HLS PIPELINE II=1
            if (size > MIN_BLOCK && word * 16 + lane < size) {
                IntVectorStream_dt<8, 1> v;
                v.strobe = 1;
                v.data[0] = value.data.range(lane * 8 + 7, lane * 8);
                bytes.write(v);
            }
        }
    }
    IntVectorStream_dt<8, 1> end;
    end.data[0] = 0;
    end.strobe = 0;
    if (size > MIN_BLOCK) bytes.write(end); // end of block
    bytes.write(end);                     // end of file
}

static void transmit(hls::stream<IntVectorStream_dt<32, 1>>& tokens,
                     hls::stream<ap_uint<96>>& metadata,
                     hls::stream<Axis>& output) {
    ap_uint<128> buffer[MAX_SEQUENCES];
#pragma HLS BIND_STORAGE variable=buffer type=ram_1p impl=bram
    auto meta = metadata.read();
    unsigned size = meta.range(31, 0);
    unsigned status = meta.range(95, 64);
    unsigned count = 0, literals = 0, consumed = 0;
    for (auto token = tokens.read(); token.strobe != 0; token = tokens.read()) {
#pragma HLS PIPELINE II=1
        unsigned match = token.data[0].range(15, 8);
        if (match) {
            ap_uint<128> seq = 0;
            seq.range(31, 0) = unsigned(token.data[0].range(31, 16)) + 1;
            seq.range(63, 32) = literals;
            seq.range(95, 64) = match;
            if (count < MAX_SEQUENCES) buffer[count] = seq;
            else status = 2;
            ++count;
            literals = 0;
            consumed += match;
        } else {
            ++literals;
            ++consumed;
        }
    }
    if (size > MIN_BLOCK) {
        tokens.read(); // end of file after the block terminator
        if (consumed != size) status = 3;
    } else literals = size;
    ap_uint<128> last = 0; // explicit block delimiter and final literal run
    last.range(63, 32) = literals;
    if (count < MAX_SEQUENCES) buffer[count] = last;
    else status = 2;
    ++count;
    Axis beat;
    beat.keep = -1;
    beat.strb = -1;
    beat.last = 0;
    beat.data = 0;
    beat.data.range(31, 0) = 0x3251535a;
    beat.data.range(63, 32) = count;
    beat.data.range(95, 64) = meta.range(63, 32);
    beat.data.range(127, 96) = status;
    output.write(beat);
    for (unsigned word = 0; word < MAX_SEQUENCES; ++word) {
#pragma HLS PIPELINE II=1
        beat.data = word < count ? buffer[word] : ap_uint<128>(0);
        beat.last = word == MAX_SEQUENCES - 1;
        output.write(beat);
    }
}

void zstd_accel(hls::stream<Axis>& input, hls::stream<Axis>& output) {
#pragma HLS INTERFACE axis port=input
#pragma HLS INTERFACE axis port=output
#pragma HLS INTERFACE ap_ctrl_hs port=return
#pragma HLS DATAFLOW
    hls::stream<IntVectorStream_dt<8, 1>> bytes("bytes");
    hls::stream<IntVectorStream_dt<32, 1>> matches("matches"), boosted("boosted");
    hls::stream<ap_uint<96>> metadata("metadata");
#pragma HLS STREAM variable=bytes depth=32
#pragma HLS STREAM variable=matches depth=32
#pragma HLS STREAM variable=boosted depth=32
#pragma HLS STREAM variable=metadata depth=2
    receive(input, bytes, metadata);
    // These are the LZ77 stages used by L1 Zstandard getLitSequences().
    xf::compression::lzCompress<BLOCK, uint32_t, 6, 3, BLOCK>(bytes, matches);
    xf::compression::lzBooster<255>(matches, boosted);
    transmit(boosted, metadata, output);
}
