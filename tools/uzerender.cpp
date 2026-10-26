/*
 * uzerender - list the patches in a Uzebox .inc file, or render one of them
 * to an 8-bit 15734 Hz WAV file: the exact sample stream a Uzebox kernel
 * writes to its PWM audio output. Useful for checking the emulation against
 * real hardware or cuzebox recordings.
 *
 *   uzerender [options] patches.inc                  (list patches)
 *   uzerender [options] patches.inc patch03 out.wav [note] [hold_seconds] [velocity]
 *
 * options:
 *   --waves FILE   custom wavetable (MIXER_WAVES .inc, C array or raw binary)
 *   --drum         drum kit mode: PATCH is a patch number, played on the noise channel
 *   --fx           sound FX mode: PATCH is a patch number, played like TriggerFx()
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "../src/engine/PatchParser.hpp"
#include "../src/engine/UzeboxSynth.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }

static void usage(const char* argv0)
{
    std::fprintf(stderr,
                 "usage: %s [--waves FILE] [--drum|--fx] file.inc [patch out.wav [note=60] [hold_seconds=1] [velocity=127]]\n",
                 argv0);
}

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    std::string wavesFile;
    uze::UzeboxSynth::PlayMode mode = uze::UzeboxSynth::PlayMode::Melodic;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--waves" && i + 1 < argc) wavesFile = argv[++i];
        else if (a == "--drum") mode = uze::UzeboxSynth::PlayMode::DrumKit;
        else if (a == "--fx") mode = uze::UzeboxSynth::PlayMode::SoundFx;
        else if (a.size() > 1 && a[0] == '-' && a[1] == '-') { usage(argv[0]); return 1; }
        else args.push_back(a);
    }
    if (args.empty())
    {
        usage(argv[0]);
        return 1;
    }

    uze::SoundData data;

    std::string text;
    if (!uze::readTextFile(args[0], text))
    {
        std::fprintf(stderr, "cannot read %s\n", args[0].c_str());
        return 1;
    }
    data.bank = uze::parsePatchFile(text);
    if (!data.bank.error.empty())
    {
        std::fprintf(stderr, "%s\n", data.bank.error.c_str());
        return 1;
    }

    if (!wavesFile.empty())
    {
        std::string contents, error, warning;
        if (!uze::readTextFile(wavesFile, contents)
            || !uze::parseWavetable(contents, wavesFile, data.waves, error, warning))
        {
            std::fprintf(stderr, "wavetable %s: %s\n", wavesFile.c_str(), error.empty() ? "cannot read" : error.c_str());
            return 1;
        }
        std::fprintf(stderr, "wavetable: %d waves%s%s\n", data.waves.count(),
                     warning.empty() ? "" : " - ", warning.c_str());
    }

    const uze::PatchBank& bank = data.bank;
    if (args.size() < 3)
    {
        std::printf("num  name                     type  size       comment\n");
        for (size_t n = 0; n < bank.table.size(); ++n)
        {
            const int i = bank.table[n];
            if (i < 0) { std::printf("%3zu  (PCM / unsupported)\n", n); continue; }
            const uze::Patch& p = bank.patches[static_cast<size_t>(i)];
            std::printf("%3zu  %-24s %-5s %3zu bytes  %s\n", n, p.name.c_str(),
                        p.data.type == uze::PatchType::Noise ? "noise" : "wave",
                        p.data.stream.size(), p.comment.c_str());
        }
        for (size_t i = 0; i < bank.patches.size(); ++i)
        {
            bool inTable = false;
            for (int t : bank.table) inTable |= (t == static_cast<int>(i));
            if (!inTable)
                std::printf("  -  %-24s (not in patch table)\n", bank.patches[i].name.c_str());
        }
        return 0;
    }

    // Resolve the patch argument: a name, or a patch table number
    int index = bank.indexOf(args[1]);
    long number = -1;
    {
        char* end = nullptr;
        const long n = std::strtol(args[1].c_str(), &end, 10);
        if (end != args[1].c_str() && *end == '\0') number = n;
    }
    if (index < 0 && number >= 0 && number < static_cast<long>(bank.table.size()))
        index = bank.table[static_cast<size_t>(number)];
    if (number < 0 && index >= 0)
        for (size_t n = 0; n < bank.table.size(); ++n)
            if (bank.table[n] == index) { number = static_cast<long>(n); break; }
    if (index < 0)
    {
        std::fprintf(stderr, "no patch %s\n", args[1].c_str());
        return 1;
    }

    int note = args.size() > 3 ? std::atoi(args[3].c_str()) : 60;
    const double hold = args.size() > 4 ? std::atof(args[4].c_str()) : 1.0;
    const int velocity = args.size() > 5 ? std::atoi(args[5].c_str()) : 127;

    uze::UzeboxSynth synth;
    synth.setSoundData(&data);
    synth.setPlayMode(mode);
    synth.setPatchIndex(index);
    if (mode != uze::UzeboxSynth::PlayMode::Melodic)
    {
        if (number < 0)
        {
            std::fprintf(stderr, "%s is not in the patch table\n", args[1].c_str());
            return 1;
        }
        note = static_cast<int>(number); // the note number selects the patch
    }
    synth.noteOn(static_cast<uint8_t>(note), static_cast<uint8_t>(velocity));

    const int holdSamples = static_cast<int>(hold * uze::kNativeSampleRate);
    const int tailSamples = static_cast<int>(2.0 * uze::kNativeSampleRate);

    std::vector<uint8_t> pcm;
    for (int i = 0; i < holdSamples; ++i)
        pcm.push_back(static_cast<uint8_t>(synth.renderNativeSample() + 128));
    synth.noteOff(static_cast<uint8_t>(note));
    for (int i = 0; i < tailSamples; ++i)
        pcm.push_back(static_cast<uint8_t>(synth.renderNativeSample() + 128));

    std::vector<uint8_t> wav;
    const uint32_t rate = 15734;
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put32(wav, static_cast<uint32_t>(36 + pcm.size() + (pcm.size() & 1)));
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(wav, 16); put16(wav, 1); put16(wav, 1);
    put32(wav, rate); put32(wav, rate); put16(wav, 1); put16(wav, 8);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put32(wav, static_cast<uint32_t>(pcm.size()));
    wav.insert(wav.end(), pcm.begin(), pcm.end());
    if (pcm.size() & 1) wav.push_back(0);

    FILE* f = std::fopen(args[2].c_str(), "wb");
    if (f == nullptr || std::fwrite(wav.data(), 1, wav.size(), f) != wav.size())
    {
        std::fprintf(stderr, "cannot write %s\n", args[2].c_str());
        if (f) std::fclose(f);
        return 1;
    }
    std::fclose(f);
    const uze::Patch& p = bank.patches[static_cast<size_t>(index)];
    std::printf("rendered %s (%s) note %d -> %s\n", p.name.c_str(), p.comment.c_str(), note, args[2].c_str());
    return 0;
}
