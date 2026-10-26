/*
 * UzeSynth - mixer wavetables
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "Wavetable.hpp"
#include "PatchParser.hpp"
#include "UzeboxTables.hpp"

#include <cctype>
#include <cstdlib>

namespace uze {

namespace {

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const char* suffix)
{
    const std::string suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool looksBinary(const std::string& contents)
{
    const size_t n = contents.size() < 4096 ? contents.size() : 4096;
    size_t odd = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(contents[i]);
        if (c == 0) return true;
        if (c < 9 || (c > 13 && c < 32) || c > 126) ++odd;
    }
    return n > 0 && odd * 10 > n; // >10% non-text
}

// Parse one assembler numeric literal: 0x1F, $1F, 1Fh, 0b101, -12, 'a'
bool parseAsmNumber(std::string t, long& v)
{
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    size_t a = 0;
    while (a < t.size() && std::isspace(static_cast<unsigned char>(t[a]))) ++a;
    t = t.substr(a);
    if (t.empty()) return false;

    bool neg = false;
    if (t[0] == '-') { neg = true; t = t.substr(1); }
    else if (t[0] == '+') { t = t.substr(1); }
    if (t.empty()) return false;

    if (t.size() >= 3 && t[0] == '\'' && t[2] == '\'') { v = static_cast<unsigned char>(t[1]); }
    else
    {
        int base = 10;
        std::string digits = t;
        const std::string l = lower(t);
        if (l.size() > 2 && l[0] == '0' && l[1] == 'x') { base = 16; digits = t.substr(2); }
        else if (l.size() > 2 && l[0] == '0' && l[1] == 'b') { base = 2; digits = t.substr(2); }
        else if (l.size() > 1 && l[0] == '$') { base = 16; digits = t.substr(1); }
        else if (l.size() > 1 && l.back() == 'h') { base = 16; digits = t.substr(0, t.size() - 1); }
        char* end = nullptr;
        v = std::strtol(digits.c_str(), &end, base);
        if (end == digits.c_str() || *end != '\0') return false;
    }
    if (neg) v = -v;
    return true;
}

// Collect the operands of every .byte / .db directive
bool parseAsm(const std::string& text, std::vector<uint8_t>& out)
{
    bool found = false;
    size_t i = 0;
    while (i < text.size())
    {
        size_t eol = text.find('\n', i);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(i, eol - i);
        i = eol + 1;

        // strip comments: ';' and '//' (C preprocessor runs on .s files too)
        size_t cut = line.find(';');
        const size_t cpp = line.find("//");
        if (cpp != std::string::npos && (cut == std::string::npos || cpp < cut)) cut = cpp;
        if (cut != std::string::npos) line = line.substr(0, cut);

        const std::string l = lower(line);
        size_t d = l.find(".byte");
        size_t skip = 5;
        if (d == std::string::npos) { d = l.find(".db"); skip = 3; }
        if (d == std::string::npos) continue;
        const char next = (d + skip < l.size()) ? l[d + skip] : ' ';
        if (!std::isspace(static_cast<unsigned char>(next))) continue;

        std::string ops = line.substr(d + skip);
        size_t start = 0;
        while (start <= ops.size())
        {
            size_t comma = ops.find(',', start);
            if (comma == std::string::npos) comma = ops.size();
            long v;
            if (parseAsmNumber(ops.substr(start, comma - start), v))
            {
                out.push_back(static_cast<uint8_t>(v & 0xff));
                found = true;
            }
            start = comma + 1;
        }
    }
    return found;
}

} // namespace

const Wavetable& defaultWavetable()
{
    static const Wavetable table = [] {
        Wavetable t;
        t.data.assign(kWaves, kWaves + kNumWaves * kWaveLen);
        return t;
    }();
    return table;
}

bool parseWavetable(const std::string& contents, const std::string& fileName,
                    Wavetable& out, std::string& error, std::string& warning)
{
    error.clear();
    warning.clear();
    std::vector<uint8_t> bytes;

    const std::string ext = lower(fileName);
    if (endsWith(ext, ".raw") || endsWith(ext, ".bin") || endsWith(ext, ".pcm") || looksBinary(contents))
    {
        bytes.assign(contents.begin(), contents.end());
    }
    else if (!parseAsm(contents, bytes))
    {
        // C arrays: concatenate them in file order
        for (const auto& a : parseCByteArrays(contents))
            bytes.insert(bytes.end(), a.begin(), a.end());
    }

    if (bytes.size() < 256)
    {
        error = bytes.empty() ? "No wave data found (expected .byte lines, C arrays or raw bytes)"
                              : "Wave data is shorter than one 256-byte wave";
        return false;
    }

    size_t usable = bytes.size() / 256;
    if (usable > static_cast<size_t>(kMaxWaves))
    {
        usable = kMaxWaves;
        warning = "Only the first 256 waves are addressable";
    }
    else if (bytes.size() % 256 != 0)
    {
        warning = "Ignored " + std::to_string(bytes.size() % 256) + " trailing bytes";
    }

    bytes.resize(usable * 256);
    out.data = bytes;
    return true;
}

std::string toHex(const std::vector<uint8_t>& data)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (uint8_t b : data)
    {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

bool fromHex(const std::string& hex, std::vector<uint8_t>& out)
{
    out.clear();
    if (hex.size() % 2 != 0)
        return false;
    out.reserve(hex.size() / 2);
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = val(hex[i]), lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) { out.clear(); return false; }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace uze
