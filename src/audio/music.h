#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "data/archive.h"
#include "data/xmidi.h"

namespace bt {

// Plays the game's General MIDI music and MIDI sound effects on the system
// MIDI synthesizer (on Windows the Microsoft GS Wavetable Synth, the same
// output Blizzard's 2013 DOSBox release used).
class Music {
public:
    explicit Music(const Archive& archive);
    ~Music();
    Music(const Music&) = delete;
    Music& operator=(const Music&) = delete;

    bool open();

    // Song numbers are the level header's byte 5 (index into ds:2938h).
    void load(int song);
    void start();
    void stop();
    void play_effect(int sequence);  // a sequence of the MIDI effects bank

    float volume = 0.8f;

private:
    struct Track {
        const MidiSequence* seq = nullptr;
        double start_time = 0;   // seconds on the player clock
        std::size_t next = 0;
        bool loop = false;
    };
    void run();
    void send(std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
    void all_notes_off();
    double now() const;

    std::vector<MidiSequence> songs_[8];
    std::vector<MidiSequence> effects_;
    int loaded_ = -1;
    Track music_, effect_;

    void* handle_ = nullptr;  // HMIDIOUT on Windows
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> quit_{false};
    std::thread thread_;
};

}  // namespace bt
