/*
 * UzeSynth - Uzebox sound engine plugin
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND    "Uzebox"
#define DISTRHO_PLUGIN_NAME     "UzeSynth"
#define DISTRHO_PLUGIN_URI      "urn:uzebox:uzesynth"
#define DISTRHO_PLUGIN_CLAP_ID  "org.uzebox.uzesynth"

#define DISTRHO_PLUGIN_BRAND_ID  Uzbx
#define DISTRHO_PLUGIN_UNIQUE_ID UzSy

#define DISTRHO_PLUGIN_HAS_UI          1
#define DISTRHO_PLUGIN_IS_RT_SAFE      1
#define DISTRHO_PLUGIN_IS_SYNTH        1
#define DISTRHO_PLUGIN_NUM_INPUTS      0
#define DISTRHO_PLUGIN_NUM_OUTPUTS     2
#define DISTRHO_PLUGIN_WANT_LATENCY    1
#define DISTRHO_PLUGIN_WANT_STATE      1
#define DISTRHO_PLUGIN_WANT_FULL_STATE 1

#define DISTRHO_UI_USE_NANOVG          1
#define DISTRHO_UI_FILE_BROWSER        1
#define DISTRHO_UI_USER_RESIZABLE      0
#define DISTRHO_UI_DEFAULT_WIDTH       520
#define DISTRHO_UI_DEFAULT_HEIGHT      460

#define DISTRHO_PLUGIN_LV2_CATEGORY    "lv2:InstrumentPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|Synth"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "instrument", "synthesizer", "stereo"

enum Parameters {
    kParamPolyphony = 0,
    kParamMasterVolume,
    kParamOutputMode,
    kParamPlayMode,      // 0 melodic, 1 drum kit (noise channel), 2 sound FX
    kParamMixer,         // 0 VSYNC mixer (kernel default), 1 inline mixer
    kParamCount
};

enum States {
    kStateFileText = 0,  // full text of the loaded .inc file(s) (so sessions don't depend on the files)
    kStateFileName,      // path(s) they were loaded from, one per line, for display
    kStatePatchName,     // selected patch (array name)
    kStateWaves,         // custom wavetable as hex, empty = kernel default waves
    kStateWavesName,     // path the wavetable was loaded from, for display
    kStateModified,      // "1" if the patches were edited and not saved to a file
    kStateCount
};

#define UZESYNTH_STATE_FILE_TEXT  "file"
#define UZESYNTH_STATE_FILE_NAME  "filename"
#define UZESYNTH_STATE_PATCH_NAME "patch"
#define UZESYNTH_STATE_WAVES      "waves"
#define UZESYNTH_STATE_WAVES_NAME "wavesname"
#define UZESYNTH_STATE_MODIFIED   "modified"

#endif // DISTRHO_PLUGIN_INFO_H_INCLUDED
