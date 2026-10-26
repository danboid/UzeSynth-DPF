/*
 * UzeSynth - Uzebox sound engine plugin (UI side)
 *
 * Deliberately small: a folder icon to load a .inc patch file, a wave icon to
 * load a custom wavetable, a mode switch and a drop-down list. Drawn with the
 * NanoVG renderer bundled with DPF, so there are no extra dependencies.
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "DistrhoUI.hpp"

#include "../engine/PatchParser.hpp"
#include "../engine/Wavetable.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

START_NAMESPACE_DISTRHO

namespace {

constexpr float kW = DISTRHO_UI_DEFAULT_WIDTH;
constexpr float kH = DISTRHO_UI_DEFAULT_HEIGHT;
constexpr float kPad = 16.0f;
constexpr float kRowH = 22.0f;

struct Box {
    float x, y, w, h;
    bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

const Box kLoadBox   { kPad, 54.0f, 44.0f, 40.0f };
const Box kWavesBox  { kPad, 102.0f, 44.0f, 40.0f };
const Box kResetBox  { kW - kPad - 64.0f, 111.0f, 64.0f, 22.0f };
const Box kModeBox   { kPad, 156.0f, kW - 2 * kPad, 28.0f };
const Box kComboBox  { kPad, 198.0f, kW - 2 * kPad, 32.0f };
const Box kListBox   { kPad, 44.0f, kW - 2 * kPad, kH - 44.0f - 8.0f };

const char* const kModeNames[3] = { "Melodic", "Drum kit", "Sound FX" };

enum class Browse { None, Patches, Waves };

std::string baseName(const std::string& path)
{
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string dirName(const std::string& path)
{
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

std::string noteName(int n)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s%d", names[n % 12], n / 12 - 1);
    return buf;
}

} // namespace

class UzeSynthUI : public UI
{
public:
    UzeSynthUI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT, true)
    {
        loadSharedResources();
    }

protected:
    // ------------------------------------------------------------------------
    // DSP -> UI

    void parameterChanged(uint32_t index, float value) override
    {
        if (index == kParamPlayMode)
        {
            int m = static_cast<int>(value + 0.5f);
            fMode = m < 0 ? 0 : (m > 2 ? 2 : m);
            fListOpen = false;
            repaint();
        }
    }

    void stateChanged(const char* key, const char* value) override
    {
        if (value == nullptr)
            value = "";

        if (std::strcmp(key, UZESYNTH_STATE_FILE_TEXT) == 0)
        {
            fBank = uze::parsePatchFile(value);
            fStatus = fBank.error;
        }
        else if (std::strcmp(key, UZESYNTH_STATE_FILE_NAME) == 0)
        {
            fFilePath = value;
        }
        else if (std::strcmp(key, UZESYNTH_STATE_PATCH_NAME) == 0)
        {
            fPatchName = value;
        }
        else if (std::strcmp(key, UZESYNTH_STATE_WAVES) == 0)
        {
            const size_t bytes = std::strlen(value) / 2;
            fWaveCount = bytes >= 256 ? static_cast<int>(bytes / 256) : 0;
        }
        else if (std::strcmp(key, UZESYNTH_STATE_WAVES_NAME) == 0)
        {
            fWavesPath = value;
        }
        repaint();
    }

    // ------------------------------------------------------------------------
    // File browser

    void uiFileBrowserSelected(const char* filename) override
    {
        const Browse what = fBrowse;
        fBrowse = Browse::None;
        if (filename == nullptr)
            return; // cancelled

        std::string contents;
        if (!uze::readTextFile(filename, contents))
        {
            fStatus = "Could not read file (missing or larger than 4 MB)";
            repaint();
            return;
        }

        if (what == Browse::Waves)
            loadWavetable(filename, contents);
        else
            loadPatches(filename, contents);
        repaint();
    }

    // ------------------------------------------------------------------------
    // Drawing

    void onNanoDisplay() override
    {
        beginPath();
        rect(0, 0, kW, kH);
        fillColor(Color(24, 26, 32));
        fill();

        // header
        beginPath();
        rect(0, 0, kW, 40);
        fillColor(Color(36, 40, 52));
        fill();
        fontSize(20.0f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(Color(255, 200, 60));
        text(kPad, 20, "UzeSynth", nullptr);
        fontSize(12.0f);
        fillColor(Color(150, 156, 170));
        textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
        text(kW - kPad, 20, "Uzebox sound engine", nullptr);

        // patch file row
        drawIconButton(kLoadBox, fHover == Hover::Load, false);
        drawRowLabel(kLoadBox,
                     fFilePath.empty() ? "Click the folder to load a .inc patch file" : baseName(fFilePath),
                     fFilePath.empty());

        // wavetable row
        drawIconButton(kWavesBox, fHover == Hover::Waves, true);
        {
            std::string label;
            if (fWaveCount > 0)
                label = (fWavesPath.empty() ? std::string("Custom wavetable") : baseName(fWavesPath))
                        + "  (" + std::to_string(fWaveCount) + " waves)";
            else
                label = "Kernel default waves (10)";
            drawRowLabel(kWavesBox, label, fWaveCount == 0, fWaveCount > 0 ? kResetBox.w + 8 : 0);
        }
        if (fWaveCount > 0)
        {
            beginPath();
            roundedRect(kResetBox.x, kResetBox.y, kResetBox.w, kResetBox.h, 4);
            fillColor(fHover == Hover::Reset ? Color(70, 76, 96) : Color(44, 48, 62));
            fill();
            fontSize(12.0f);
            textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
            fillColor(Color(200, 204, 214));
            text(kResetBox.x + kResetBox.w / 2, kResetBox.y + kResetBox.h / 2, "Default", nullptr);
        }

        drawModeSwitch();
        drawCombo();

        // status / info
        fontSize(12.0f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        if (!fStatus.empty())
        {
            fillColor(Color(255, 110, 100));
            text(kPad, 248, fStatus.c_str(), nullptr);
        }
        else if (fMode == 0)
        {
            if (const uze::Patch* p = currentPatch())
            {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s channel  -  %u bytes  -  %u patches in file",
                              p->data.type == uze::PatchType::Noise ? "Noise" : "Wave",
                              static_cast<unsigned>(p->data.stream.size()),
                              static_cast<unsigned>(fBank.patches.size()));
                fillColor(Color(150, 156, 170));
                text(kPad, 248, buf, nullptr);
            }
        }
        else
        {
            fillColor(Color(150, 156, 170));
            text(kPad, 248, fMode == 1 ? "MIDI note n plays patch n on the noise channel (song channel 4)"
                                       : "MIDI note n triggers patch n as a sound effect (TriggerFx)", nullptr);
        }

        fillColor(Color(100, 106, 120));
        fontSize(11.0f);
        text(kPad, kH - 16, "MIDI CC 7 volume  -  11 expression  -  92 tremolo level  -  100 tremolo rate", nullptr);

        if (fListOpen)
            drawList();
    }

    // ------------------------------------------------------------------------
    // Events

    bool onMouse(const MouseEvent& ev) override
    {
        if (ev.button != 1 || !ev.press)
            return false;

        const double x = ev.pos.getX();
        const double y = ev.pos.getY();

        if (fListOpen)
        {
            if (kListBox.contains(x, y) && fMode == 0)
            {
                const int row = rowAt(y);
                if (row >= 0 && row < static_cast<int>(fBank.patches.size()))
                    selectPatch(row);
            }
            fListOpen = false;
            repaint();
            return true;
        }

        if (kLoadBox.contains(x, y))
        {
            openBrowser(Browse::Patches, "Load Uzebox patch file (.inc)", fFilePath);
            return true;
        }
        if (fWaveCount > 0 && kResetBox.contains(x, y))
        {
            fWaveCount = 0;
            fWavesPath.clear();
            fStatus.clear();
            setState(UZESYNTH_STATE_WAVES, "");
            setState(UZESYNTH_STATE_WAVES_NAME, "");
            repaint();
            return true;
        }
        if (kWavesBox.contains(x, y))
        {
            openBrowser(Browse::Waves, "Load Uzebox wavetable (MIXER_WAVES .inc, C array or .raw)",
                        fWavesPath.empty() ? fFilePath : fWavesPath);
            return true;
        }
        if (kModeBox.contains(x, y))
        {
            const int m = static_cast<int>((x - kModeBox.x) / (kModeBox.w / 3));
            if (m >= 0 && m <= 2 && m != fMode)
            {
                fMode = m;
                editParameter(kParamPlayMode, true);
                setParameterValue(kParamPlayMode, static_cast<float>(m));
                editParameter(kParamPlayMode, false);
                repaint();
            }
            return true;
        }
        if (kComboBox.contains(x, y) && listCount() > 0)
        {
            fListOpen = true;
            int cur = 0;
            if (fMode == 0)
                cur = fBank.indexOf(fPatchName);
            fScroll = clampScroll(cur - visibleRows() / 2);
            repaint();
            return true;
        }
        return false;
    }

    bool onMotion(const MotionEvent& ev) override
    {
        const double x = ev.pos.getX(), y = ev.pos.getY();
        Hover h = Hover::None;
        int row = -1;
        if (fListOpen)
        {
            if (kListBox.contains(x, y))
                row = rowAt(y);
        }
        else if (kLoadBox.contains(x, y))  h = Hover::Load;
        else if (kWavesBox.contains(x, y)) h = Hover::Waves;
        else if (fWaveCount > 0 && kResetBox.contains(x, y)) h = Hover::Reset;

        if (h != fHover || row != fHoverRow)
        {
            fHover = h;
            fHoverRow = row;
            repaint();
        }
        return false;
    }

    bool onScroll(const ScrollEvent& ev) override
    {
        const int dir = ev.delta.getY() > 0 ? -1 : (ev.delta.getY() < 0 ? 1 : 0);
        if (dir == 0)
            return false;

        if (fListOpen)
        {
            fScroll = clampScroll(fScroll + dir * 2);
            repaint();
            return true;
        }
        // scrolling over the closed combo steps through patches (melodic mode)
        if (fMode == 0 && kComboBox.contains(ev.pos.getX(), ev.pos.getY()) && !fBank.patches.empty())
        {
            int i = fBank.indexOf(fPatchName);
            i = i < 0 ? 0 : i + dir;
            if (i >= 0 && i < static_cast<int>(fBank.patches.size()))
                selectPatch(i);
            return true;
        }
        return false;
    }

private:
    enum class Hover { None, Load, Waves, Reset };

    uze::PatchBank fBank;
    std::string fFilePath, fPatchName, fWavesPath, fStatus;
    int fWaveCount = 0; // 0 = kernel default waves
    int fMode = 0;
    Browse fBrowse = Browse::None;
    bool fListOpen = false;
    Hover fHover = Hover::None;
    int fHoverRow = -1;
    int fScroll = 0;

    // ---- loading -------------------------------------------------------------

    void openBrowser(Browse what, const char* title, const std::string& near)
    {
        fBrowse = what;
        const std::string dir = dirName(near);
        FileBrowserOptions opts;
        opts.title = title;
        if (!dir.empty())
            opts.startDir = dir.c_str();
        openFileBrowser(opts);
    }

    void loadPatches(const std::string& path, const std::string& text)
    {
        uze::PatchBank bank = uze::parsePatchFile(text);
        if (bank.patches.empty())
        {
            fStatus = "No Uzebox patches found in " + baseName(path);
            return;
        }
        fBank = bank;
        fFilePath = path;
        fStatus.clear();
        // keep the same patch name selected if the new file has it
        if (fBank.indexOf(fPatchName) < 0)
            fPatchName = fBank.patches.front().name;

        setState(UZESYNTH_STATE_FILE_TEXT, text.c_str());
        setState(UZESYNTH_STATE_FILE_NAME, fFilePath.c_str());
        setState(UZESYNTH_STATE_PATCH_NAME, fPatchName.c_str());
    }

    void loadWavetable(const std::string& path, const std::string& contents)
    {
        uze::Wavetable table;
        std::string error, warning;
        if (!uze::parseWavetable(contents, path, table, error, warning))
        {
            fStatus = baseName(path) + ": " + error;
            return;
        }
        fWaveCount = table.count();
        fWavesPath = path;
        fStatus = warning; // shown in red, but the table is loaded
        setState(UZESYNTH_STATE_WAVES, uze::toHex(table.data).c_str());
        setState(UZESYNTH_STATE_WAVES_NAME, fWavesPath.c_str());
    }

    // ---- list helpers --------------------------------------------------------

    const uze::Patch* currentPatch() const
    {
        const int i = fBank.indexOf(fPatchName);
        return i >= 0 ? &fBank.patches[static_cast<size_t>(i)] : nullptr;
    }

    // Melodic: every patch. Drum/FX: the patch table, which MIDI notes index.
    int listCount() const
    {
        const size_t n = fMode == 0 ? fBank.patches.size() : std::min<size_t>(fBank.table.size(), 128);
        return static_cast<int>(n);
    }

    int rowAt(double y) const { return static_cast<int>((y - kListBox.y - 4) / kRowH) + fScroll; }
    int visibleRows() const { return static_cast<int>((kListBox.h - 8) / kRowH); }

    int clampScroll(int s) const
    {
        const int maxScroll = listCount() - visibleRows();
        if (s > maxScroll) s = maxScroll;
        if (s < 0) s = 0;
        return s;
    }

    void selectPatch(int index)
    {
        fPatchName = fBank.patches[static_cast<size_t>(index)].name;
        setState(UZESYNTH_STATE_PATCH_NAME, fPatchName.c_str());
        repaint();
    }

    static std::string patchLabel(const uze::Patch& p)
    {
        std::string s = p.name;
        if (!p.comment.empty())
            s += "  -  " + p.comment;
        return s;
    }

    std::string rowLabel(int row) const
    {
        if (fMode == 0)
            return patchLabel(fBank.patches[static_cast<size_t>(row)]);
        const int i = fBank.table[static_cast<size_t>(row)];
        if (i < 0)
            return "(PCM - not supported)";
        return patchLabel(fBank.patches[static_cast<size_t>(i)]);
    }

    bool rowIsNoise(int row) const
    {
        if (fMode == 1)
            return true; // drum kit always uses the noise channel
        const int i = fMode == 0 ? row : fBank.table[static_cast<size_t>(row)];
        return i >= 0 && fBank.patches[static_cast<size_t>(i)].data.type == uze::PatchType::Noise;
    }

    // ---- drawing helpers -----------------------------------------------------

    void drawRowLabel(const Box& b, const std::string& label, bool dim, float rightReserve = 0)
    {
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fontSize(14.0f);
        fillColor(dim ? Color(150, 156, 170) : Color(220, 224, 232));
        const float tx = b.x + b.w + 12;
        scissor(tx, b.y, kW - tx - kPad - rightReserve, b.h);
        text(tx, b.y + b.h / 2, label.c_str(), nullptr);
        resetScissor();
    }

    void drawIconButton(const Box& b, bool hover, bool waveIcon)
    {
        beginPath();
        roundedRect(b.x, b.y, b.w, b.h, 6);
        fillColor(hover ? Color(60, 66, 84) : Color(44, 48, 62));
        fill();

        const float x = b.x + 10, y = b.y + 11, w = b.w - 20, h = b.h - 22;
        if (waveIcon)
        {
            // one cycle of a stepped (8-bit looking) sine
            beginPath();
            const int steps = 12;
            for (int i = 0; i <= steps; ++i)
            {
                const float px = x + w * i / steps;
                const float py = y + h / 2 - std::sin(6.2831853f * i / steps) * h / 2;
                if (i == 0) moveTo(px, py);
                else { lineTo(px, std::round(py)); }
            }
            strokeColor(Color(140, 200, 255));
            strokeWidth(2.0f);
            stroke();
            return;
        }

        // folder
        beginPath();
        moveTo(x, y + 2);
        lineTo(x + w * 0.38f, y + 2);
        lineTo(x + w * 0.48f, y + 6);
        lineTo(x + w, y + 6);
        lineTo(x + w, y + h);
        lineTo(x, y + h);
        closePath();
        fillColor(Color(255, 200, 60));
        fill();
        beginPath();
        rect(x, y + 9, w, h - 9);
        fillColor(Color(255, 220, 110));
        fill();
    }

    void drawModeSwitch()
    {
        const Box& b = kModeBox;
        const float sw = b.w / 3;
        fontSize(13.0f);
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        for (int i = 0; i < 3; ++i)
        {
            beginPath();
            roundedRect(b.x + i * sw + 1, b.y, sw - 2, b.h, 5);
            fillColor(i == fMode ? Color(255, 200, 60) : Color(44, 48, 62));
            fill();
            fillColor(i == fMode ? Color(24, 26, 32) : Color(200, 204, 214));
            text(b.x + i * sw + sw / 2, b.y + b.h / 2, kModeNames[i], nullptr);
        }
    }

    void drawCombo()
    {
        const Box& b = kComboBox;
        beginPath();
        roundedRect(b.x, b.y, b.w, b.h, 5);
        fillColor(Color(44, 48, 62));
        fill();
        strokeColor(fListOpen ? Color(255, 200, 60) : Color(70, 76, 96));
        strokeWidth(1.0f);
        stroke();

        const float ax = b.x + b.w - 20, ay = b.y + b.h / 2;
        beginPath();
        moveTo(ax - 6, ay - 3);
        lineTo(ax + 6, ay - 3);
        lineTo(ax, ay + 4);
        closePath();
        fillColor(Color(200, 204, 214));
        fill();

        fontSize(14.0f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        scissor(b.x + 8, b.y, b.w - 40, b.h);
        std::string label;
        bool dim = false;
        if (fBank.patches.empty())
        {
            label = "No patches loaded";
            dim = true;
        }
        else if (fMode != 0)
        {
            label = "Patch table: notes 0-" + std::to_string(listCount() - 1) + "  (click to view)";
        }
        else if (const uze::Patch* p = currentPatch())
        {
            label = patchLabel(*p);
        }
        else
        {
            label = "Choose a patch";
            dim = true;
        }
        fillColor(dim ? Color(120, 126, 140) : Color(235, 238, 245));
        text(b.x + 10, b.y + b.h / 2, label.c_str(), nullptr);
        resetScissor();
    }

    void drawList()
    {
        const Box& b = kListBox;
        beginPath();
        roundedRect(b.x, b.y, b.w, b.h, 6);
        fillColor(Color(32, 35, 45));
        fill();
        strokeColor(Color(255, 200, 60));
        strokeWidth(1.0f);
        stroke();

        const int rows = visibleRows();
        const int count = listCount();
        const int current = fMode == 0 ? fBank.indexOf(fPatchName) : -1;

        fontSize(13.0f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        scissor(b.x + 2, b.y + 2, b.w - 14, b.h - 4);
        for (int r = 0; r < rows && r + fScroll < count; ++r)
        {
            const int i = r + fScroll;
            const float ry = b.y + 4 + r * kRowH;
            if (i == current || (i == fHoverRow && fMode == 0))
            {
                beginPath();
                rect(b.x + 3, ry, b.w - 14, kRowH);
                fillColor(i == current ? Color(255, 200, 60, 0.25f) : Color(255, 255, 255, 0.08f));
                fill();
            }
            fillColor(rowIsNoise(i) ? Color(140, 200, 255) : Color(230, 233, 240));
            if (fMode == 0)
            {
                text(b.x + 10, ry + kRowH / 2, rowLabel(i).c_str(), nullptr);
            }
            else
            {
                // patch number = MIDI note, in fixed columns
                const std::string num = std::to_string(i);
                textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
                text(b.x + 32, ry + kRowH / 2, num.c_str(), nullptr);
                textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
                fillColor(Color(150, 156, 170));
                text(b.x + 42, ry + kRowH / 2, noteName(i).c_str(), nullptr);
                fillColor(rowIsNoise(i) ? Color(140, 200, 255) : Color(230, 233, 240));
                text(b.x + 84, ry + kRowH / 2, rowLabel(i).c_str(), nullptr);
            }
        }
        resetScissor();

        if (count > rows)
        {
            const float trackH = b.h - 8;
            const float thumbH = std::fmax(16.0f, trackH * rows / count);
            const float thumbY = b.y + 4 + (trackH - thumbH) * fScroll / static_cast<float>(count - rows);
            beginPath();
            roundedRect(b.x + b.w - 8, thumbY, 4, thumbH, 2);
            fillColor(Color(120, 126, 140));
            fill();
        }
    }

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UzeSynthUI)
};

UI* createUI()
{
    return new UzeSynthUI();
}

END_NAMESPACE_DISTRHO
