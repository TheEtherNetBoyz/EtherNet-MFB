// Offline conversion only. Dusk does not link the MP3 decoder.
#include <array>
#include <cstdio>
#include <cstdint>
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

static void be16(unsigned char* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void be32(unsigned char* p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v); }
int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "Usage: mp3_to_ast input.mp3 output.ast\n"); return 1; }
    drmp3 decoder;
    if (!drmp3_init_file(&decoder, argv[1], nullptr)) return 2;
    constexpr uint32_t blockBytes = 0x2760; // JAUInitializer::aramBlockSize_
    constexpr uint32_t blockFrames = blockBytes / 2;
    const auto frames = drmp3_get_pcm_frame_count(&decoder);
    if (decoder.channels != 2 || frames <= blockFrames * 10 || frames > 0x7fffffff ||
        !drmp3_seek_to_pcm_frame(&decoder, 0)) return 3;
    const uint64_t blocks = (frames + blockFrames - 1) / blockFrames;
    const uint64_t dataSize = blocks * (32 + blockBytes * 2);
    if (dataSize > 0xffffffff) return 4;
    FILE* output = std::fopen(argv[2], "wb");
    if (!output) return 5;
    std::array<unsigned char, 64> header{};
    be32(header.data(), 0x5354524d); // STRM
    be32(header.data() + 4, static_cast<uint32_t>(dataSize));
    be16(header.data() + 8, 1); // Native PCM16
    be16(header.data() + 10, 16);
    be16(header.data() + 12, 2);
    be16(header.data() + 14, 0xffff); // Loop
    be32(header.data() + 16, decoder.sampleRate);
    be32(header.data() + 20, static_cast<uint32_t>(frames));
    be32(header.data() + 24, 0);
    be32(header.data() + 28, static_cast<uint32_t>(frames));
    be32(header.data() + 32, blockBytes);
    header[40] = 127;
    if (std::fwrite(header.data(), 1, header.size(), output) != header.size()) return 6;
    uint64_t decodedFrames = 0;
    for (uint64_t b = 0; b < blocks; ++b) {
        std::array<drmp3_int16, blockFrames * 2> pcm{};
        decodedFrames += drmp3_read_pcm_frames_s16(&decoder, blockFrames, pcm.data());
        std::array<unsigned char, 32 + blockBytes * 2> block{};
        be32(block.data(), 0x424c434b); // BLCK
        be32(block.data() + 4, blockBytes);
        for (uint32_t c = 0; c < 2; ++c)
            for (uint32_t i = 0; i < blockFrames; ++i)
                be16(block.data() + 32 + c * blockBytes + i * 2, static_cast<uint16_t>(pcm[i * 2 + c]));
        if (std::fwrite(block.data(), 1, block.size(), output) != block.size()) return 7;
    }
    const int result = std::fclose(output);
    std::printf("AST: %llu frames, %u Hz stereo, %llu bytes, loop 0..%llu\n",
        frames, decoder.sampleRate, dataSize + 64, frames);
    drmp3_uninit(&decoder);
    return result == 0 && decodedFrames == frames ? 0 : 8;
}
