/*
 * UzeSynth - mixer wavetables
 *
 * The kernel plays waves from the 256-byte aligned table at the 'waves' label.
 * Games may replace the default table with their own via
 * -DMIXER_WAVES="sounds.inc" (an assembler file of .byte lines).
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uze {

constexpr int kMaxWaves = 256; // PC_WAVE's parameter is a byte

struct Wavetable {
    std::vector<uint8_t> data;   // count() * 256 signed 8-bit samples
    int count() const { return static_cast<int>(data.size() / 256); }
    const uint8_t* wave(int i) const { return data.data() + static_cast<size_t>(i) * 256; }
};

// The kernel's built-in 10 waves (kernel/data/sounds.inc)
const Wavetable& defaultWavetable();

// Load a wavetable from file contents. Accepted formats:
//  - assembler, as used with MIXER_WAVES:   .byte 0x00,0x03,... (also .db)
//  - C arrays:   const char waves[] PROGMEM = { 0, 3, 6, ... };
//  - raw binary: N * 256 bytes (.raw / .bin)
// Returns false and sets 'error' if no complete 256-byte wave was found.
// 'warning' is set if trailing bytes were ignored or there were too many waves.
bool parseWavetable(const std::string& contents, const std::string& fileName,
                    Wavetable& out, std::string& error, std::string& warning);

// Hex helpers used to keep binary data in plugin state strings
std::string toHex(const std::vector<uint8_t>& data);
bool fromHex(const std::string& hex, std::vector<uint8_t>& out);

} // namespace uze
