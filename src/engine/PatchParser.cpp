/*
 * UzeSynth - parser for Uzebox patch include files (.inc)
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "PatchParser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>

namespace uze {

namespace {

// --------------------------------------------------------------------------
// Tokenizer

enum class Tok { Ident, Number, Punct, Comment, Other };

struct Token {
    Tok kind;
    std::string text;
    long value = 0;          // for numbers / char literals
    bool newlineBefore = false;
};

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parseNumber(const std::string& t, long& out)
{
    std::string s = t;
    while (!s.empty() && (s.back() == 'u' || s.back() == 'U' || s.back() == 'l' || s.back() == 'L'))
        s.pop_back();
    if (s.empty())
        return false;

    int base = 10;
    size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; i = 2; }
    else if (s.size() > 1 && s[0] == '0') { base = 8; i = 1; }

    long v = 0;
    for (; i < s.size(); ++i)
    {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        if (d >= base) return false;
        v = v * base + d;
        if (v > 0x7fffffffL) return false;
    }
    out = v;
    return true;
}


std::vector<Token> lex(const std::string& src, bool keepComments, std::map<std::string, std::vector<Token>>* defines)
{
    std::vector<Token> out;
    size_t i = 0;
    const size_t n = src.size();
    bool atLineStart = true;
    bool sawNewline = true;

    while (i < n)
    {
        const char c = src[i];

        if (c == '\n') { atLineStart = true; sawNewline = true; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        // preprocessor line
        if (c == '#' && atLineStart)
        {
            std::string line;
            ++i;
            while (i < n && src[i] != '\n')
            {
                if (src[i] == '\\' && i + 1 < n && (src[i + 1] == '\n' || src[i + 1] == '\r'))
                {
                    while (i < n && src[i] != '\n') ++i;
                    ++i;
                    line += ' ';
                    continue;
                }
                line += src[i++];
            }
            if (defines != nullptr)
            {
                // strip comments from the directive
                std::vector<Token> toks = lex(line, false, nullptr);
                if (toks.size() >= 2 && toks[0].kind == Tok::Ident && toks[0].text == "define"
                    && toks[1].kind == Tok::Ident)
                {
                    // function-like macros are not supported
                    const size_t namePos = line.find(toks[1].text);
                    const size_t after = namePos + toks[1].text.size();
                    if (after < line.size() && line[after] == '(')
                        continue;
                    (*defines)[toks[1].text] = std::vector<Token>(toks.begin() + 2, toks.end());
                }
                else if (toks.size() >= 2 && toks[0].kind == Tok::Ident && toks[0].text == "undef"
                         && toks[1].kind == Tok::Ident)
                {
                    defines->erase(toks[1].text);
                }
            }
            continue;
        }
        atLineStart = false;

        Token t;
        t.newlineBefore = sawNewline;
        sawNewline = false;

        if (c == '/' && i + 1 < n && src[i + 1] == '/')
        {
            size_t j = i + 2;
            while (j < n && src[j] != '\n') ++j;
            t.kind = Tok::Comment;
            t.text = trim(src.substr(i + 2, j - i - 2));
            i = j;
            if (keepComments) out.push_back(t);
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*')
        {
            size_t j = src.find("*/", i + 2);
            if (j == std::string::npos) j = n;
            t.kind = Tok::Comment;
            t.text = trim(src.substr(i + 2, j - i - 2));
            i = (j == n) ? n : j + 2;
            if (keepComments) out.push_back(t);
            continue;
        }
        if (isIdentStart(c))
        {
            size_t j = i;
            while (j < n && isIdentChar(src[j])) ++j;
            t.kind = Tok::Ident;
            t.text = src.substr(i, j - i);
            i = j;
            out.push_back(t);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(src[j])))) ++j;
            t.text = src.substr(i, j - i);
            t.kind = parseNumber(t.text, t.value) ? Tok::Number : Tok::Other;
            i = j;
            out.push_back(t);
            continue;
        }
        if (c == '\'')
        {
            // character literal
            size_t j = i + 1;
            long v = 0;
            if (j < n && src[j] == '\\' && j + 1 < n)
            {
                const char e = src[j + 1];
                switch (e)
                {
                case 'n': v = '\n'; break;
                case 't': v = '\t'; break;
                case 'r': v = '\r'; break;
                case '0': v = 0; break;
                default:  v = static_cast<unsigned char>(e); break;
                }
                j += 2;
            }
            else if (j < n)
            {
                v = static_cast<unsigned char>(src[j]);
                ++j;
            }
            while (j < n && src[j] != '\'' && src[j] != '\n') ++j;
            if (j < n && src[j] == '\'') ++j;
            t.kind = Tok::Number;
            t.value = v;
            t.text = "'";
            i = j;
            out.push_back(t);
            continue;
        }
        if (c == '"')
        {
            size_t j = i + 1;
            while (j < n && src[j] != '"' && src[j] != '\n')
            {
                if (src[j] == '\\') ++j;
                ++j;
            }
            t.kind = Tok::Other;
            t.text = "\"";
            i = (j < n) ? j + 1 : n;
            out.push_back(t);
            continue;
        }

        // two-char operators we care about
        if (i + 1 < n)
        {
            const std::string two = src.substr(i, 2);
            if (two == "<<" || two == ">>")
            {
                t.kind = Tok::Punct;
                t.text = two;
                i += 2;
                out.push_back(t);
                continue;
            }
        }
        t.kind = Tok::Punct;
        t.text = std::string(1, c);
        ++i;
        out.push_back(t);
    }
    return out;
}

// --------------------------------------------------------------------------
// Constant expression evaluation

const std::map<std::string, long>& builtinSymbols()
{
    static const std::map<std::string, long> symbols = {
        {"PC_ENV_SPEED", 0}, {"PC_NOISE_PARAMS", 1}, {"PC_WAVE", 2},
        {"PC_NOTE_UP", 3}, {"PC_NOTE_DOWN", 4}, {"PC_NOTE_CUT", 5},
        {"PC_NOTE_HOLD", 6}, {"PC_ENV_VOL", 7}, {"PC_PITCH", 8},
        {"PC_TREMOLO_LEVEL", 9}, {"PC_TREMOLO_RATE", 10}, {"PC_SLIDE", 11},
        {"PC_SLIDE_SPEED", 12}, {"PC_LOOP_START", 13}, {"PC_LOOP_END", 14},
        {"PATCH_END", 0xff},
        {"WAVE_SINE", 0}, {"WAVE_SAWTOOTH", 1}, {"WAVE_TRIANGLE", 2},
        {"WAVE_SQUARE_25", 3}, {"WAVE_SQUARE_50", 4}, {"WAVE_SQUARE_75", 5},
        {"WAVE_FUZZY_SINE1", 6}, {"WAVE_FUZZY_SINE2", 7}, {"WAVE_FUZZY_SINE3", 8},
        {"WAVE_FILTERED_SQUARE", 9},
        {"WSIN", 0}, {"WSAW", 1}, {"WTRI", 2}, {"WS25", 3}, {"WS50", 4},
        {"WS75", 5}, {"WFS1", 6}, {"WFS2", 7}, {"WFS3", 8}, {"WFSQ", 9},
        {"NULL", 0}, {"true", 1}, {"false", 0},
    };
    return symbols;
}

bool isTypeName(const std::string& s)
{
    static const std::set<std::string> types = {
        "char", "signed", "unsigned", "int", "short", "long", "u8", "s8", "u16", "s16",
        "int8_t", "uint8_t", "int16_t", "uint16_t", "const", "volatile",
    };
    return types.count(s) != 0;
}

class ExprEval {
public:
    ExprEval(const std::vector<Token>& toks, const std::map<std::string, std::vector<Token>>& defs, int nesting,
             const std::map<std::string, size_t>* arraySizes = nullptr)
        : t(toks), defines(defs), depth(nesting), sizes(arraySizes) {}

    bool evaluate(long& out)
    {
        if (t.empty() || depth > 16)
            return false;
        p = 0;
        ok = true;
        out = parseOr();
        return ok && p == t.size();
    }

private:
    const std::vector<Token>& t;
    const std::map<std::string, std::vector<Token>>& defines;
    int depth;
    const std::map<std::string, size_t>* sizes;
    size_t p = 0;
    bool ok = true;

    bool peek(const char* s) const { return p < t.size() && t[p].kind == Tok::Punct && t[p].text == s; }

    long parseOr()
    {
        long v = parseXor();
        while (ok && peek("|")) { ++p; v |= parseXor(); }
        return v;
    }
    long parseXor()
    {
        long v = parseAnd();
        while (ok && peek("^")) { ++p; v ^= parseAnd(); }
        return v;
    }
    long parseAnd()
    {
        long v = parseShift();
        while (ok && peek("&")) { ++p; v &= parseShift(); }
        return v;
    }
    long parseShift()
    {
        long v = parseAdd();
        for (;;)
        {
            if (peek("<<")) { ++p; v = static_cast<long>(static_cast<unsigned long>(v) << (parseAdd() & 31)); }
            else if (peek(">>")) { ++p; v >>= (parseAdd() & 31); }
            else break;
        }
        return v;
    }
    long parseAdd()
    {
        long v = parseMul();
        for (;;)
        {
            if (peek("+")) { ++p; v += parseMul(); }
            else if (peek("-")) { ++p; v -= parseMul(); }
            else break;
        }
        return v;
    }
    long parseMul()
    {
        long v = parseUnary();
        for (;;)
        {
            if (peek("*")) { ++p; v *= parseUnary(); }
            else if (peek("/") || peek("%"))
            {
                const bool div = peek("/");
                ++p;
                const long d = parseUnary();
                if (d == 0) { ok = false; return 0; }
                v = div ? v / d : v % d;
            }
            else break;
        }
        return v;
    }
    long parseUnary()
    {
        if (peek("-")) { ++p; return -parseUnary(); }
        if (peek("+")) { ++p; return parseUnary(); }
        if (peek("~")) { ++p; return ~parseUnary(); }
        if (peek("!")) { ++p; return !parseUnary(); }
        if (peek("("))
        {
            // cast: ( type-names... )
            size_t q = p + 1;
            bool allTypes = q < t.size() && t[q].kind == Tok::Ident;
            while (q < t.size() && t[q].kind == Tok::Ident)
            {
                if (!isTypeName(t[q].text)) allTypes = false;
                ++q;
            }
            if (allTypes && q < t.size() && t[q].kind == Tok::Punct && t[q].text == ")")
            {
                p = q + 1;
                return parseUnary();
            }
            ++p;
            const long v = parseOr();
            if (!peek(")")) { ok = false; return 0; }
            ++p;
            return v;
        }
        return parsePrimary();
    }
    long parsePrimary()
    {
        if (p >= t.size()) { ok = false; return 0; }
        const Token& k = t[p++];
        if (k.kind == Tok::Number)
            return k.value;
        if (k.kind == Tok::Ident && k.text == "sizeof")
        {
            // sizeof(array) or sizeof array, for arrays of bytes
            const bool paren = peek("(");
            if (paren) ++p;
            long v = 0;
            if (p < t.size() && t[p].kind == Tok::Ident)
            {
                const std::string& n = t[p++].text;
                if (n == "char" || n == "u8" || n == "s8" || n == "int8_t" || n == "uint8_t") v = 1;
                else if (sizes != nullptr && sizes->count(n)) v = static_cast<long>(sizes->at(n));
                else ok = false;
            }
            else ok = false;
            if (paren)
            {
                if (!peek(")")) { ok = false; return 0; }
                ++p;
            }
            return v;
        }
        if (k.kind == Tok::Ident)
        {
            const auto& b = builtinSymbols();
            auto d = defines.find(k.text);
            if (d != defines.end())
            {
                ExprEval sub(d->second, defines, depth + 1, sizes);
                long v = 0;
                if (sub.evaluate(v)) return v;
            }
            auto it = b.find(k.text);
            if (it != b.end())
                return it->second;
        }
        ok = false;
        return 0;
    }
};

// --------------------------------------------------------------------------
// Declarations

struct ByteArray {
    std::string name;
    std::string comment;
    std::vector<uint8_t> bytes;
    bool valid = false;   // looks like a well-formed patch command stream
    bool hasNoise = false;
};

struct StructEntry {
    std::vector<std::vector<Token>> fields; // type, pcmData, cmdStream, loopStart, loopEnd
};

// split tokens [begin,end) at top-level commas
std::vector<std::vector<Token>> splitTopLevel(const std::vector<Token>& toks, size_t begin, size_t end)
{
    std::vector<std::vector<Token>> parts(1);
    int depth = 0;
    for (size_t i = begin; i < end; ++i)
    {
        const Token& k = toks[i];
        if (k.kind == Tok::Punct)
        {
            if (k.text == "{" || k.text == "(" || k.text == "[") ++depth;
            else if (k.text == "}" || k.text == ")" || k.text == "]") --depth;
            else if (k.text == "," && depth == 0) { parts.emplace_back(); continue; }
        }
        parts.back().push_back(k);
    }
    if (!parts.empty() && parts.back().empty())
        parts.pop_back(); // trailing comma
    return parts;
}

// find matching closing brace for toks[open] == "{"
size_t matchBrace(const std::vector<Token>& toks, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < toks.size(); ++i)
    {
        if (toks[i].kind != Tok::Punct) continue;
        if (toks[i].text == "{") ++depth;
        else if (toks[i].text == "}" && --depth == 0) return i;
    }
    return std::string::npos;
}

void analyseStream(ByteArray& a)
{
    const auto& b = a.bytes;
    size_t i = 0;
    a.valid = false;
    while (i + 1 < b.size())
    {
        const uint8_t cmd = b[i + 1];
        if (cmd == PATCH_END) { a.valid = true; break; }
        if (cmd > PC_LOOP_END) break;
        if (cmd == PC_NOISE_PARAMS) a.hasNoise = true;
        i += 3;
    }
}

std::string firstLine(const std::string& s)
{
    std::string r;
    for (char c : s)
    {
        if (c == '\n' || c == '\r') break;
        r += c;
    }
    r = trim(r);
    // drop leading '*' from block comment continuation
    while (!r.empty() && r[0] == '*') r = trim(r.substr(1));
    if (r.size() > 48) r = r.substr(0, 45) + "...";
    return r;
}

struct Scan {
    std::map<std::string, std::vector<Token>> defines;
    std::vector<ByteArray> arrays;
    std::map<std::string, size_t> arrayIndex;
    std::vector<std::vector<StructEntry>> structs;   // one entry list per PatchStruct array
};

Scan scanDeclarations(const std::string& text)
{
    Scan scan;
    std::map<std::string, std::vector<Token>>& defines = scan.defines;
    const std::vector<Token> all = lex(text, true, &defines);

    // Separate comments, remembering the comment that directly precedes each token
    std::vector<Token> toks;
    std::vector<std::string> commentBefore;
    std::string pending;
    for (const Token& k : all)
    {
        if (k.kind == Tok::Comment)
        {
            pending = k.text;
            continue;
        }
        toks.push_back(k);
        commentBefore.push_back(pending);
        pending.clear();
    }

    size_t stmtStart = 0;
    for (size_t i = 0; i < toks.size(); ++i)
    {
        const Token& k = toks[i];
        if (k.kind == Tok::Punct && k.text == ";") { stmtStart = i + 1; continue; }
        if (k.kind == Tok::Punct && k.text == "{")
        {
            // stray block (function body, etc.) -> skip it
            const size_t close = matchBrace(toks, i);
            if (close == std::string::npos) break;
            i = close;
            stmtStart = i + 1;
            continue;
        }
        if (!(k.kind == Tok::Punct && k.text == "=")) continue;
        if (i + 1 >= toks.size() || !(toks[i + 1].kind == Tok::Punct && toks[i + 1].text == "{"))
            continue;

        const size_t open = i + 1;
        const size_t close = matchBrace(toks, open);
        if (close == std::string::npos) break;

        // inspect the declaration [stmtStart, i)
        bool isStruct = false, isBytes = false, isPointer = false;
        std::string name;
        for (size_t j = stmtStart; j < i; ++j)
        {
            const Token& d = toks[j];
            if (d.kind == Tok::Punct && d.text == "*") isPointer = true;
            if (d.kind == Tok::Punct && d.text == "[" && name.empty() && j > stmtStart
                && toks[j - 1].kind == Tok::Ident)
                name = toks[j - 1].text;
            if (d.kind != Tok::Ident) continue;
            if (d.text == "PatchStruct" || d.text == "Patch") isStruct = true;
            if (d.text == "char" || d.text == "u8" || d.text == "s8" || d.text == "int8_t"
                || d.text == "uint8_t")
                isBytes = true;
        }

        if (!name.empty() && !isPointer)
        {
            const std::vector<std::vector<Token>> items = splitTopLevel(toks, open + 1, close);

            if (isStruct)
            {
                std::vector<StructEntry> entries;
                for (const auto& item : items)
                {
                    StructEntry e;
                    if (item.size() >= 2 && item.front().text == "{")
                    {
                        const std::vector<Token> inner(item.begin() + 1, item.end() - 1);
                        e.fields = splitTopLevel(inner, 0, inner.size());
                    }
                    entries.push_back(e); // keep position even if malformed: indices matter
                }
                scan.structs.push_back(entries);
            }
            else if (isBytes)
            {
                ByteArray a;
                a.name = name;
                a.comment = firstLine(commentBefore[stmtStart < commentBefore.size() ? stmtStart : 0]);
                bool good = true;
                for (const auto& item : items)
                {
                    long v = 0;
                    ExprEval ev(item, defines, 0);
                    if (!ev.evaluate(v)) { good = false; break; }
                    a.bytes.push_back(static_cast<uint8_t>(v & 0xff));
                }
                if (good)
                {
                    analyseStream(a);
                    scan.arrayIndex[a.name] = scan.arrays.size();
                    scan.arrays.push_back(a);
                }
            }
        }

        i = close;
        stmtStart = i + 1;
    }
    return scan;
}

} // namespace

int PatchBank::indexOf(const std::string& name) const
{
    for (size_t i = 0; i < patches.size(); ++i)
        if (patches[i].name == name)
            return static_cast<int>(i);
    return -1;
}

const PatchData* PatchBank::tableEntry(int number) const
{
    if (number < 0 || number >= static_cast<int>(table.size()))
        return nullptr;
    const int i = table[static_cast<size_t>(number)];
    return i >= 0 ? &patches[static_cast<size_t>(i)].data : nullptr;
}

PatchBank parsePatchFile(const std::string& text)
{
    PatchBank bank;
    const Scan scan = scanDeclarations(text);

    std::map<std::string, size_t> sizes;
    for (const ByteArray& a : scan.arrays)
        sizes[a.name] = a.bytes.size();

    auto evaluate = [&](const std::vector<Token>& f, long fallback) -> long {
        long v = 0;
        ExprEval ev(f, scan.defines, 0, &sizes);
        return ev.evaluate(v) ? v : fallback;
    };
    // the array named by a pointer field: name, &name[0], (const char*)name, NULL
    auto identOf = [](const std::vector<Token>& f) -> std::string {
        for (const Token& k : f)
            if (k.kind == Tok::Ident && k.text != "NULL" && !isTypeName(k.text) && k.text != "PROGMEM")
                return k.text;
        return std::string();
    };
    auto findArray = [&](const std::string& name) -> const ByteArray* {
        auto it = scan.arrayIndex.find(name);
        return it == scan.arrayIndex.end() ? nullptr : &scan.arrays[it->second];
    };

    std::set<std::string> referencedSamples, usedArrays, missing;
    std::map<std::string, int> listIndex; // dedup key -> index in bank.patches
    auto nameTaken = [&](const std::string& n) { return bank.indexOf(n) >= 0; };

    for (const auto& entries : scan.structs)
        for (const StructEntry& e : entries)
            if (e.fields.size() >= 2)
            {
                const std::string id = identOf(e.fields[1]);
                if (!id.empty()) referencedSamples.insert(id);
            }

    for (size_t s = 0; s < scan.structs.size(); ++s)
    {
        for (const StructEntry& e : scan.structs[s])
        {
            int idx = -1;
            if (e.fields.size() >= 3)
            {
                const long type = evaluate(e.fields[0], 0);
                const std::string pcmId = identOf(e.fields[1]);
                const std::string cmdId = identOf(e.fields[2]);
                const ByteArray* cmd = cmdId.empty() ? nullptr : findArray(cmdId);
                if (cmd != nullptr)
                    usedArrays.insert(cmdId);

                // Type 2 is PCM. Games built with MIXER_CHAN4_TYPE=1 (PCM instead of noise)
                // also use type 1 with sample data, e.g. Atomix.
                const bool isPcm = type == 2 || (type == 1 && !pcmId.empty());

                if (isPcm)
                {
                    const long loopStart = e.fields.size() > 3 ? evaluate(e.fields[3], 0) : 0;
                    const long loopEnd   = e.fields.size() > 4 ? evaluate(e.fields[4], 0) : 0;
                    const std::string key = "pcm:" + pcmId + "|" + cmdId + "|" + std::to_string(loopStart)
                                          + "|" + std::to_string(loopEnd);
                    auto it = listIndex.find(key);
                    if (it != listIndex.end())
                    {
                        idx = it->second;
                    }
                    else
                    {
                        const ByteArray* sample = pcmId.empty() ? nullptr : findArray(pcmId);
                        Patch p;
                        p.name = !pcmId.empty() ? pcmId : (!cmdId.empty() ? cmdId : "pcm_patch");
                        if (nameTaken(p.name) && !cmdId.empty()) p.name += "/" + cmdId;
                        for (int n = 2; nameTaken(p.name); ++n)
                            p.name = (pcmId.empty() ? cmdId : pcmId) + "#" + std::to_string(n);
                        p.comment = (cmd != nullptr && !cmd->comment.empty()) ? cmd->comment
                                  : (sample != nullptr ? sample->comment : std::string());
                        p.sampleName = pcmId;
                        p.data.type = PatchType::Pcm;
                        if (cmd != nullptr) p.data.stream = cmd->bytes;
                        p.data.loopStart = static_cast<int32_t>(loopStart);
                        p.data.loopEnd = static_cast<int32_t>(loopEnd);
                        if (sample != nullptr)
                        {
                            p.data.sample = sample->bytes;
                            usedArrays.insert(pcmId);
                        }
                        else
                        {
                            p.sampleMissing = true;
                            if (!pcmId.empty()) missing.insert(pcmId);
                        }
                        bank.patches.push_back(p);
                        idx = static_cast<int>(bank.patches.size()) - 1;
                        listIndex[key] = idx;
                    }
                }
                else if (cmd != nullptr)
                {
                    auto it = listIndex.find(cmdId);
                    if (it != listIndex.end())
                    {
                        idx = it->second;
                    }
                    else
                    {
                        Patch p;
                        p.name = cmd->name;
                        p.comment = cmd->comment;
                        p.data.type = type == 1 ? PatchType::Noise : PatchType::Wave;
                        p.data.stream = cmd->bytes;
                        bank.patches.push_back(p);
                        idx = static_cast<int>(bank.patches.size()) - 1;
                        listIndex[cmdId] = idx;
                    }
                }
            }
            // The first PatchStruct array is the kernel's patch table
            // (the one passed to InitMusicPlayer in nearly every game).
            if (s == 0)
                bank.table.push_back(idx);
        }
    }

    // Other well-formed command streams that no table references
    for (const ByteArray& a : scan.arrays)
    {
        if (usedArrays.count(a.name) || referencedSamples.count(a.name) || !a.valid || nameTaken(a.name))
            continue;
        Patch p;
        p.name = a.name;
        p.comment = a.comment;
        p.data.type = a.hasNoise ? PatchType::Noise : PatchType::Wave;
        p.data.stream = a.bytes;
        bank.patches.push_back(p);
    }

    // No PatchStruct table: patch numbers follow file order.
    if (scan.structs.empty())
        for (size_t i = 0; i < bank.patches.size(); ++i)
            bank.table.push_back(static_cast<int>(i));

    bank.missingSamples.assign(missing.begin(), missing.end());
    if (bank.patches.empty())
        bank.error = "No Uzebox patches found in file";
    return bank;
}

std::vector<std::vector<uint8_t>> parseCByteArrays(const std::string& text)
{
    std::vector<std::vector<uint8_t>> out;
    for (const ByteArray& a : scanDeclarations(text).arrays)
        out.push_back(a.bytes);
    return out;
}

namespace {

// true if 'text' declares an array called 'name' ("name[" with optional spaces)
bool declaresArray(const std::string& text, const std::string& name)
{
    size_t pos = 0;
    while ((pos = text.find(name, pos)) != std::string::npos)
    {
        const bool startOk = pos == 0 || !isIdentChar(text[pos - 1]);
        size_t q = pos + name.size();
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
        if (startOk && q < text.size() && text[q] == '[')
            return true;
        pos += name.size();
    }
    return false;
}

} // namespace

std::vector<std::string> findSampleFiles(const std::string& patchPath, const std::vector<std::string>& missing)
{
    namespace fs = std::filesystem;
    std::vector<std::string> found;
    if (missing.empty())
        return found;

    std::error_code ec;
    fs::path self = fs::absolute(fs::path(patchPath), ec);
    if (ec) self = fs::path(patchPath);
    const fs::path dir = self.parent_path();
    std::vector<fs::path> dirs = { dir, dir / "data", dir.parent_path(), dir.parent_path() / "data" };

    std::set<std::string> want(missing.begin(), missing.end());
    std::set<std::string> seen;
    for (const fs::path& d : dirs)
    {
        if (want.empty()) break;
        if (d.empty() || !fs::is_directory(d, ec)) continue;

        std::vector<fs::path> files;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::path f = it->path();
            std::string ext = f.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if ((ext == ".inc" || ext == ".h") && it->is_regular_file(ec) && it->file_size(ec) < 4u * 1024u * 1024u)
                files.push_back(f);
            if (files.size() > 200) break;
        }
        std::sort(files.begin(), files.end());

        for (const fs::path& f : files)
        {
            if (want.empty()) break;
            const std::string canon = fs::weakly_canonical(f, ec).string();
            if (seen.count(canon) || fs::equivalent(f, self, ec)) continue;
            seen.insert(canon);

            std::string text;
            if (!readTextFile(f.string(), text)) continue;
            bool useful = false;
            for (auto it = want.begin(); it != want.end();)
            {
                if (declaresArray(text, *it)) { useful = true; it = want.erase(it); }
                else ++it;
            }
            if (useful)
                found.push_back(f.string());
        }
    }
    return found;
}

bool readTextFile(const std::string& path, std::string& out, size_t maxBytes)
{
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    char buf[8192];
    size_t got;
    bool ok = true;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
    {
        out.append(buf, got);
        if (out.size() > maxBytes) { ok = false; break; }
    }
    std::fclose(f);
    return ok;
}

} // namespace uze
