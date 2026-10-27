/*
 * UzeSynth - Uzebox sound engine plugin (DSP side)
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "DistrhoPlugin.hpp"

#include "../engine/PatchParser.hpp"
#include "../engine/UzeboxSynth.hpp"
#include "../engine/Wavetable.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

START_NAMESPACE_DISTRHO

class UzeSynthPlugin : public Plugin
{
public:
    UzeSynthPlugin()
        : Plugin(kParamCount, 0, kStateCount)
    {
        fParams[kParamPolyphony]    = 1.0f;
        fParams[kParamMasterVolume] = static_cast<float>(uze::kDefaultMasterVolume);
        fParams[kParamOutputMode]   = 1.0f;
        fParams[kParamPlayMode]     = 0.0f;
        fParams[kParamMixer]        = 0.0f;
        applyParams();
        sampleRateChanged(getSampleRate());
    }

    ~UzeSynthPlugin() override
    {
        delete fActive;
        delete fPending.data;
        delete fRetired;
    }

protected:
    // ------------------------------------------------------------------------
    // Information

    const char* getLabel() const override       { return "UzeSynth"; }
    const char* getDescription() const override { return "Bit-exact emulation of the Uzebox game console sound engine"; }
    const char* getMaker() const override       { return "Uzebox community"; }
    const char* getHomePage() const override    { return "https://uzebox.org"; }
    const char* getLicense() const override     { return "GPL-3.0-or-later"; }
    uint32_t getVersion() const override        { return d_version(0, 4, 1); }

    // ------------------------------------------------------------------------
    // Init

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override
    {
        port.groupId = kPortGroupStereo;
        Plugin::initAudioPort(input, index, port);
    }

    void initParameter(uint32_t index, Parameter& p) override
    {
        p.hints = kParameterIsAutomatable | kParameterIsInteger;
        switch (index)
        {
        case kParamPolyphony:
            p.name = "Polyphony";
            p.symbol = "polyphony";
            p.description = "1 = a single Uzebox track (authentic). Higher values run several tracks.";
            p.ranges.min = 1.0f;
            p.ranges.max = static_cast<float>(uze::UzeboxSynth::kMaxVoices);
            p.ranges.def = 1.0f;
            break;
        case kParamMasterVolume:
            p.name = "Master Volume";
            p.symbol = "master_volume";
            p.description = "Kernel master volume (0x6f = DEFAULT_MASTER_VOL)";
            p.ranges.min = 0.0f;
            p.ranges.max = 255.0f;
            p.ranges.def = static_cast<float>(uze::kDefaultMasterVolume);
            break;
        case kParamOutputMode:
        {
            p.name = "Output";
            p.symbol = "output_mode";
            p.ranges.min = 0.0f;
            p.ranges.max = 1.0f;
            p.ranges.def = 1.0f;
            ParameterEnumerationValue* const v = new ParameterEnumerationValue[2];
            v[0].label = "Raw (stepped)";
            v[0].value = 0.0f;
            v[1].label = "Smooth (band-limited)";
            v[1].value = 1.0f;
            p.enumValues.count = 2;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = v;
            break;
        }
        case kParamPlayMode:
        {
            p.name = "Mode";
            p.symbol = "play_mode";
            p.description = "Melodic: selected patch. Drum kit: note number = patch number, noise channel. "
                            "Sound FX: note number = patch number, played like TriggerFx().";
            p.ranges.min = 0.0f;
            p.ranges.max = 2.0f;
            p.ranges.def = 0.0f;
            ParameterEnumerationValue* const v = new ParameterEnumerationValue[3];
            v[0].label = "Melodic";
            v[0].value = 0.0f;
            v[1].label = "Drum kit";
            v[1].value = 1.0f;
            v[2].label = "Sound FX";
            v[2].value = 2.0f;
            p.enumValues.count = 3;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = v;
            break;
        }
        case kParamMixer:
        {
            p.name = "Kernel Mixer";
            p.symbol = "kernel_mixer";
            p.description = "VSYNC: default kernel mixer (PCM games built with MIXER_CHAN4_TYPE=1). "
                            "Inline: SOUND_MIXER=1 (PCM on the 5th channel).";
            p.ranges.min = 0.0f;
            p.ranges.max = 1.0f;
            p.ranges.def = 0.0f;
            ParameterEnumerationValue* const v = new ParameterEnumerationValue[2];
            v[0].label = "VSYNC";
            v[0].value = 0.0f;
            v[1].label = "Inline";
            v[1].value = 1.0f;
            p.enumValues.count = 2;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = v;
            break;
        }
        }
    }

    void initState(uint32_t index, State& state) override
    {
        state.hints = 0; // saved by the host and shared with the UI
        state.defaultValue = "";
        switch (index)
        {
        case kStateFileText:  state.key = UZESYNTH_STATE_FILE_TEXT;  state.label = "Patch file contents"; break;
        case kStateFileName:  state.key = UZESYNTH_STATE_FILE_NAME;  state.label = "Patch file name";     break;
        case kStatePatchName: state.key = UZESYNTH_STATE_PATCH_NAME; state.label = "Patch";               break;
        case kStateWaves:     state.key = UZESYNTH_STATE_WAVES;      state.label = "Wavetable data";      break;
        case kStateWavesName: state.key = UZESYNTH_STATE_WAVES_NAME; state.label = "Wavetable file name"; break;
        case kStateModified:  state.key = UZESYNTH_STATE_MODIFIED;   state.label = "Edited, unsaved";     break;
        }
    }

    // ------------------------------------------------------------------------
    // Parameters

    float getParameterValue(uint32_t index) const override
    {
        return index < kParamCount ? fParams[index] : 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override
    {
        if (index >= kParamCount)
            return;
        fParams[index] = value;
        applyParams();
    }

    // ------------------------------------------------------------------------
    // State (called from a non-realtime thread)

    String getState(const char* key) const override
    {
        const std::lock_guard<std::mutex> lock(fStateMutex);
        if (std::strcmp(key, UZESYNTH_STATE_FILE_TEXT) == 0)  return String(fFileText.c_str());
        if (std::strcmp(key, UZESYNTH_STATE_FILE_NAME) == 0)  return String(fFileName.c_str());
        if (std::strcmp(key, UZESYNTH_STATE_PATCH_NAME) == 0) return String(fPatchName.c_str());
        if (std::strcmp(key, UZESYNTH_STATE_WAVES) == 0)      return String(fWavesHex.c_str());
        if (std::strcmp(key, UZESYNTH_STATE_WAVES_NAME) == 0) return String(fWavesName.c_str());
        if (std::strcmp(key, UZESYNTH_STATE_MODIFIED) == 0)   return String(fModified.c_str());
        return String();
    }

    void setState(const char* key, const char* value) override
    {
        if (value == nullptr)
            value = "";

        const std::lock_guard<std::mutex> lock(fStateMutex);

        // free sound data the audio thread has finished with
        delete fRetired;
        fRetired = nullptr;

        if (std::strcmp(key, UZESYNTH_STATE_FILE_TEXT) == 0)
        {
            fFileText = value;
            fBank = uze::parsePatchFile(fFileText);
            publishSoundData();
        }
        else if (std::strcmp(key, UZESYNTH_STATE_WAVES) == 0)
        {
            fWavesHex = value;
            std::vector<uint8_t> bytes;
            if (!fWavesHex.empty() && uze::fromHex(fWavesHex, bytes) && bytes.size() >= 256)
            {
                bytes.resize(std::min<size_t>(bytes.size() / 256, uze::kMaxWaves) * 256);
                fWaves.data = bytes;
            }
            else
            {
                fWaves = uze::defaultWavetable();
            }
            publishSoundData();
        }
        else if (std::strcmp(key, UZESYNTH_STATE_FILE_NAME) == 0)
        {
            fFileName = value;
        }
        else if (std::strcmp(key, UZESYNTH_STATE_WAVES_NAME) == 0)
        {
            fWavesName = value;
        }
        else if (std::strcmp(key, UZESYNTH_STATE_MODIFIED) == 0)
        {
            fModified = value;
        }
        else if (std::strcmp(key, UZESYNTH_STATE_PATCH_NAME) == 0)
        {
            fPatchName = value;
            const int idx = fBank.indexOf(fPatchName);
            fPending.index = idx < 0 ? 0 : idx;
            fPending.hasIndex = true;
        }
    }

    // ------------------------------------------------------------------------
    // Audio

    void sampleRateChanged(double newSampleRate) override
    {
        fSynth.setSampleRate(newSampleRate);
        setLatency(static_cast<uint32_t>(fSynth.latencySamples()));
    }

    void activate() override
    {
        fSynth.allSoundOff();
    }

    void run(const float**, float** outputs, uint32_t frames,
             const MidiEvent* midiEvents, uint32_t midiEventCount) override
    {
        pickUpPendingState();

        float* const outL = outputs[0];
        float* const outR = outputs[1];

        uint32_t pos = 0;
        for (uint32_t e = 0; e < midiEventCount; ++e)
        {
            const MidiEvent& ev = midiEvents[e];
            const uint32_t at = ev.frame < frames ? ev.frame : frames;
            if (at > pos)
            {
                fSynth.render(outL + pos, at - pos);
                pos = at;
            }
            handleMidi(ev);
        }
        if (pos < frames)
            fSynth.render(outL + pos, frames - pos);

        std::memcpy(outR, outL, sizeof(float) * frames);
    }

private:
    struct Pending {
        uze::SoundData* data = nullptr;
        int index = 0;
        bool hasIndex = false;
    };

    uze::UzeboxSynth fSynth;
    float fParams[kParamCount];

    // Non-realtime side (guarded by fStateMutex)
    mutable std::mutex fStateMutex; // DPF's Mutex is not implemented for MSVC
    std::string fFileText, fFileName, fPatchName, fWavesHex, fWavesName, fModified;
    uze::PatchBank fBank;
    uze::Wavetable fWaves = uze::defaultWavetable();
    Pending fPending;
    uze::SoundData* fRetired = nullptr;

    // Audio thread only
    uze::SoundData* fActive = nullptr;

    // Build an immutable copy of the current bank + wavetable for the audio thread
    void publishSoundData()
    {
        uze::SoundData* d = new uze::SoundData();
        d->bank = fBank;
        d->waves = fWaves;
        delete fPending.data; // never picked up by the audio thread
        fPending.data = d;
        const int idx = fBank.indexOf(fPatchName);
        fPending.index = idx < 0 ? 0 : idx;
        fPending.hasIndex = true;
    }

    void applyParams()
    {
        fSynth.setPolyphony(static_cast<int>(fParams[kParamPolyphony] + 0.5f));
        float mv = fParams[kParamMasterVolume];
        mv = mv < 0.0f ? 0.0f : (mv > 255.0f ? 255.0f : mv);
        fSynth.setMasterVolume(static_cast<uint8_t>(mv + 0.5f));
        fSynth.setOutputMode(fParams[kParamOutputMode] >= 0.5f ? uze::UzeboxSynth::OutputMode::Smooth
                                                               : uze::UzeboxSynth::OutputMode::Raw);
        int mode = static_cast<int>(fParams[kParamPlayMode] + 0.5f);
        mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
        fSynth.setPlayMode(static_cast<uze::UzeboxSynth::PlayMode>(mode));
        fSynth.setMixer(fParams[kParamMixer] >= 0.5f ? uze::UzeboxVoice::Mixer::Inline
                                                     : uze::UzeboxVoice::Mixer::Vsync);
    }

    // Never blocks the audio thread: if the host/UI is busy changing state we
    // simply pick the change up on the next block.
    void pickUpPendingState()
    {
        if (!fStateMutex.try_lock())
            return;

        if (fPending.data != nullptr && fRetired == nullptr)
        {
            fRetired = fActive;          // freed later by setState() or the destructor
            fActive = fPending.data;
            fPending.data = nullptr;
            fSynth.setSoundData(fActive);
        }
        if (fPending.hasIndex && fPending.data == nullptr)
        {
            fSynth.setPatchIndex(fPending.index);
            fPending.hasIndex = false;
        }

        fStateMutex.unlock();
    }

    void handleMidi(const MidiEvent& ev)
    {
        const uint8_t* data = ev.size > MidiEvent::kDataSize ? ev.dataExt : ev.data;
        if (ev.size < 2 || data == nullptr)
            return;

        const uint8_t status = data[0] & 0xF0;
        const uint8_t d1 = data[1] & 0x7F;
        const uint8_t d2 = ev.size > 2 ? (data[2] & 0x7F) : 0;

        switch (status)
        {
        case 0x90:
            if (d2 != 0) fSynth.noteOn(d1, d2);
            else         fSynth.noteOff(d1);
            break;
        case 0x80:
            fSynth.noteOff(d1);
            break;
        case 0xB0:
            fSynth.controlChange(d1, d2);
            break;
        default:
            break;
        }
    }

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UzeSynthPlugin)
};

Plugin* createPlugin()
{
    return new UzeSynthPlugin();
}

END_NAMESPACE_DISTRHO
