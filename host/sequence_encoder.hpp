#pragma once
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Zstd entropy coding of FPGA-supplied matches. No CPU match finder is called.
class SequenceEncoder {
    ZSTD_CCtx* context_;
    static size_t checked(size_t result) {
        if (ZSTD_isError(result)) throw std::runtime_error(ZSTD_getErrorName(result));
        return result;
    }
public:
    SequenceEncoder() : context_(ZSTD_createCCtx()) {
        if (!context_) throw std::runtime_error("Cannot allocate Zstandard context");
        try {
            checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_compressionLevel, 3));
            checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_minMatch, 3));
            checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_windowLog, 15));
            checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_checksumFlag, 1));
            checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters));
            checked(ZSTD_CCtx_setParameter(context_, ZSTD_c_validateSequences, 1));
        } catch (...) { ZSTD_freeCCtx(context_); throw; }
    }
    ~SequenceEncoder() { ZSTD_freeCCtx(context_); }
    SequenceEncoder(const SequenceEncoder&) = delete;
    std::vector<unsigned char> encode(const unsigned char* source, size_t size,
                                     const std::vector<ZSTD_Sequence>& sequences,
                                     size_t& matched_bytes) {
        if (sequences.empty()) throw std::runtime_error("Missing FPGA block delimiter");
        size_t position = 0;
        matched_bytes = 0;
        for (size_t i = 0; i < sequences.size(); ++i) {
            const auto& seq = sequences[i];
            if (seq.litLength > size - position) throw std::runtime_error("FPGA literal count exceeds input");
            position += seq.litLength;
            if (i + 1 == sequences.size()) {
                if (seq.offset || seq.matchLength || position != size)
                    throw std::runtime_error("Invalid FPGA block delimiter");
            } else {
                if (seq.matchLength < 3 || seq.matchLength > size - position || !seq.offset || seq.offset > position)
                    throw std::runtime_error("Invalid FPGA match range");
                // Check actual match bytes as well as the ranges checked by libzstd.
                if (std::memcmp(source + position, source + position - seq.offset, seq.matchLength))
                    throw std::runtime_error("FPGA match does not match the original bytes");
                position += seq.matchLength;
                matched_bytes += seq.matchLength;
            }
        }
        std::vector<unsigned char> output(ZSTD_compressBound(size));
        size_t result = checked(ZSTD_compressSequences(context_, output.data(), output.size(),
                                sequences.data(), sequences.size(), source, size));
        output.resize(result);
        return output;
    }
};
