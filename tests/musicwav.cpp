// Renders a song through a SoundFont to a WAV file.
// Usage: btmusicwav <DATA.DAT> <font.sf2> <song 0-7> <seconds> <out.wav>
// Prints the peak level so tests can check that something was played.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "audio/music.h"
#include "data/archive.h"

int main(int argc, char** argv) {
    if (argc < 6) {
        std::printf("usage: btmusicwav <DATA.DAT> <font.sf2> <song 0-7> <seconds> <out.wav>\n");
        return 1;
    }
    bt::Archive archive(argv[1]);
    bt::Music music(archive);
    if (!music.open_offline(argv[2])) {
        std::printf("cannot load %s\n", argv[2]);
        return 1;
    }
    music.load(std::atoi(argv[3]));
    music.start();
    const int frames = static_cast<int>(std::atof(argv[4]) * bt::Music::kRate);
    std::vector<float> buf(static_cast<std::size_t>(frames) * 2);
    music.render(buf.data(), frames);

    float peak = 0;
    std::vector<std::int16_t> pcm(buf.size());
    for (std::size_t i = 0; i < buf.size(); ++i) {
        const float v = std::fmax(-1.0f, std::fmin(1.0f, buf[i]));
        peak = std::fmax(peak, std::fabs(v));
        pcm[i] = static_cast<std::int16_t>(v * 32767);
    }
    std::ofstream out(argv[5], std::ios::binary);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t bytes = static_cast<std::uint32_t>(pcm.size() * 2);
    out.write("RIFF", 4); u32(36 + bytes); out.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(bt::Music::kRate); u32(bt::Music::kRate * 4); u16(4); u16(16);
    out.write("data", 4); u32(bytes);
    out.write(reinterpret_cast<const char*>(pcm.data()), bytes);
    std::printf("rendered %d frames, peak %.3f\n", frames, peak);
    return peak > 0.01f ? 0 : 2;
}
