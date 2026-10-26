/*
 * UzeSynth - Uzebox sound engine emulation
 *
 * Port of ProcessMusic()/TriggerNote()/PatchCommandXX() from
 * kernel/uzeboxSoundEngine.c and the channel mixing loop from
 * kernel/soundMixerVsync.s (Uzebox kernel, (C) 2008-2009 Alec Bourque, GPLv3).
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "UzeboxVoice.hpp"
#include "UzeboxTables.hpp"
#include "Wavetable.hpp"

namespace uze {

namespace {

constexpr uint8_t TRACK_FLAGS_SLIDING   = 8;
constexpr uint8_t TRACK_FLAGS_ALLOCATED = 16;
constexpr uint8_t TRACK_FLAGS_PLAYING   = 32;
constexpr uint8_t TRACK_FLAGS_HOLD_ENV  = 64;
constexpr uint8_t TRACK_FLAGS_PRIORITY  = 128;

// Safety net against malformed loops (the real kernel would hang forever).
constexpr int kMaxCommandsPerFrame = 4096;

// steptable[] lookup. The kernel performs no bounds check; indices outside the
// 127-entry table would read unrelated flash, so we clamp instead.
inline uint16_t stepFor(int index)
{
    if (index < 0) index = 0;
    if (index >= kNumSteps) index = kNumSteps - 1;
    return kStepTable[index];
}

} // namespace

// --------------------------------------------------------------------------
// Power on / InitMusicPlayer()

void UzeboxVoice::reset()
{
    flags = TRACK_FLAGS_ALLOCATED;
    note = 0;
    expressionVol = 0;
    loopCount = 0;
    slideStep = 0;
    slideNote = 0;
    slideSpeed = 0x10;
    tremoloPos = 0;
    tremoloLevel = 0;
    tremoloRate = 24;  // ~6 Hz
    trackVol = 0xff;   // DEFAULT_TRACK_VOL
    noteVol = 0;
    envelopeVol = 0;
    envelopeStep = 0;
    patchNextDeltaTime = 0;
    patchCurrDeltaTime = 0;
    patch = nullptr;
    streamPos = -1;

    noise = false;
    volume = 0;
    step = 0;
    pos = 0;
    wave = 0;
    noiseParams = 0;
    barrel = 0;
    divider = 0;

    samplesUntilFrame = 0;
    midiKey = -1;
    released = true;
    age = 0;
}

void UzeboxVoice::kill()
{
    flags &= ~(TRACK_FLAGS_PLAYING | TRACK_FLAGS_PRIORITY | TRACK_FLAGS_HOLD_ENV);
    streamPos = -1;
    patch = nullptr;
    volume = 0;
    midiKey = -1;
    released = true;
}

bool UzeboxVoice::isSilent() const
{
    if (!(flags & TRACK_FLAGS_PLAYING))
        return true;
    // Command stream finished and nothing can make it audible again.
    return streamPos < 0 && (noteVol == 0 || (envelopeVol == 0 && envelopeStep <= 0));
}

// --------------------------------------------------------------------------
// Mixer helpers: SetMixerNote / SetMixerWave (uzeboxSoundEngineCore.s)

void UzeboxVoice::setMixerNote(uint8_t n)
{
    // VSYNC mixer: "cpi r24,3 / brlo set_note_waves / ret" -> noise ignores notes
    if (noise)
        return;
    step = stepFor(n);
}

void UzeboxVoice::setMixerWave(uint8_t w)
{
    if (noise)
    {
        // Channel 3: 0xfe selects 7-bit LFSR, 0xff "15-bit" (the kernel does
        // ori 0xfe here, which sets the divider bits rather than bit 0; kept).
        if (w == 0xfe)      noiseParams &= 0xfe;
        else if (w == 0xff) noiseParams |= 0xfe;
        // other values write tr4_reserved1 - inaudible
        return;
    }
    // Wave numbers beyond the end of the table would point into unrelated
    // flash on the real console; wrap them around instead.
    const int count = (waves != nullptr && waves->count() > 0) ? waves->count() : kNumWaves;
    wave = static_cast<uint8_t>(w % count);
}

// mixer.channels.all[ch].step. For the noise channel the same struct bytes
// alias tr4_params (step_lo) and tr4_barrel_lo (step_hi).
uint16_t UzeboxVoice::mixerStep() const
{
    if (noise)
        return static_cast<uint16_t>(noiseParams | ((barrel & 0xff) << 8));
    return step;
}

void UzeboxVoice::setMixerStep(uint16_t s)
{
    if (noise)
    {
        noiseParams = static_cast<uint8_t>(s & 0xff);
        barrel = static_cast<uint16_t>((barrel & 0xff00) | (s >> 8));
        return;
    }
    step = s;
}

uint8_t UzeboxVoice::waveByte(int w, int index) const
{
    if (waves != nullptr && waves->count() > 0)
    {
        if (w >= waves->count()) w %= waves->count();
        return waves->wave(w)[index & 0xff];
    }
    return kWaves[(w % kNumWaves) * kWaveLen + (index & 0xff)];
}

uint8_t UzeboxVoice::readStream(int p) const
{
    if (patch == nullptr || p < 0 || p >= static_cast<int>(patch->stream.size()))
        return PATCH_END; // runs off the end -> treat as end of stream
    return patch->stream[static_cast<size_t>(p)];
}

// --------------------------------------------------------------------------
// TriggerNote() / TriggerCommon()

bool UzeboxVoice::noteOn(const PatchData* p, uint8_t n, uint8_t vol, Channel channel)
{
    // TriggerNote: "allow only other music notes" - an FX in progress wins
    if ((flags & TRACK_FLAGS_PLAYING) != 0 && (flags & TRACK_FLAGS_PRIORITY) != 0)
        return false;

    flags = 0;
    streamPos = -1;
    triggerCommon(p, vol, n, channel);
    flags |= TRACK_FLAGS_PLAYING;
    return true;
}

void UzeboxVoice::triggerFx(const PatchData* p, uint8_t vol, Channel channel)
{
    flags = TRACK_FLAGS_PRIORITY;
    streamPos = -1;
    triggerCommon(p, vol, 80, channel);
    flags |= TRACK_FLAGS_PLAYING;
}

void UzeboxVoice::triggerCommon(const PatchData* p, uint8_t vol, uint8_t n, Channel channel)
{
    envelopeStep = 0;
    envelopeVol = 0xff;
    noteVol = vol;
    flags &= ~(TRACK_FLAGS_HOLD_ENV | TRACK_FLAGS_SLIDING);
    note = n;
    tremoloLevel = 0;
    tremoloPos = 0;
    loopCount = 0;
    expressionVol = 0xff; // DEFAULT_EXPRESSION_VOL

    patch = p;
    noise = (channel == Channel::Noise);

    if (noise)
    {
        barrel = 0x0101;
        noiseParams = 1; // 15 bits, no divider
    }
    else
    {
        setMixerNote(n);
        setMixerWave(0);
    }

    if (p == nullptr || p->stream.empty())
    {
        streamPos = -1;
    }
    else
    {
        patchCurrDeltaTime = 0;
        patchNextDeltaTime = p->stream[0];
        streamPos = 1;
    }
}

void UzeboxVoice::noteOff()
{
    if ((flags & TRACK_FLAGS_PLAYING) == 0 || (flags & TRACK_FLAGS_PRIORITY) == 0)
    {
        // cut note if there's no envelope & no note hold
        if (envelopeStep == 0 && !(flags & TRACK_FLAGS_HOLD_ENV))
            noteVol = 0;
        flags &= ~TRACK_FLAGS_HOLD_ENV;
    }
}

// --------------------------------------------------------------------------
// PatchCommand00..14

void UzeboxVoice::runCommand(uint8_t cmd, uint8_t uparam)
{
    const int8_t param = static_cast<int8_t>(uparam); // avr-gcc 'char' is signed

    switch (cmd)
    {
    case PC_ENV_SPEED:
        envelopeStep = param;
        break;

    case PC_NOISE_PARAMS:
        // Only touches the (single) noise channel. On a wave track this would
        // alter the separate noise channel, which a patch alone can't hear.
        if (noise)
        {
            barrel = 0x0101;
            noiseParams = uparam;
        }
        break;

    case PC_WAVE:
        setMixerWave(uparam);
        break;

    case PC_NOTE_UP:
        note = static_cast<uint8_t>(note + param);
        setMixerNote(note);
        break;

    case PC_NOTE_DOWN:
        note = static_cast<uint8_t>(note - param);
        setMixerNote(note);
        break;

    case PC_NOTE_CUT:
        flags &= ~(TRACK_FLAGS_PLAYING | TRACK_FLAGS_PRIORITY);
        break;

    case PC_NOTE_HOLD:
        flags |= TRACK_FLAGS_HOLD_ENV;
        break;

    case PC_ENV_VOL:
        envelopeVol = uparam;
        break;

    case PC_PITCH:
        setMixerNote(uparam);
        note = uparam;
        flags &= ~TRACK_FLAGS_SLIDING;
        break;

    case PC_TREMOLO_LEVEL:
        tremoloLevel = uparam;
        break;

    case PC_TREMOLO_RATE:
        tremoloRate = uparam;
        break;

    case PC_SLIDE:
    {
        // s16 maths with a 16-bit 'int', as on the AVR
        const int16_t currentStep = static_cast<int16_t>(stepFor(note));
        const int16_t targetStep  = static_cast<int16_t>(stepFor(note + param));
        const int16_t diff = static_cast<int16_t>(static_cast<uint16_t>(targetStep - currentStep));
        int16_t delta;
        if (slideSpeed != 0)
            delta = static_cast<int16_t>(diff / static_cast<int>(slideSpeed));
        else
            delta = (diff < 0) ? 1 : -1; // avr-libgcc __divmodhi4 by zero: quotient 0xffff, sign-corrected
        if (delta == 0)
            delta++;

        setMixerStep(static_cast<uint16_t>(mixerStep() + delta));

        slideStep = delta;
        flags |= TRACK_FLAGS_SLIDING;
        slideNote = static_cast<uint8_t>(note + param);
        break;
    }

    case PC_SLIDE_SPEED:
        slideSpeed = uparam;
        break;

    case PC_LOOP_START:
        loopCount = uparam;
        break;

    case PC_LOOP_END:
        if (loopCount > 0)
        {
            if (param != 0)
            {
                streamPos -= (param + 1) * 3;
            }
            else
            {
                for (;;)
                {
                    streamPos -= 3;
                    if (streamPos - 2 < 0)
                        break; // would scan before the patch start
                    const uint8_t c = readStream(streamPos - 2);
                    if (c == PC_LOOP_START || c == PATCH_END)
                        break;
                }
            }
            if (streamPos < 0)
                streamPos = 0;
            loopCount--;
        }
        break;

    default:
        // The kernel would call through a garbage function pointer. Ignore.
        break;
    }
}

// --------------------------------------------------------------------------
// ProcessMusic() for one track

void UzeboxVoice::processFrame(uint8_t masterVolume)
{
    // 1. envelope
    if (envelopeStep != 0)
    {
        int v = envelopeVol + envelopeStep;
        if (v < 0) v = 0;
        else if (v > 0xff) v = 0xff;
        envelopeVol = static_cast<uint8_t>(v);
    }

    // 2. pitch slide
    if (flags & TRACK_FLAGS_SLIDING)
    {
        const uint16_t s = static_cast<uint16_t>(mixerStep() + slideStep);
        setMixerStep(s);
        const uint16_t tStep = stepFor(slideNote);
        if ((slideStep > 0 && s >= tStep) || (slideStep < 0 && s <= tStep))
        {
            setMixerStep(tStep);
            flags &= ~TRACK_FLAGS_SLIDING;
        }
    }

    // (3. song / MIDI events: applied by the caller as they arrive)

    // 4. patch command stream
    if ((flags & TRACK_FLAGS_PLAYING) && streamPos >= 0 && (flags & TRACK_FLAGS_HOLD_ENV) == 0)
    {
        int guard = 0;
        while (patchCurrDeltaTime == patchNextDeltaTime)
        {
            if (++guard > kMaxCommandsPerFrame)
            {
                streamPos = -1;
                break;
            }

            const uint8_t c1 = readStream(streamPos++);
            if (c1 == PATCH_END)
            {
                flags &= ~TRACK_FLAGS_PRIORITY;
                streamPos = -1;
                break;
            }
            const uint8_t c2 = readStream(streamPos++);
            runCommand(c1, c2);

            patchNextDeltaTime = readStream(streamPos++);
            patchCurrDeltaTime = 0;
        }
        patchCurrDeltaTime++;
    }

    // 5. final channel volume
    uint16_t uVol = 0;
    if (flags & TRACK_FLAGS_PLAYING)
    {
        const uint8_t tVolume = (flags & TRACK_FLAGS_PRIORITY) ? 0xff : trackVol;

        if (noteVol != 0 && envelopeVol != 0 && tVolume != 0 && masterVolume != 0)
        {
            uVol = static_cast<uint16_t>((noteVol * tVolume) + 0x100);
            uVol >>= 8;
            uVol = static_cast<uint16_t>((uVol * envelopeVol) + 0x100);
            uVol >>= 8;
            uVol = static_cast<uint16_t>((uVol * expressionVol) + 0x100);
            uVol >>= 8;
            uVol = static_cast<uint16_t>((uVol * masterVolume) + 0x100);
            uVol >>= 8;

            if (tremoloLevel > 0)
            {
                uint8_t tmp = waveByte(0, tremoloPos); // wave 0 of the current table
                tmp = static_cast<uint8_t>(tmp - 128);
                uint16_t tVol = static_cast<uint16_t>((tremoloLevel * tmp) + 0x100);
                tVol >>= 8;
                uVol = static_cast<uint16_t>((uVol * (0xff - tVol)) + 0x100);
                uVol >>= 8;
            }
        }
        else
        {
            uVol = 0;
        }
        tremoloPos = static_cast<uint8_t>(tremoloPos + tremoloRate);
    }

    volume = static_cast<uint8_t>(uVol & 0xff);
}

// --------------------------------------------------------------------------
// soundMixerVsync.s inner loop, one channel

int UzeboxVoice::renderSample(uint8_t masterVolume)
{
    if (samplesUntilFrame <= 0)
    {
        processFrame(masterVolume);
        samplesUntilFrame = kSamplesPerFrame;
    }
    --samplesUntilFrame;

    int8_t sample;
    if (noise)
    {
        // dec r24 / brpl no_shift
        divider = static_cast<uint8_t>(divider - 1);
        if (divider & 0x80)
        {
            divider = static_cast<uint8_t>(noiseParams >> 1);
            const uint8_t lo = static_cast<uint8_t>(barrel & 0xff);
            const bool t = ((lo ^ (lo >> 1)) & 1) != 0;
            barrel = static_cast<uint16_t>(barrel >> 1);
            barrel = static_cast<uint16_t>(t ? (barrel | 0x4000) : (barrel & ~0x4000)); // bld r23,6
            if (!(noiseParams & 1))                                                   // 7-bit mode
                barrel = static_cast<uint16_t>(t ? (barrel | 0x0040) : (barrel & ~0x0040)); // bld r22,6
        }
        sample = (barrel & 1) ? int8_t(127) : int8_t(-128);
    }
    else
    {
        // add step to pos (frac + lo byte only; wraps within the 256-byte wave)
        pos = static_cast<uint16_t>(pos + step);
        sample = static_cast<int8_t>(waveByte(wave, pos >> 8));
    }

    // mulsu sample,vol -> keep the high byte, sign extended
    const int product = static_cast<int>(sample) * static_cast<int>(volume);
    return product >> 8; // arithmetic shift == AVR high byte + sbc sign extension
}

} // namespace uze
