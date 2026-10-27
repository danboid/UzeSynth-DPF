/*
 * UzeSynth - patch editing support
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "PatchEdit.hpp"
#include "UzeboxVoice.hpp"

#include <cctype>
#include <cstdio>
#include <map>
#include <set>

namespace uze {

namespace {

const CommandInfo kCommands[kNumCommands] = {
    { "PC_ENV_SPEED",     "Envelope speed",  true,  true,  "Volume change per frame (negative = fade out)" },
    { "PC_NOISE_PARAMS",  "Noise params",    true,  false, "Noise channel: bit 0 = 15-bit LFSR, bits 1-7 = divider" },
    { "PC_WAVE",          "Wave",            true,  false, "Select the waveform" },
    { "PC_NOTE_UP",       "Note up",         true,  true,  "Raise the note by N semitones" },
    { "PC_NOTE_DOWN",     "Note down",       true,  true,  "Lower the note by N semitones" },
    { "PC_NOTE_CUT",      "Note cut",        false, false, "Stop the note immediately" },
    { "PC_NOTE_HOLD",     "Note hold",       false, false, "Pause the patch until note-off" },
    { "PC_ENV_VOL",       "Envelope volume", true,  false, "Set the envelope volume (0-255)" },
    { "PC_PITCH",         "Pitch",           true,  false, "Set an absolute note" },
    { "PC_TREMOLO_LEVEL", "Tremolo level",   true,  false, "Tremolo depth" },
    { "PC_TREMOLO_RATE",  "Tremolo rate",    true,  false, "Tremolo speed" },
    { "PC_SLIDE",         "Slide",           true,  true,  "Slide pitch by N semitones" },
    { "PC_SLIDE_SPEED",   "Slide speed",     true,  false, "Frames a slide takes (divider)" },
    { "PC_LOOP_START",    "Loop start",      true,  false, "Start of a loop; repeat count" },
    { "PC_LOOP_END",      "Loop end",        true,  true,  "Jump back: 0 = to loop start, N = N commands back" },
};

const CommandInfo kUnknown = { "", "Unknown", true, false, "Not a kernel command (ignored)" };

const char* const kWaveNames[10] = {
    "sine", "sawtooth", "triangle", "square 25%", "square 50%", "square 75%",
    "fuzzy sine 1", "fuzzy sine 2", "fuzzy sine 3", "filtered square",
};

std::string sanitizeComment(const std::string& s)
{
    std::string r;
    for (char c : s)
        r += (c == '\n' || c == '\r') ? ' ' : c;
    return r;
}

std::string formatParam(uint8_t cmd, uint8_t raw)
{
    return std::to_string(paramToDisplay(cmd, raw));
}

void appendStream(std::string& out, const std::vector<uint8_t>& b)
{
    size_t i = 0;
    bool ended = false;
    while (i < b.size())
    {
        const uint8_t delta = b[i];
        if (i + 1 >= b.size() || b[i + 1] == PATCH_END)
        {
            out += std::to_string(delta) + ",PATCH_END\n";
            ended = true;
            break;
        }
        const uint8_t cmd = b[i + 1];
        const uint8_t param = i + 2 < b.size() ? b[i + 2] : 0;
        const std::string name = cmd < kNumCommands ? kCommands[cmd].id : std::to_string(cmd);
        out += std::to_string(delta) + "," + name + "," + formatParam(cmd, param) + ",\n";
        i += 3;
    }
    if (!ended)
        out += "0,PATCH_END\n";
}

} // namespace

const CommandInfo& commandInfo(uint8_t cmd)
{
    return cmd < kNumCommands ? kCommands[cmd] : kUnknown;
}

int paramToDisplay(uint8_t cmd, uint8_t raw)
{
    return commandInfo(cmd).isSigned ? static_cast<int>(static_cast<int8_t>(raw)) : static_cast<int>(raw);
}

int paramMin(uint8_t cmd) { return commandInfo(cmd).isSigned ? -128 : 0; }
int paramMax(uint8_t cmd) { return commandInfo(cmd).isSigned ? 127 : 255; }

uint8_t displayToParam(uint8_t cmd, int v)
{
    if (v < paramMin(cmd)) v = paramMin(cmd);
    if (v > paramMax(cmd)) v = paramMax(cmd);
    return static_cast<uint8_t>(v & 0xff);
}

std::string noteName(int n)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (n < 0) n = 0;
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

std::string describeParam(uint8_t cmd, uint8_t raw, int waveCount)
{
    const int v = paramToDisplay(cmd, raw);
    char buf[64];
    switch (cmd)
    {
    case PC_ENV_SPEED:
        if (v == 0) return "hold level";
        std::snprintf(buf, sizeof(buf), "%+d per frame", v);
        return buf;
    case PC_NOISE_PARAMS:
        std::snprintf(buf, sizeof(buf), "%s, divider %d", (raw & 1) ? "15-bit" : "7-bit", raw >> 1);
        return buf;
    case PC_WAVE:
    {
        const int count = waveCount > 0 ? waveCount : 10;
        const int w = raw % count;
        const std::string name = waveCount > 0 ? "wave " + std::to_string(w) : std::string(kWaveNames[w]);
        if (raw >= count)  // the kernel reads past the table; UzeSynth wraps around
            return "no such wave (plays " + name + ")";
        return name;
    }
    case PC_NOTE_UP:
    case PC_NOTE_DOWN:
    case PC_SLIDE:
        std::snprintf(buf, sizeof(buf), "%+d semitone%s", v, (v == 1 || v == -1) ? "" : "s");
        return buf;
    case PC_ENV_VOL:
        std::snprintf(buf, sizeof(buf), "%d%%", (v * 100 + 127) / 255);
        return buf;
    case PC_PITCH:
        return v < 127 ? noteName(v) : "out of range";
    case PC_TREMOLO_RATE:
        std::snprintf(buf, sizeof(buf), "%.1f Hz", v * (kNativeSampleRate / kSamplesPerFrame) / 256.0);
        return buf;
    case PC_SLIDE_SPEED:
        return v == 0 ? "(0: avoid)" : "";
    case PC_LOOP_START:
        std::snprintf(buf, sizeof(buf), "%d repeat%s", v, v == 1 ? "" : "s");
        return buf;
    case PC_LOOP_END:
        if (v == 0) return "to loop start";
        std::snprintf(buf, sizeof(buf), "back %d command%s", v, v == 1 ? "" : "s");
        return buf;
    case PC_NOTE_CUT:
    case PC_NOTE_HOLD:
        return "";
    default:
        return "";
    }
}

EditStream decodeStream(const std::vector<uint8_t>& b)
{
    EditStream s;
    size_t i = 0;
    while (i < b.size())
    {
        const uint8_t delta = b[i];
        if (i + 1 >= b.size() || b[i + 1] == PATCH_END)
        {
            s.endDelta = delta;
            return s;
        }
        EditCommand c;
        c.delta = delta;
        c.cmd = b[i + 1];
        c.param = i + 2 < b.size() ? b[i + 2] : 0;
        s.commands.push_back(c);
        i += 3;
    }
    return s;
}

std::vector<uint8_t> encodeStream(const EditStream& s)
{
    std::vector<uint8_t> b;
    for (const EditCommand& c : s.commands)
    {
        b.push_back(c.delta);
        b.push_back(c.cmd);
        b.push_back(c.param);
    }
    b.push_back(s.endDelta);
    b.push_back(PATCH_END);
    return b;
}

int streamLengthFrames(const EditStream& s)
{
    int n = s.endDelta;
    for (const EditCommand& c : s.commands)
        n += c.delta;
    return n;
}

bool isValidIdentifier(const std::string& s)
{
    if (s.empty() || s.size() > 64)
        return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
        return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    static const std::set<std::string> reserved = {
        "patches", "NULL", "const", "char", "struct", "int", "unsigned", "signed", "PROGMEM", "sizeof",
        "if", "else", "for", "while", "return", "static", "void",
    };
    return reserved.count(s) == 0 && s.compare(0, 3, "PC_") != 0;
}

std::string uniqueName(const PatchBank& bank, const std::string& base)
{
    auto taken = [&](const std::string& n) {
        if (n == "patches") return true;
        for (const Patch& p : bank.patches)
            if (p.name == n || p.sampleName == n || p.cmdName == n)
                return true;
        return false;
    };
    if (!taken(base))
        return base;
    for (int i = 2;; ++i)
    {
        const std::string n = base + "_" + std::to_string(i);
        if (!taken(n))
            return n;
    }
}

std::string generatePatchFile(const PatchBank& bank)
{
    std::string out;
    out += "/*\n"
           " * Uzebox patches - written by UzeSynth\n"
           " * Use with InitMusicPlayer(patches);\n"
           " */\n\n";

    // ---- sample arrays (one per name)
    std::set<std::string> writtenSamples;
    for (const Patch& p : bank.patches)
    {
        if (p.data.type != PatchType::Pcm || p.sampleName.empty() || p.data.sample.empty()
            || writtenSamples.count(p.sampleName))
            continue;
        writtenSamples.insert(p.sampleName);
        out += "const char " + p.sampleName + "[] PROGMEM = {";
        for (size_t i = 0; i < p.data.sample.size(); ++i)
        {
            if (i % 16 == 0) out += "\n";
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d,", static_cast<int>(static_cast<int8_t>(p.data.sample[i])));
            out += buf;
        }
        out += "\n};\n\n";
    }

    // ---- command stream arrays
    std::map<std::string, std::vector<uint8_t>> written; // name -> bytes
    std::vector<std::string> cmdNames(bank.patches.size());
    for (size_t i = 0; i < bank.patches.size(); ++i)
    {
        const Patch& p = bank.patches[i];
        const bool pcm = p.data.type == PatchType::Pcm;
        if (pcm && p.data.stream.empty())
            continue; // cmdStream = NULL

        std::vector<uint8_t> bytes = p.data.stream;
        if (bytes.empty())
            bytes = { 0, PATCH_END };

        std::string name = pcm ? (p.cmdName.empty() ? p.name + "_cmd" : p.cmdName) : p.name;
        // share identical streams; rename clashes
        for (int n = 2;; ++n)
        {
            auto it = written.find(name);
            if (it == written.end() || it->second == bytes)
                break;
            name = (pcm ? (p.cmdName.empty() ? p.name + "_cmd" : p.cmdName) : p.name) + "_" + std::to_string(n);
        }
        cmdNames[i] = name;
        if (written.count(name))
            continue;
        written[name] = bytes;

        if (!p.comment.empty())
            out += "//" + sanitizeComment(p.comment) + "\n";
        out += "const char " + name + "[] PROGMEM ={\n";
        appendStream(out, bytes);
        out += "};\n\n";
    }

    // ---- patch table: existing order, then patches not yet in it
    std::vector<int> table = bank.table;
    std::vector<bool> inTable(bank.patches.size(), false);
    for (int t : table)
        if (t >= 0 && t < static_cast<int>(bank.patches.size()))
            inTable[static_cast<size_t>(t)] = true;
    for (size_t i = 0; i < bank.patches.size(); ++i)
        if (!inTable[i])
            table.push_back(static_cast<int>(i));

    out += "const struct PatchStruct patches[] PROGMEM = {\n";
    for (size_t n = 0; n < table.size(); ++n)
    {
        const int t = table[n];
        std::string entry;
        if (t < 0 || t >= static_cast<int>(bank.patches.size()))
        {
            entry = "{0,NULL,NULL,0,0}";
        }
        else
        {
            const Patch& p = bank.patches[static_cast<size_t>(t)];
            const std::string cmd = cmdNames[static_cast<size_t>(t)].empty() ? "NULL" : cmdNames[static_cast<size_t>(t)];
            if (p.data.type == PatchType::Pcm)
                entry = "{2," + (p.sampleName.empty() ? std::string("NULL") : p.sampleName) + "," + cmd + ","
                      + std::to_string(p.data.loopStart) + "," + std::to_string(p.data.loopEnd) + "}";
            else
                entry = std::string("{") + (p.data.type == PatchType::Noise ? "1" : "0") + ",NULL," + cmd + ",0,0}";
        }
        out += entry + (n + 1 < table.size() ? "," : "") + "\n";
    }
    out += "};\n";
    return out;
}

} // namespace uze
