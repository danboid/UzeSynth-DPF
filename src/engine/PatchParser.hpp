/*
 * UzeSynth - parser for Uzebox patch include files (.inc)
 *
 * Understands the C subset used by Uzebox games and Uzebox Patch Studio:
 *
 *   //INST: Synth Piano
 *   const char patch00[] PROGMEM ={
 *   0,PC_WAVE,6,
 *   0,PC_ENV_SPEED,-5,
 *   0,PATCH_END
 *   };
 *
 *   const struct PatchStruct patches[] PROGMEM = {
 *   {0,NULL,patch00,0,0},
 *   ...
 *   };
 *
 * Simple "#define NAME value" macros and constant expressions are supported.
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#pragma once

#include "UzeboxVoice.hpp"

#include <string>
#include <vector>

namespace uze {

struct Patch {
    std::string name;     // C identifier of the command stream array (PCM: of the sample)
    std::string comment;  // comment directly above the declaration, if any
    std::string sampleName;      // PCM: name of the sample array
    bool sampleMissing = false;  // PCM: sample array not found in the loaded file(s)
    PatchData   data;
};

struct PatchBank {
    std::vector<Patch> patches;
    // The kernel's patch table (first PatchStruct array): table[n] is the index
    // into 'patches' of patch number n, or -1 (PCM or missing). Drum kit and
    // sound FX modes select patches by these numbers, as TriggerNote/TriggerFx do.
    // Without a PatchStruct array, patch numbers follow file order.
    std::vector<int> table;
    std::string error;    // non-empty when nothing usable was found
    // PCM sample arrays referenced by the patch table but not defined in the
    // loaded text (they often live in a separate .inc file)
    std::vector<std::string> missingSamples;

    int indexOf(const std::string& name) const;
    const PatchData* tableEntry(int number) const;
};

// Parse the text of a .inc file. Never throws.
PatchBank parsePatchFile(const std::string& text);

// All char/u8 arrays in the file, in order (used for C-format wavetables)
std::vector<std::vector<uint8_t>> parseCByteArrays(const std::string& text);

// Look in the directory of 'patchPath' (and a "data" subdirectory) for .inc
// files that define any of the 'missing' sample arrays. Returns their paths.
std::vector<std::string> findSampleFiles(const std::string& patchPath, const std::vector<std::string>& missing);

// Read a whole file into a string (returns false on error or if > maxBytes).
bool readTextFile(const std::string& path, std::string& out, size_t maxBytes = 4u * 1024u * 1024u);

} // namespace uze
