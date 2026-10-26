/*
 * UzeSynth - Uzebox sound engine emulation
 *
 * One UzeboxVoice == one kernel "Track" (uzeboxSoundEngine.c) plus the
 * mixer channel it drives (soundMixerVsync.s). The code follows the kernel
 * line by line, including its integer widths and rounding, so that the
 * 8-bit output is bit-identical to the default (VSYNC mixer, MIDI music
 * engine) kernel configuration.
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace uze {

struct Wavetable;

// 28.63636 MHz / 1820 cycles per scanline = one mixed sample per scanline
constexpr double kNativeSampleRate = 28636360.0 / 1820.0; // 15734.2637 Hz
// MIX_BANK_SIZE: samples mixed per VSYNC (one video frame, ~60.05 Hz)
constexpr int kSamplesPerFrame = 262;
// DEFAULT_MASTER_VOL in kernel/defines.h
constexpr uint8_t kDefaultMasterVolume = 0x6f;

enum PatchCommand : uint8_t {
    PC_ENV_SPEED     = 0,
    PC_NOISE_PARAMS  = 1,
    PC_WAVE          = 2,
    PC_NOTE_UP       = 3,
    PC_NOTE_DOWN     = 4,
    PC_NOTE_CUT      = 5,
    PC_NOTE_HOLD     = 6,
    PC_ENV_VOL       = 7,
    PC_PITCH         = 8,
    PC_TREMOLO_LEVEL = 9,
    PC_TREMOLO_RATE  = 10,
    PC_SLIDE         = 11,
    PC_SLIDE_SPEED   = 12,
    PC_LOOP_START    = 13,
    PC_LOOP_END      = 14,
    PATCH_END        = 0xff,
};

enum class PatchType : uint8_t { Wave = 0, Noise = 1, Pcm = 2 };

// A patch command stream exactly as it sits in flash:
// <delta>,<cmd>,<param>, <delta>,<cmd>,<param>, ... <delta>,PATCH_END
struct PatchData {
    PatchType type = PatchType::Wave;
    std::vector<uint8_t> stream;   // may be empty (cmdStream == NULL)

    // PCM patches (PatchStruct pcmData / loopStart / loopEnd)
    std::vector<uint8_t> sample;   // signed 8-bit; empty if the array wasn't found
    int32_t loopStart = 0;         // byte offsets into 'sample'
    int32_t loopEnd = 0;
};

class UzeboxVoice {
public:
    UzeboxVoice() { reset(); }

    // Power-on state (InitMusicPlayer + zeroed .bss)
    void reset();

    // Which kind of mixer channel the track drives
    enum class Channel { Wave, Noise, Pcm };

    // Kernel mixer build option (SOUND_MIXER). They differ in the noise
    // channel's output and in how the PCM channel loops:
    //  Vsync  - MIXER_TYPE_VSYNC (default); PCM when built with MIXER_CHAN4_TYPE=1
    //  Inline - MIXER_TYPE_INLINE; PCM is the 5th channel (SOUND_CHANNEL_5_ENABLE)
    enum class Mixer { Vsync, Inline };
    void setMixer(Mixer m) { mixer = m; }

    // TriggerNote(channel, patch, note, volume) with volume != 0.
    // On the noise channel (channel 3 / MIDI channel 4) the kernel uses the
    // note number as the patch number; the caller resolves that.
    // Returns false if ignored because a sound effect owns the track.
    bool noteOn(const PatchData* patch, uint8_t note, uint8_t volume, Channel channel);
    // TriggerFx(patch, volume, retrig): priority playback at note 80, full
    // track volume, note-offs ignored until the patch reaches PATCH_END.
    void triggerFx(const PatchData* patch, uint8_t volume, Channel channel);
    // Mixer wavetable (the 'waves' table, default or MIXER_WAVES)
    void setWavetable(const Wavetable* w) { waves = w; }
    // TriggerNote(..., volume == 0)
    void noteOff();
    // Immediately silence (not a kernel operation; used for panic/patch reload)
    void kill();

    // MIDI controllers as handled by the kernel's MIDI player
    void setTrackVolume(uint8_t v)      { trackVol = v; }
    void setExpression(uint8_t v)       { expressionVol = v; }
    void setTremoloLevel(uint8_t v)     { tremoloLevel = v; }
    void setTremoloRate(uint8_t v)      { tremoloRate = v; }

    // Next output sample of this channel, already scaled by the channel
    // volume: (sample * vol) >> 8, as the mixer's mulsu/sbc sequence does.
    // Runs ProcessMusic() for this track every 262 samples.
    int renderSample(uint8_t masterVolume);

    // Restart the 60 Hz frame clock so ProcessMusic runs on the next sample.
    void syncFrame() { samplesUntilFrame = 0; }

    // Voice allocation helpers (plugin side, not kernel state)
    bool isSilent() const;
    int  midiKey  = -1;
    bool released = true;
    uint32_t age  = 0;

private:
    void triggerCommon(const PatchData* patch, uint8_t volume, uint8_t note, Channel channel);
    void processFrame(uint8_t masterVolume);
    uint8_t waveByte(int wave, int index) const;
    void runCommand(uint8_t cmd, uint8_t param);
    uint8_t readStream(int pos) const;

    void setMixerNote(uint8_t note);
    void setMixerWave(uint8_t wave);
    uint16_t mixerStep() const;
    void setMixerStep(uint16_t step);

    // ----- struct TrackStruct -----
    uint8_t  flags;
    uint8_t  note;
    uint8_t  expressionVol;
    uint8_t  loopCount;
    int16_t  slideStep;
    uint8_t  slideNote;
    uint8_t  slideSpeed;
    uint8_t  tremoloPos;
    uint8_t  tremoloLevel;
    uint8_t  tremoloRate;
    uint8_t  trackVol;
    uint8_t  noteVol;
    uint8_t  envelopeVol;
    int8_t   envelopeStep;
    uint8_t  patchNextDeltaTime;
    uint8_t  patchCurrDeltaTime;
    const PatchData* patch;
    int      streamPos;            // -1 == NULL

    const Wavetable* waves = nullptr;
    Mixer mixer = Mixer::Vsync;

    // ----- mixer channel -----
    bool     noise;                // channel 4 (LFSR) instead of a wave channel
    bool     pcm;                  // PCM channel
    uint8_t  volume;               // tr*_vol, written once per frame
    // wave channel
    uint16_t step;                 // step_hi:step_lo
    uint16_t pos;                  // pos_lo:pos_frac (pos_hi selects the wave)
    uint8_t  wave;
    // noise channel (tr4_params, tr4_barrel, tr4_divider, tr4_reserved1)
    uint8_t  noiseParams;
    uint16_t barrel;
    uint8_t  divider;
    // PCM channel: position as an offset from the sample start (the kernel keeps
    // an absolute flash address), 8-bit fraction, loop length and loop end.
    int32_t  pcmPos;
    uint8_t  pcmFrac;
    int32_t  pcmLoopLen;
    int32_t  pcmLoopEnd;
    int32_t  pcmLoopTarget;        // VSYNC mixer: computed once per frame

    int samplesUntilFrame;
};

} // namespace uze
