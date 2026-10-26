/*
 * UzeSynth - polyphonic wrapper around emulated Uzebox tracks, plus
 * resampling from the native 15734.26 Hz mixing rate to the host rate.
 *
 * Real-time safe: no allocation or locking after setSampleRate().
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#pragma once

#include "PatchParser.hpp"
#include "UzeboxVoice.hpp"
#include "Wavetable.hpp"

#include <cstdint>
#include <vector>

namespace uze {

// Everything a loaded "sound set" consists of. Immutable once handed to the
// synth, so it can be swapped in from another thread as a single pointer.
struct SoundData {
    PatchBank bank;
    Wavetable waves = defaultWavetable();
};

class UzeboxSynth {
public:
    static constexpr int kMaxVoices = 8;

    enum class OutputMode { Raw = 0, Smooth = 1 };

    enum class PlayMode {
        Melodic = 0, // selected patch, MIDI note = pitch (wave channels 1-3)
        DrumKit = 1, // MIDI note = patch number, noise channel (MIDI channel 4 in songs)
        SoundFx = 2, // MIDI note = patch number, played like TriggerFx()
    };

    UzeboxSynth();

    void setSampleRate(double hostRate);
    // Latency (in host samples) introduced by the resampler look-ahead
    int latencySamples() const;

    // Must stay alive while it is set. Resets all voices.
    void setSoundData(const SoundData* data);
    void setPatchIndex(int index);

    void setPolyphony(int voices);              // 1 = one Uzebox track (mono)
    void setMasterVolume(uint8_t v) { master = v; }
    void setOutputMode(OutputMode m) { mode = m; }
    void setPlayMode(PlayMode m);
    void setMixer(UzeboxVoice::Mixer m);

    // MIDI (channel is ignored: the plugin is one Uzebox track)
    void noteOn(uint8_t key, uint8_t velocity);
    void noteOff(uint8_t key);
    void controlChange(uint8_t cc, uint8_t value);
    void allNotesOff();
    void allSoundOff();

    // Render one native Uzebox sample (what the kernel writes to the PWM, signed)
    int8_t renderNativeSample();

    // Render host-rate mono audio
    void render(float* out, uint32_t frames);

private:
    UzeboxVoice voices[kMaxVoices];
    int polyphony = 1;
    uint8_t master = kDefaultMasterVolume;
    OutputMode mode = OutputMode::Smooth;
    uint32_t ageCounter = 0;

    // persistent MIDI controller values (re-applied to newly triggered voices
    // exactly as the kernel would: trackVol persists, others reset on trigger)
    uint8_t trackVol = 0xff;

    const SoundData* data = nullptr;
    PlayMode playMode = PlayMode::Melodic;
    int patchIndex = 0;

    // ---- resampler ----
    static constexpr int kHalfTaps = 8;              // taps on each side
    static constexpr int kTaps = kHalfTaps * 2;
    static constexpr int kPhases = 256;
    static constexpr int kRingSize = 64;             // power of two > kTaps
    std::vector<float> table;                         // (kPhases + 1) * kTaps
    float ring[kRingSize] = {};
    int64_t nextIndex = 0;                            // next native sample to generate
    int64_t timeInt = 0;                              // output time (native samples)
    double timeFrac = 0.0;
    double ratio = 1.0;                               // native samples per host sample
    double hostRate = 48000.0;

    const PatchData* currentPatch() const;
    UzeboxVoice* allocateVoice(uint8_t key);
};

} // namespace uze
