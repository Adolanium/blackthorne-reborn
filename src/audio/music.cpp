#include "audio/music.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif

#ifdef _MSC_VER
#pragma warning(push, 0)
#pragma warning(disable : 4244 4456 4459 4701 4996)
#endif
#define TSF_IMPLEMENTATION
#include "tsf.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace bt {
namespace {

// General MIDI rendition (music card base 2 in ds:2948h, shared by the Gravis
// UltraSound and MT-32/MPU drivers). Song resource = ds:2938h[song] + base.
constexpr int kGeneralMidiBase = 2;
constexpr int kSongIds[8] = {0x20, 0x17, 0x23, 0x1A, 0x1D, 0x135, 0x14, 0x138};
constexpr int kEffectsId = 0x13;  // ds:2958h / 2 + 0x12 for base 2
constexpr int kRenderChunk = 64;  // frames between sequencer updates (1.5 ms)

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
    if (stream_) SDL_DestroyAudioStream(stream_);
    if (sf_) tsf_close(sf_);
#ifdef _WIN32
    if (handle_) {
        all_notes_off();
        midiOutClose(static_cast<HMIDIOUT>(handle_));
    }
#endif
}

bool Music::load_soundfont(const std::filesystem::path& path) {
    if (path.empty() || !std::filesystem::exists(path)) return false;
    sf_ = tsf_load_filename(path.string().c_str());
    if (!sf_) return false;
    tsf_set_output(sf_, TSF_STEREO_INTERLEAVED, kRate, -3.0f);
    tsf_set_max_voices(sf_, 96);  // allocate up front, not in the audio callback
    for (int ch = 0; ch < 16; ++ch) tsf_channel_set_presetnumber(sf_, ch, 0, ch == 9);
    return true;
}

bool Music::open_offline(const std::filesystem::path& soundfont) { return load_soundfont(soundfont); }

bool Music::open(const std::filesystem::path& soundfont) {
    if (load_soundfont(soundfont)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Music::stream_callback, this);
        if (!stream_) return false;
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }
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

// The player clock: frames rendered for a SoundFont, else wall time.
double Music::now() const {
    if (sf_) return static_cast<double>(rendered_) / kRate;
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Music::send(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    if ((status & 0xF0) == 0x90 && d2) d2 = static_cast<std::uint8_t>(std::max(1.0f, d2 * volume));
    if (sf_) {
        const int ch = status & 0x0F;
        switch (status & 0xF0) {
            case 0x80: tsf_channel_note_off(sf_, ch, d1); break;
            case 0x90:
                if (d2) tsf_channel_note_on(sf_, ch, d1, d2 / 127.0f);
                else tsf_channel_note_off(sf_, ch, d1);
                break;
            case 0xB0: tsf_channel_midi_control(sf_, ch, d1, d2); break;
            case 0xC0:
                // Fall back to the bank's first preset when the SoundFont lacks one.
                if (!tsf_channel_set_presetnumber(sf_, ch, d1, ch == 9)) {
                    const int index = tsf_get_presetindex(sf_, ch == 9 ? 128 : 0, 0);
                    tsf_channel_set_presetindex(sf_, ch, index >= 0 ? index : 0);
                }
                break;
            case 0xE0: tsf_channel_set_pitchwheel(sf_, ch, d1 | (d2 << 7)); break;
            default: break;
        }
        return;
    }
#ifdef _WIN32
    if (!handle_) return;
    midiOutShortMsg(static_cast<HMIDIOUT>(handle_), static_cast<DWORD>(status | (d1 << 8) | (d2 << 16)));
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

// Sends every event due by `time`; songs loop (from the FOR controller to
// NEXT when present, else from the start). The caller holds the mutex.
double Music::pump(double time) {
    double next_due = time + 0.25;
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
            if (due > time) {
                next_due = std::min(next_due, due);
                break;
            }
            send(e.status, e.data1, e.data2);
            ++t->next;
        }
    }
    return next_due;
}

// The system-synth sequencer thread.
void Music::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
        const double next_due = pump(now());
        wake_.wait_until(lock, std::chrono::steady_clock::now() +
                                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(std::max(0.0, next_due - now()))));
    }
}

void Music::render(float* out, int frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sf_) {
        std::fill(out, out + frames * 2, 0.0f);
        return;
    }
    while (frames > 0) {
        const int n = std::min(frames, kRenderChunk);
        pump(now());
        tsf_render_float(sf_, out, n, 0);
        rendered_ += static_cast<std::uint64_t>(n);
        out += n * 2;
        frames -= n;
    }
}

void Music::stream_callback(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* self = static_cast<Music*>(user);
    const int frames = additional / static_cast<int>(sizeof(float) * 2);
    if (frames <= 0) return;
    std::vector<float> buf(static_cast<std::size_t>(frames) * 2);
    self->render(buf.data(), frames);
    SDL_PutAudioStreamData(stream, buf.data(), frames * static_cast<int>(sizeof(float) * 2));
}

}  // namespace bt
