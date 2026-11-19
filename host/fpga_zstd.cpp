#include "sequence_encoder.hpp"
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr size_t BLOCK = 32768, CAPACITY = 262144;
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) { require(fd >= 0, std::strerror(errno)); }
    ~Fd() { close(value); }
    Fd(const Fd&) = delete;
};
struct Buffer {
    unsigned char* bytes = nullptr;
    explicit Buffer(size_t size) {
        int rc = posix_memalign(reinterpret_cast<void**>(&bytes), 4096, size);
        require(rc == 0, std::strerror(rc));
    }
    ~Buffer() { free(bytes); }
};
void put32(unsigned char* p, unsigned value) {
    for (unsigned k = 0; k < 4; ++k) p[k] = (value >> (k * 8)) & 255;
}
unsigned get32(const unsigned char* p) {
    return unsigned(p[0]) | (unsigned(p[1]) << 8) | (unsigned(p[2]) << 16) | (unsigned(p[3]) << 24);
}
void dma(int fd, void* data, size_t size, bool send) {
    ssize_t n;
    do { n = send ? write(fd, data, size) : read(fd, data, size); } while (n < 0 && errno == EINTR);
    require(n >= 0, std::string(send ? "H2C: " : "C2H: ") + std::strerror(errno));
    require(size_t(n) == size, "Short DMA transfer; reprogram the FPGA before retrying");
}
void write_all(int fd, const unsigned char* data, size_t size) {
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, std::string("Output write: ") + std::strerror(errno));
        data += n; size -= n;
    }
}
}

int main(int argc, char** argv) {
    std::string temporary;
    try {
        std::string prefix = "/dev/xdma0";
        bool verify = false, force = false;
        std::vector<std::string> paths;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--verify") verify = true;
            else if (arg == "-f") force = true;
            else if (arg == "--device" && i + 1 < argc) prefix = argv[++i];
            else if (arg == "--help" || arg == "-h") {
                std::printf("Usage: fpga-zstd [-f] [--verify] [--device /dev/xdma0] INPUT [OUTPUT.zst]\n"
                            "Vitis L1 LZ77 matching on the PCIe FPGA; Zstandard entropy coding on the CPU.\n"
                            "--verify decompresses every output frame in software and compares bytes.\n"
                            "-f atomically replaces an existing output after successful compression.\n"
                            "Without -f, the output must not already exist. Requires the supplied FPGA bitstream.\n");
                return 0;
            } else {
                require(arg.empty() || arg[0] != '-', "Unknown option: " + arg);
                paths.push_back(arg);
            }
        }
        require(paths.size() == 1 || paths.size() == 2, "Usage: fpga-zstd [-f] [--verify] [--device /dev/xdma0] INPUT [OUTPUT.zst]");
        std::string output = paths.size() == 2 ? paths[1] : paths[0] + ".zst";
        struct stat st;
        int output_status = lstat(output.c_str(), &st);
        require(output_status == 0 || errno == ENOENT, "Output is inaccessible: " + output);
        if (output_status == 0) {
            require(force, "Output already exists (use -f to overwrite): " + output);
            require(S_ISREG(st.st_mode) || S_ISLNK(st.st_mode), "Output must be a regular file or symlink: " + output);
        }
        Fd input(open(paths[0].c_str(), O_RDONLY | O_CLOEXEC));
        require(fstat(input.value, &st) == 0 && S_ISREG(st.st_mode), "Input must be a regular file");
        struct stat destination;
        if (force && stat(output.c_str(), &destination) == 0) {
            require(st.st_dev != destination.st_dev || st.st_ino != destination.st_ino,
                    "Input and output refer to the same file");
        }
        Fd h2c(open((prefix + "_h2c_0").c_str(), O_WRONLY | O_CLOEXEC));
        require(flock(h2c.value, LOCK_EX | LOCK_NB) == 0, "FPGA is in use by another compressor");
        Fd c2h(open((prefix + "_c2h_0").c_str(), O_RDONLY | O_CLOEXEC));
        Fd control(open((prefix + "_user").c_str(), O_RDONLY | O_CLOEXEC));
        void* regs = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, control.value, 0);
        require(regs != MAP_FAILED, "Cannot map FPGA identity register");
        unsigned id = static_cast<volatile unsigned*>(regs)[2];
        munmap(regs, 4096);
        require(id == 0x5a535432, "FPGA identity mismatch: load build/accelerator.bit with scripts/program.sh");

        std::vector<char> pattern(output.begin(), output.end());
        const char suffix[] = ".tmp.XXXXXX";
        pattern.insert(pattern.end(), suffix, suffix + sizeof(suffix));
        Fd file(mkstemp(pattern.data()));
        temporary = pattern.data();
        Buffer tx(BLOCK + 16), rx(CAPACITY + 16);
        std::vector<unsigned char> decoded(BLOCK);
        SequenceEncoder encoder;
        uint64_t match_count = 0, total_matched = 0;
        uint64_t input_size = 0, output_size = 0, blocks = 0;
        auto start = std::chrono::steady_clock::now();
        while (true) {
            memset(tx.bytes, 0, BLOCK + 16);
            size_t size = 0;
            while (size < BLOCK) {
                ssize_t n = read(input.value, tx.bytes + 16 + size, BLOCK - size);
                if (n < 0 && errno == EINTR) continue;
                require(n >= 0, std::string("Input read: ") + std::strerror(errno));
                if (!n) break;
                size += n;
            }
            if (!size && blocks) break;
            put32(tx.bytes, 0x3254535a);
            put32(tx.bytes + 4, size);
            put32(tx.bytes + 8, static_cast<unsigned>(blocks));
            dma(h2c.value, tx.bytes, BLOCK + 16, true);
            dma(c2h.value, rx.bytes, CAPACITY + 16, false);
            unsigned sequence_count = get32(rx.bytes + 4);
            require(get32(rx.bytes) == 0x3251535a && get32(rx.bytes + 8) == static_cast<unsigned>(blocks),
                    "FPGA response signature or sequence mismatch; reprogram the FPGA");
            require(get32(rx.bytes + 12) == 0 && sequence_count > 0 && sequence_count <= CAPACITY / 16,
                    "FPGA returned an error or invalid sequence count");
            std::vector<ZSTD_Sequence> sequences;
            for (unsigned i = 0; i < sequence_count; ++i) {
                auto p = rx.bytes + 16 + i * 16;
                sequences.push_back({get32(p), get32(p + 4), get32(p + 8), 0});
            }
            size_t matched = 0;
            auto compressed = encoder.encode(tx.bytes + 16, size, sequences, matched);
            size_t compressed_size = compressed.size();
            match_count += sequence_count - 1;
            total_matched += matched;
            if (verify) {
                size_t n = ZSTD_decompress(decoded.data(), decoded.size(), compressed.data(), compressed_size);
                require(!ZSTD_isError(n), std::string("Software decompression: ") + ZSTD_getErrorName(n));
                require(n == size && memcmp(decoded.data(), tx.bytes + 16, size) == 0,
                        "Software decompression does not match the original input");
            }
            write_all(file.value, compressed.data(), compressed_size);
            input_size += size; output_size += compressed_size; ++blocks;
            if (size < BLOCK) break;
        }
        require(fsync(file.value) == 0, "Cannot flush compressed output");
        // Publish only complete output. link() preserves no-overwrite behavior under a race.
        int published = force ? rename(temporary.c_str(), output.c_str())
                              : link(temporary.c_str(), output.c_str());
        require(published == 0, std::string("Cannot publish output: ") + std::strerror(errno));
        if (!force) unlink(temporary.c_str());
        temporary.clear();
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%s -> %s\nInput: %llu bytes; compressed: %llu bytes; frames: %llu\n",
                    paths[0].c_str(), output.c_str(), static_cast<unsigned long long>(input_size),
                    static_cast<unsigned long long>(output_size), static_cast<unsigned long long>(blocks));
        if (input_size) {
            double output_percent = 100.0 * double(output_size) / input_size;
            std::printf("Compression achieved: %.2f:1; output size: %.2f%% of input; size reduction: %.2f%%\n",
                        double(input_size) / output_size, output_percent, 100.0 - output_percent);
        } else std::printf("Compression achieved: n/a; output size: n/a (empty input; valid empty frame)\n");
        std::printf("Hardware LZ77: %llu matches covering %llu input bytes; software Zstandard entropy coding\n",
                    (unsigned long long)match_count, (unsigned long long)total_matched);
        std::printf("End-to-end%s: %.3f s, %.2f MiB/s%s\n", verify ? " with verification" : "", seconds,
                    input_size / seconds / 1048576.0, verify ? "; software round-trip: PASS" : "");
        return 0;
    } catch (const std::exception& e) {
        if (!temporary.empty()) unlink(temporary.c_str());
        std::fprintf(stderr, "fpga-zstd: %s\n", e.what());
        return 1;
    }
}
