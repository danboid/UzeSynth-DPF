/*
 * UzeSynth - patch editing support: command metadata, conversion between a
 * patch's byte stream and an editable command list, and writing a patch bank
 * back out as a Uzebox .inc file.
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#pragma once

#include "PatchParser.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace uze {

constexpr int kNumCommands = 15; // PC_ENV_SPEED .. PC_LOOP_END

struct CommandInfo {
    const char* id;        // C name, e.g. "PC_ENV_SPEED"
    const char* label;     // short human name
    bool hasParam;         // false for commands whose parameter is ignored
    bool isSigned;         // parameter is used as a signed char
    const char* help;      // one-line explanation
};

const CommandInfo& commandInfo(uint8_t cmd);

// Parameter value as shown to the user (signed commands as -128..127)
int paramToDisplay(uint8_t cmd, uint8_t raw);
uint8_t displayToParam(uint8_t cmd, int value);   // clamps to the valid range
int paramMin(uint8_t cmd);
int paramMax(uint8_t cmd);

// Human-readable meaning of a parameter ("square 50%", "C4", "15-bit, div 2"...)
// waveCount: number of waves in the loaded table (0 = the kernel's 10 default waves)
std::string describeParam(uint8_t cmd, uint8_t raw, int waveCount);

std::string noteName(int note); // 60 -> "C4"

// ---- editable form of a command stream ------------------------------------

struct EditCommand {
    uint8_t delta = 0;   // frames to wait before this command
    uint8_t cmd = PC_ENV_SPEED;
    uint8_t param = 0;
};

struct EditStream {
    std::vector<EditCommand> commands;
    uint8_t endDelta = 0; // frames before PATCH_END
};

EditStream decodeStream(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encodeStream(const EditStream& s);
int streamLengthFrames(const EditStream& s); // ignoring loops

// ---- names ----------------------------------------------------------------

bool isValidIdentifier(const std::string& s);
// 'base' if unused (by patches and sample arrays), else base_2, base_3...
std::string uniqueName(const PatchBank& bank, const std::string& base);

// ---- writing --------------------------------------------------------------

// Generate a complete .inc file: sample arrays, command stream arrays and the
// PatchStruct table (every patch is included; the table order is kept).
std::string generatePatchFile(const PatchBank& bank);

} // namespace uze
