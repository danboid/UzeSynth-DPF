/*
 * Round-trip test: parse a patch file, write it back out with
 * generatePatchFile(), parse that, and check nothing changed.
 *
 *   roundtrip file.inc [more.inc...]     (extra files are appended, e.g. samples)
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "../src/engine/PatchEdit.hpp"
#include "../src/engine/PatchParser.hpp"

#include <cstdio>
#include <string>

static bool sameStream(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    // compare as edit streams: the writer normalises missing/empty PATCH_END
    const uze::EditStream x = uze::decodeStream(a), y = uze::decodeStream(b);
    if (x.commands.size() != y.commands.size() || x.endDelta != y.endDelta) return false;
    for (size_t i = 0; i < x.commands.size(); ++i)
        if (x.commands[i].delta != y.commands[i].delta || x.commands[i].cmd != y.commands[i].cmd
            || x.commands[i].param != y.commands[i].param)
            return false;
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s file.inc [more.inc...]\n", argv[0]); return 2; }
    std::string text;
    for (int i = 1; i < argc; ++i)
    {
        std::string t;
        if (!uze::readTextFile(argv[i], t)) { std::fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
        text += t + "\n";
    }
    const uze::PatchBank a = uze::parsePatchFile(text);
    const std::string out = uze::generatePatchFile(a);
    const uze::PatchBank b = uze::parsePatchFile(out);

    int errors = 0;
    auto fail = [&](const std::string& what) { std::printf("  FAIL: %s\n", what.c_str()); ++errors; };

    // every original table entry must map to an equivalent patch
    if (b.table.size() < a.table.size()) fail("table shrank");
    for (size_t n = 0; n < a.table.size() && n < b.table.size(); ++n)
    {
        const int ia = a.table[n], ib = b.table[n];
        if ((ia < 0) != (ib < 0)) { fail("table entry " + std::to_string(n) + " presence"); continue; }
        if (ia < 0) continue;
        const uze::Patch& pa = a.patches[size_t(ia)];
        const uze::Patch& pb = b.patches[size_t(ib)];
        if (pa.name != pb.name) fail("name " + pa.name + " -> " + pb.name);
        if (pa.data.type != pb.data.type) fail("type of " + pa.name);
        if (!sameStream(pa.data.stream, pb.data.stream) && !(pa.data.type == uze::PatchType::Pcm && pa.data.stream.empty()))
            fail("stream of " + pa.name);
        if (pa.data.sample != pb.data.sample) fail("sample of " + pa.name);
        if (pa.data.loopStart != pb.data.loopStart || pa.data.loopEnd != pb.data.loopEnd) fail("loop of " + pa.name);
        if (pa.comment != pb.comment) fail("comment of " + pa.name + ": '" + pa.comment + "' vs '" + pb.comment + "'");
    }
    // every patch must still exist
    for (const uze::Patch& p : a.patches)
        if (b.indexOf(p.name) < 0) fail("lost patch " + p.name);
    // writing again must be stable
    if (uze::generatePatchFile(b) != out) fail("second write differs");

    std::printf("%s: %zu patches, %zu table entries: %s\n", argv[1], a.patches.size(), a.table.size(),
                errors ? "FAILED" : "ok");
    return errors ? 1 : 0;
}
