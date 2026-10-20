#pragma once

#include <mutex>
#include <vector>

#include "data/archive.h"
#include "engine/data_segment.h"
#include "engine/vm.h"

struct SDL_AudioStream;

namespace bt {

// A decoded digitized sample (Creative VOC), mono float.
struct Sample {
    int rate = 11111;
    std::vector<float> data;
};

// Decodes a Creative Voice File (block types 1 and 9, 8-bit unsigned PCM).
Sample decode_voc(const Bytes& voc);

// Sound effects: the game's digitized samples (DATA.DAT entries 37..63) mixed
// in software into one SDL audio stream.
class Audio {
public:
    explicit Audio(const Archive& archive);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool open();  // opens the default playback device; false if unavailable

    // Acts on VM sound events, resolving ids through the game's tables
    // (sub_11D2E: ds:2964h maps a sound id to a sample or a MIDI effect).
    void handle(const VmEvent& event, const DataSegment& ds);

    void play_sample(int index);
    void stop_sample(int index);

    float volume = 0.8f;

private:
    static void callback(void* user, SDL_AudioStream* stream, int additional, int total);
    void mix(float* out, int frames);

    struct Voice {
        int sample;
        double pos;
    };
    static constexpr int kRate = 44100;
    std::vector<Sample> samples_;
    std::vector<Voice> voices_;
    std::mutex mutex_;
    SDL_AudioStream* stream_ = nullptr;
};

}  // namespace bt
