#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "data/archive.h"
#include "data/xmidi.h"

struct SDL_AudioStream;
struct tsf;

namespace bt {

// Plays the game's General MIDI music and MIDI sound effects, either through
// a SoundFont (rendered by TinySoundFont into an SDL audio stream, on any
// platform) or on the system MIDI synthesizer (on Windows the Microsoft GS
// Wavetable Synth, the same output Blizzard's 2013 DOSBox release used).
class Music {
public:
    explicit Music(const Archive& archive);
    ~Music();
    Music(const Music&) = delete;
    Music& operator=(const Music&) = delete;

    // Uses the SoundFont when one is given and loads, else the system synth.
    bool open(const std::filesystem::path& soundfont = {});
    void close();  // before SDL_Quit, which would free the stream under us
    // Loads a SoundFont without opening an audio device (render() drives it).
    bool open_offline(const std::filesystem::path& soundfont);
    bool using_soundfont() const { return sf_ != nullptr; }

    // Song numbers are the level header's byte 5 (index into ds:2938h).
    void load(int song);
    void start();
    void stop();
    void play_effect(int sequence);  // a sequence of the MIDI effects bank

    // SoundFont output: interleaved stereo float frames at kRate.
    static constexpr int kRate = 44100;
    void render(float* out, int frames);

    float volume = 0.8f;

private:
    struct Track {
        const MidiSequence* seq = nullptr;
        double start_time = 0;   // seconds on the player clock
        std::size_t next = 0;
        bool loop = false;
    };
    void run();
    double pump(double time);  // sends the events due by `time`; returns the next due time
    void send(std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
    void all_notes_off();
    double now() const;
    bool load_soundfont(const std::filesystem::path& path);
    static void stream_callback(void* user, SDL_AudioStream* stream, int additional, int total);

    std::vector<MidiSequence> songs_[8];
    std::vector<MidiSequence> effects_;
    int loaded_ = -1;
    Track music_, effect_;

    void* handle_ = nullptr;  // HMIDIOUT on Windows
    tsf* sf_ = nullptr;
    SDL_AudioStream* stream_ = nullptr;
    std::uint64_t rendered_ = 0;  // SoundFont clock, in frames
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> quit_{false};
    std::thread thread_;
};

}  // namespace bt
