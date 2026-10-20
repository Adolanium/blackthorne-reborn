#include "audio/music.h"

#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif

namespace bt {
namespace {

// General MIDI rendition (music card base 2 in ds:2948h, shared by the Gravis
// UltraSound and MT-32/MPU drivers). Song resource = ds:2938h[song] + base.
constexpr int kGeneralMidiBase = 2;
constexpr int kSongIds[8] = {0x20, 0x17, 0x23, 0x1A, 0x1D, 0x135, 0x14, 0x138};
constexpr int kEffectsId = 0x13;  // ds:2958h / 2 + 0x12 for base 2

}  // namespace

Music::Music(const Archive& archive) {
    for (int i = 0; i < 8; ++i) {
        try {
            songs_[i] = parse_xmidi(archive.resource(kSongIds[i] + kGeneralMidiBase));
        } catch (const std::exception&) {
        }
    }
    try {
        effects_ = parse_xmidi(archive.resource(kEffectsId));
    } catch (const std::exception&) {
    }
}

Music::~Music() {
    quit_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
#ifdef _WIN32
    if (handle_) {
        all_notes_off();
        midiOutClose(static_cast<HMIDIOUT>(handle_));
    }
#endif
}

bool Music::open() {
#ifdef _WIN32
    HMIDIOUT h = nullptr;
    if (midiOutOpen(&h, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return false;
    handle_ = h;
    thread_ = std::thread([this] { run(); });
    return true;
#else
    return false;
#endif
}

double Music::now() const {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Music::send(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
#ifdef _WIN32
    if (!handle_) return;
    if ((status & 0xF0) == 0x90 && d2) d2 = static_cast<std::uint8_t>(d2 * volume);
    midiOutShortMsg(static_cast<HMIDIOUT>(handle_), static_cast<DWORD>(status | (d1 << 8) | (d2 << 16)));
#else
    (void)status; (void)d1; (void)d2;
#endif
}

void Music::all_notes_off() {
    for (int ch = 0; ch < 16; ++ch) {
        send(static_cast<std::uint8_t>(0xB0 | ch), 123, 0);  // all notes off
        send(static_cast<std::uint8_t>(0xB0 | ch), 121, 0);  // reset controllers
    }
}

void Music::load(int song) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (song < 0 || song >= 8 || songs_[song].empty()) return;
    loaded_ = song;
}

void Music::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded_ < 0) return;
        all_notes_off();
        music_ = {&songs_[loaded_][0], now(), 0, true};
    }
    wake_.notify_all();
}

void Music::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    music_.seq = nullptr;
    all_notes_off();
}

void Music::play_effect(int sequence) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sequence < 0 || sequence >= static_cast<int>(effects_.size())) return;
        effect_ = {&effects_[sequence], now(), 0, false};
    }
    wake_.notify_all();
}

// The sequencer thread: sends events when their time comes; songs loop
// (from the FOR controller to NEXT when present, else from the start).
void Music::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
        double next_due = now() + 0.25;
        for (Track* t : {&music_, &effect_}) {
            if (!t->seq) continue;
            const auto& ev = t->seq->events;
            const double tick = 1.0 / kXmidiTicksPerSecond;
            for (;;) {
                if (t->next >= ev.size()) {
                    if (!t->loop) {
                        t->seq = nullptr;
                        break;
                    }
                    const std::int64_t from = t->seq->loop_start >= 0 ? t->seq->loop_start : 0;
                    const std::int64_t to = t->seq->loop_end >= 0 ? t->seq->loop_end : t->seq->length_ticks;
                    t->start_time += static_cast<double>(to - from) * tick;
                    t->next = 0;
                    while (t->next < ev.size() && static_cast<std::int64_t>(ev[t->next].tick) < from) ++t->next;
                    if (to <= from) {
                        t->seq = nullptr;
                        break;
                    }
                    continue;
                }
                const MidiEvent& e = ev[t->next];
                if (t->loop && t->seq->loop_end >= 0 && static_cast<std::int64_t>(e.tick) > t->seq->loop_end) {
                    t->next = ev.size();
                    continue;
                }
                const double due = t->start_time + e.tick * tick;
                if (due > now()) {
                    next_due = std::min(next_due, due);
                    break;
                }
                send(e.status, e.data1, e.data2);
                ++t->next;
            }
        }
        wake_.wait_until(lock, std::chrono::steady_clock::now() +
                                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(std::max(0.0, next_due - now()))));
    }
}

}  // namespace bt
