#include "audio/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace bt {
namespace {

constexpr int kFirstSampleEntry = 37;  // resource 0x26, loaded in order by sub_11BFD
constexpr int kSampleCount = 27;
constexpr std::uint16_t kSoundOn = 0x380;
constexpr std::uint16_t kSoundTable = 0x2964;
constexpr std::uint16_t kNoSound = 0x3E8;

}  // namespace

Sample decode_voc(const Bytes& voc) {
    Sample s;
    if (voc.size() < 26 || std::memcmp(voc.data(), "Creative Voice File", 19) != 0) return s;
    std::size_t o = voc[20] | (voc[21] << 8);
    while (o + 4 <= voc.size()) {
        const int type = voc[o];
        if (type == 0) break;
        const std::size_t size = voc[o + 1] | (voc[o + 2] << 8) | (static_cast<std::size_t>(voc[o + 3]) << 16);
        const std::size_t body = o + 4;
        std::size_t data = 0, end = std::min(body + size, voc.size());
        if (type == 1 && size >= 2) {  // time constant, codec, 8-bit PCM
            s.rate = 1000000 / (256 - voc[body]);
            data = body + 2;
        } else if (type == 9 && size >= 12) {  // u32 rate, bits, channels, codec, reserved
            s.rate = static_cast<int>(voc[body] | (voc[body + 1] << 8) | (voc[body + 2] << 16) | (static_cast<std::uint32_t>(voc[body + 3]) << 24));
            data = body + 12;
        } else if (type == 2) {  // continuation of the previous block
            data = body;
        }
        for (std::size_t i = data; data && i < end; ++i) s.data.push_back((static_cast<int>(voc[i]) - 128) / 128.0f);
        o = body + size;
    }
    return s;
}

Audio::Audio(const Archive& archive) {
    for (int i = 0; i < kSampleCount; ++i) samples_.push_back(decode_voc(archive.entry(kFirstSampleEntry + i)));
}

Audio::~Audio() {
    if (stream_) SDL_DestroyAudioStream(stream_);
}

bool Audio::open() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Audio::callback, this);
    if (!stream_) return false;
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void Audio::handle(const VmEvent& event, const DataSegment& ds) {
    if (music) {
        switch (event.kind) {
            case VmEvent::Kind::MusicLoad: music->load(event.id); return;
            case VmEvent::Kind::MusicStart: music->start(); return;
            case VmEvent::Kind::MusicRestart: if (ds.u16(0x37E)) music->start(); return;  // F4
            case VmEvent::Kind::MusicStop: music->stop(); return;
            case VmEvent::Kind::MidiEffect: music->play_effect(event.id); return;
            default: break;
        }
    }
    if (event.kind == VmEvent::Kind::StopAllSounds) {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.clear();
        return;
    }
    if (event.kind != VmEvent::Kind::PlaySound && event.kind != VmEvent::Kind::StopSound) return;
    if (!ds.u16(kSoundOn)) return;
    const std::uint16_t v = ds.u16(static_cast<std::uint16_t>(kSoundTable + (event.id - 0x80) * 2));
    if (v == kNoSound || v == 0xFFFF) return;
    if (!(v & 0x8000)) {  // sub_197EA: an effect sequence played by the music driver
        if (music && event.kind == VmEvent::Kind::PlaySound) music->play_effect(v);
        return;
    }
    if (event.kind == VmEvent::Kind::PlaySound) play_sample(v & 0xFFF);
    else stop_sample(v & 0xFFF);
}

void Audio::play_sample(int index) {
    if (index < 0 || index >= static_cast<int>(samples_.size()) || samples_[index].data.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.push_back({index, 0.0});
}

void Audio::stop_sample(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.sample == index; }), voices_.end());
}

void Audio::callback(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* self = static_cast<Audio*>(user);
    const int frames = additional / static_cast<int>(sizeof(float));
    if (frames <= 0) return;
    std::vector<float> buf(static_cast<std::size_t>(frames), 0.0f);
    self->mix(buf.data(), frames);
    SDL_PutAudioStreamData(stream, buf.data(), frames * static_cast<int>(sizeof(float)));
}

void Audio::mix(float* out, int frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& v : voices_) {
        const Sample& s = samples_[v.sample];
        const double step = static_cast<double>(s.rate) / kRate;
        for (int i = 0; i < frames; ++i) {
            const std::size_t k = static_cast<std::size_t>(v.pos);
            if (k + 1 >= s.data.size()) {
                v.pos = static_cast<double>(s.data.size());
                break;
            }
            const float t = static_cast<float>(v.pos - static_cast<double>(k));
            out[i] += (s.data[k] * (1 - t) + s.data[k + 1] * t) * volume;
            v.pos += step;
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [&](const Voice& v) { return v.pos >= static_cast<double>(samples_[v.sample].data.size()) - 1; }),
                  voices_.end());
    for (int i = 0; i < frames; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

}  // namespace bt
