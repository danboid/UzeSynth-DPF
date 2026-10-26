/*
 * UzeSynth - polyphonic wrapper and resampler
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "UzeboxSynth.hpp"

#include <algorithm>
#include <cmath>

namespace uze {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

UzeboxSynth::UzeboxSynth()
{
    setSampleRate(48000.0);
}

void UzeboxSynth::setSampleRate(double rate)
{
    hostRate = rate > 1000.0 ? rate : 48000.0;
    ratio = kNativeSampleRate / hostRate;

    // Windowed-sinc interpolation table. Cutoff just below the native Nyquist
    // frequency (or the host's, if it is lower).
    const double fc = std::min(1.0, hostRate / kNativeSampleRate) * 0.92;
    table.assign(static_cast<size_t>((kPhases + 1) * kTaps), 0.0f);
    for (int p = 0; p <= kPhases; ++p)
    {
        const double f = static_cast<double>(p) / kPhases;
        double sum = 0.0;
        double row[kTaps];
        for (int j = 0; j < kTaps; ++j)
        {
            const double d = f + (kHalfTaps - 1) - j;        // distance from tap to output time
            const double x = fc * d;
            const double sinc = (std::fabs(x) < 1e-9) ? 1.0 : std::sin(kPi * x) / (kPi * x);
            double w = 0.0;
            if (std::fabs(d) < kHalfTaps)
                w = 0.42 + 0.5 * std::cos(kPi * d / kHalfTaps) + 0.08 * std::cos(2.0 * kPi * d / kHalfTaps);
            row[j] = fc * sinc * w;
            sum += row[j];
        }
        for (int j = 0; j < kTaps; ++j)
            table[static_cast<size_t>(p * kTaps + j)] = static_cast<float>(sum != 0.0 ? row[j] / sum : 0.0);
    }

    std::fill(std::begin(ring), std::end(ring), 0.0f);
    nextIndex = 0;
    timeInt = 0;
    timeFrac = 0.0;
}

int UzeboxSynth::latencySamples() const
{
    return static_cast<int>(std::lround(kHalfTaps / ratio));
}

void UzeboxSynth::setSoundData(const SoundData* d)
{
    data = d;
    for (auto& v : voices)
    {
        v.kill();
        v.setWavetable(d != nullptr ? &d->waves : nullptr);
    }
}

void UzeboxSynth::setMixer(UzeboxVoice::Mixer m)
{
    for (auto& v : voices)
        v.setMixer(m);
}

namespace {
UzeboxVoice::Channel channelFor(const PatchData& p)
{
    switch (p.type)
    {
    case PatchType::Noise: return UzeboxVoice::Channel::Noise;
    case PatchType::Pcm:   return UzeboxVoice::Channel::Pcm;
    default:               return UzeboxVoice::Channel::Wave;
    }
}
} // namespace

void UzeboxSynth::setPlayMode(PlayMode m)
{
    if (m == playMode)
        return;
    playMode = m;
    for (auto& v : voices)
        v.kill();
}

void UzeboxSynth::setPatchIndex(int index)
{
    patchIndex = std::max(0, index);
}

const PatchData* UzeboxSynth::currentPatch() const
{
    if (data == nullptr || data->bank.patches.empty())
        return nullptr;
    const auto& patches = data->bank.patches;
    const int i = std::min(patchIndex, static_cast<int>(patches.size()) - 1);
    return &patches[static_cast<size_t>(i)].data;
}

void UzeboxSynth::setPolyphony(int n)
{
    n = std::max(1, std::min(kMaxVoices, n));
    if (n == polyphony)
        return;
    for (int i = n; i < kMaxVoices; ++i)
        voices[i].kill();
    polyphony = n;
}

UzeboxVoice* UzeboxSynth::allocateVoice(uint8_t key)
{
    if (polyphony == 1)
        return &voices[0];

    // Same key still held: retrigger on that voice
    for (int i = 0; i < polyphony; ++i)
        if (voices[i].midiKey == key && !voices[i].released)
            return &voices[i];

    UzeboxVoice* best = nullptr;
    // 1. oldest silent voice
    for (int i = 0; i < polyphony; ++i)
        if (voices[i].isSilent() && (best == nullptr || voices[i].age < best->age))
            best = &voices[i];
    if (best != nullptr) return best;
    // 2. oldest released voice
    for (int i = 0; i < polyphony; ++i)
        if (voices[i].released && (best == nullptr || voices[i].age < best->age))
            best = &voices[i];
    if (best != nullptr) return best;
    // 3. oldest voice
    best = &voices[0];
    for (int i = 1; i < polyphony; ++i)
        if (voices[i].age < best->age)
            best = &voices[i];
    return best;
}

void UzeboxSynth::noteOn(uint8_t key, uint8_t velocity)
{
    if (velocity == 0)
    {
        noteOff(key);
        return;
    }
    if (data == nullptr)
        return;

    // The kernel's MIDI player uses volume = velocity << 1
    const uint8_t volume = static_cast<uint8_t>(velocity << 1);
    UzeboxVoice* v = nullptr;

    switch (playMode)
    {
    case PlayMode::Melodic:
    {
        const PatchData* patch = currentPatch();
        if (patch == nullptr)
            return;
        v = allocateVoice(key);
        if (!v->noteOn(patch, key, volume, channelFor(*patch)))
            return;
        break;
    }
    case PlayMode::DrumKit:
    {
        // TriggerCommon on channel 3: "if(!isFx) patch=note;"
        const PatchData* patch = data->bank.tableEntry(key);
        if (patch == nullptr)
            return;
        v = allocateVoice(key);
        if (!v->noteOn(patch, key, volume, UzeboxVoice::Channel::Noise))
            return;
        break;
    }
    case PlayMode::SoundFx:
    {
        const PatchData* patch = data->bank.tableEntry(key);
        if (patch == nullptr)
            return;
        v = allocateVoice(key);
        // TriggerFx: type 1 -> channel 3 (noise), type 2 -> PCM channel
        v->triggerFx(patch, volume, channelFor(*patch));
        break;
    }
    }

    v->midiKey = key;
    v->released = false;
    v->age = ++ageCounter;
    // Start the voice's 60 Hz frame right now so the note isn't delayed by up
    // to one frame (the kernel would trigger at the next VSYNC).
    v->syncFrame();
}

void UzeboxSynth::noteOff(uint8_t key)
{
    for (int i = 0; i < polyphony; ++i)
    {
        UzeboxVoice& v = voices[i];
        if (v.midiKey == key && !v.released)
        {
            v.noteOff();
            v.released = true;
        }
    }
}

void UzeboxSynth::controlChange(uint8_t cc, uint8_t value)
{
    const uint8_t v = static_cast<uint8_t>(value << 1);
    switch (cc)
    {
    case 7:   // CONTROLER_VOL
        trackVol = v;
        for (auto& voice : voices) voice.setTrackVolume(v);
        break;
    case 11:  // CONTROLER_EXPRESSION
        for (auto& voice : voices) voice.setExpression(v);
        break;
    case 92:  // CONTROLER_TREMOLO
        for (auto& voice : voices) voice.setTremoloLevel(v);
        break;
    case 100: // CONTROLER_TREMOLO_RATE
        for (auto& voice : voices) voice.setTremoloRate(v);
        break;
    case 120: // All Sound Off
        allSoundOff();
        break;
    case 123: // All Notes Off
        allNotesOff();
        break;
    default:
        break;
    }
}

void UzeboxSynth::allNotesOff()
{
    for (int i = 0; i < polyphony; ++i)
    {
        if (!voices[i].released)
        {
            voices[i].noteOff();
            voices[i].released = true;
        }
    }
}

void UzeboxSynth::allSoundOff()
{
    for (auto& v : voices)
        v.kill();
}

int8_t UzeboxSynth::renderNativeSample()
{
    int mix = 0;
    for (int i = 0; i < polyphony; ++i)
        mix += voices[i].renderSample(master);
    // clip to 8 bits, as the mixer does before writing the PWM buffer
    if (mix > 127) mix = 127;
    else if (mix < -128) mix = -128;
    return static_cast<int8_t>(mix);
}

void UzeboxSynth::render(float* out, uint32_t frames)
{
    constexpr int64_t mask = kRingSize - 1;
    const float* const tab = table.data();

    for (uint32_t i = 0; i < frames; ++i)
    {
        while (nextIndex <= timeInt + kHalfTaps)
        {
            ring[nextIndex & mask] = static_cast<float>(renderNativeSample()) * (1.0f / 128.0f);
            ++nextIndex;
        }

        float y;
        if (mode == OutputMode::Raw)
        {
            // zero-order hold: exactly the stepped PWM sample values
            y = ring[timeInt & mask];
        }
        else
        {
            const double ph = timeFrac * kPhases;
            int p0 = static_cast<int>(ph);
            if (p0 >= kPhases) p0 = kPhases - 1;
            const float a = static_cast<float>(ph - p0);
            const float* r0 = tab + p0 * kTaps;
            const float* r1 = r0 + kTaps;
            const int64_t first = timeInt - kHalfTaps + 1;
            float acc = 0.0f;
            for (int j = 0; j < kTaps; ++j)
            {
                const float c = r0[j] + (r1[j] - r0[j]) * a;
                acc += ring[(first + j) & mask] * c;
            }
            y = acc;
        }
        out[i] = y;

        timeFrac += ratio;
        const double whole = std::floor(timeFrac);
        timeInt += static_cast<int64_t>(whole);
        timeFrac -= whole;
    }
}

} // namespace uze
