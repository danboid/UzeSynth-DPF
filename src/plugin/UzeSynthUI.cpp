/*
 * UzeSynth - Uzebox sound engine plugin (UI side)
 *
 * Main page: load a .inc patch file, a custom wavetable, choose the mode and
 * patch, and audition notes. The Edit button opens the patch editor page.
 * Drawn with the NanoVG renderer bundled with DPF: no extra dependencies.
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#include "DistrhoUI.hpp"

#include "Gui.hpp"
#include "PatchEditor.hpp"
#include "../engine/PatchEdit.hpp"
#include "../engine/PatchParser.hpp"
#include "../engine/Wavetable.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

START_NAMESPACE_DISTRHO

namespace {

constexpr float kW = DISTRHO_UI_DEFAULT_WIDTH;
constexpr float kH = DISTRHO_UI_DEFAULT_HEIGHT;
constexpr float kPad = 16.0f;

const Box kLoadBox   { kPad, 54, 44, 40 };
const Box kAddBox    { kW - kPad - 64, 63, 64, 22 };
const Box kWavesBox  { kPad, 102, 44, 40 };
const Box kResetBox  { kW - kPad - 64, 111, 64, 22 };
const Box kModeBox   { kPad, 156, kW - 2 * kPad, 28 };
const Box kComboBox  { kPad, 198, kW - 2 * kPad - 80, 32 };
const Box kEditBox   { kW - kPad - 72, 198, 72, 32 };
const Box kMixerBox  { 120, 266, kW - 120 - kPad, 24 };
const Box kOctDown   { kPad, 306, 26, 22 };
const Box kOctUp     { kPad + 30, 306, 26, 22 };
const Box kKeys      { kPad, 334, kW - 2 * kPad, 86 };

const char* const kModeNames[3] = { "Melodic", "Drum kit", "Sound FX" };
const char* const kMixerNames[2] = { "VSYNC (default)", "Inline" };

enum class Browse { None, Patches, AddFile, Waves, Save };

std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    size_t a = 0;
    while (a < s.size())
    {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

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

} // namespace

class UzeSynthUI : public UI, public PatchEditor::Host
{
public:
    UzeSynthUI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT),
          fEditor(*this, fBank, fPatchName)
    {
        loadSharedResources();
        fList.area = { kPad, 44, kW - 2 * kPad, kH - 52 };
        // Scale for high-DPI displays ourselves, as DPF's examples do. (DPF's
        // automatic scaling applied a host-given scale twice, e.g. CLAP on Windows.)
        applyScaleFactor(getScaleFactor());
    }

protected:
    // ------------------------------------------------------------------------
    // DSP -> UI

    void parameterChanged(uint32_t index, float value) override
    {
        if (index == kParamPlayMode)
        {
            const int m = static_cast<int>(value + 0.5f);
            fMode = m < 0 ? 0 : (m > 2 ? 2 : m);
            fEditor.playMode = fMode;
            fListOpen = false;
        }
        else if (index == kParamMixer)
        {
            fMixer = value >= 0.5f ? 1 : 0;
        }
        repaint();
    }

    void stateChanged(const char* key, const char* value) override
    {
        if (value == nullptr)
            value = "";

        if (std::strcmp(key, UZESYNTH_STATE_FILE_TEXT) == 0)
        {
            // ignore echoes of what we sent, and don't clobber unsent edits
            if (fCommitPending || fFileText == value)
                return;
            fFileText = value;
            fBank = uze::parsePatchFile(fFileText);
            fStatus = fBank.error.empty() ? missingSamplesMessage() : fBank.error;
            fEditor.bankReplaced();
        }
        else if (std::strcmp(key, UZESYNTH_STATE_FILE_NAME) == 0)  fFilePath = value;
        else if (std::strcmp(key, UZESYNTH_STATE_PATCH_NAME) == 0) fPatchName = value;
        else if (std::strcmp(key, UZESYNTH_STATE_MODIFIED) == 0)   fModified = value[0] != '\0';
        else if (std::strcmp(key, UZESYNTH_STATE_WAVES) == 0)
        {
            const size_t bytes = std::strlen(value) / 2;
            fWaveCount = bytes >= 256 ? static_cast<int>(bytes / 256) : 0;
            fEditor.waveCount = fWaveCount;
        }
        else if (std::strcmp(key, UZESYNTH_STATE_WAVES_NAME) == 0) fWavesPath = value;
        repaint();
    }

    // ------------------------------------------------------------------------
    // idle: send edits to the DSP once the user pauses

    void uiIdle() override
    {
        // not while dragging a value; but while holding Play, so tweaks are heard
        if (fCommitPending && ++fIdleTicks >= 6 && (!fMouseDown || fPreviewNote >= 0))
            commit();
    }

    // ------------------------------------------------------------------------
    // File browser

    void uiFileBrowserSelected(const char* filename) override
    {
        const Browse what = fBrowse;
        fBrowse = Browse::None;
        if (filename == nullptr)
            return; // cancelled

        if (what == Browse::Save)
        {
            if (saveTo(filename))
                fEditor.setMessage("Saved " + std::string(filename), false);
            repaint();
            return;
        }

        std::string contents;
        if (!uze::readTextFile(filename, contents))
        {
            fStatus = "Could not read file (missing or larger than 4 MB)";
            repaint();
            return;
        }
        if (what == Browse::Waves)
            loadWavetable(filename, contents);
        else if (what == Browse::AddFile)
            addFile(filename, contents);
        else
            loadPatches(filename, contents);
        repaint();
    }

    // ------------------------------------------------------------------------
    // Drawing

    void onNanoDisplay() override
    {
        // everything is laid out in 520x460 design units
        const float s = static_cast<float>(drawScale());
        if (s != 1.0f)
            scale(s, s);

        if (fEditorOpen)
        {
            fEditor.draw(*this);
            return;
        }

        gui::fillBox(*this, { 0, 0, kW, kH }, gui::colour::bg(), 0);
        gui::fillBox(*this, { 0, 0, kW, 40 }, gui::colour::header(), 0);
        gui::label(*this, kPad, 20, "UzeSynth", gui::colour::accent(), 20);
        gui::label(*this, kW - kPad, 20, "Uzebox sound engine", gui::colour::dim(), 12,
                   ALIGN_RIGHT | ALIGN_MIDDLE);

        // patch file row
        drawIcon(kLoadBox, fHover == Hover::Load, false);
        const bool noFile = fFilePath.empty() && !fModified;
        drawRowLabel(kLoadBox, noFile ? "Click the folder to load a .inc patch file" : filesLabel(), noFile,
                     noFile ? 0 : kAddBox.w + 8);
        if (!noFile)
            gui::button(*this, kAddBox, "+ File", fHover == Hover::Add);

        // wavetable row
        drawIcon(kWavesBox, fHover == Hover::Waves, true);
        std::string wl = "Kernel default waves (10)";
        if (fWaveCount > 0)
            wl = (fWavesPath.empty() ? std::string("Custom wavetable") : baseName(fWavesPath))
               + "  (" + std::to_string(fWaveCount) + " waves)";
        drawRowLabel(kWavesBox, wl, fWaveCount == 0, fWaveCount > 0 ? kResetBox.w + 8 : 0);
        if (fWaveCount > 0)
            gui::button(*this, kResetBox, "Default", fHover == Hover::Reset);

        gui::segments(*this, kModeBox, kModeNames, 3, fMode);
        drawCombo();
        gui::button(*this, kEditBox, "Edit", fHover == Hover::Edit, true, false, 13);

        // status / info
        if (!fStatus.empty())
            gui::label(*this, kPad, 248, fStatus, gui::colour::error(), 12);
        else if (fMode == 0)
            gui::label(*this, kPad, 248, patchInfo(), gui::colour::dim(), 12);
        else
            gui::label(*this, kPad, 248, fMode == 1 ? "MIDI note n plays patch n on the noise channel. Click a patch to hear it."
                                                    : "MIDI note n triggers patch n (TriggerFx). Click a patch to hear it.",
                       gui::colour::dim(), 12);

        gui::label(*this, kPad, kMixerBox.cy(), "Kernel mixer", gui::colour::dim(), 12);
        gui::segments(*this, kMixerBox, kMixerNames, 2, fMixer, 12);

        drawKeyboard();

        gui::label(*this, kPad, kH - 16, "MIDI CC 7 volume  -  11 expression  -  92 tremolo level  -  100 tremolo rate",
                   gui::colour::faint(), 11);

        if (fListOpen)
            drawList();
    }

    // ------------------------------------------------------------------------
    // Events

    bool onMouse(const MouseEvent& ev) override
    {
        const double x = ev.pos.getX() / drawScale(), y = ev.pos.getY() / drawScale();
        if (ev.button == 1)
            fMouseDown = ev.press;

        if (fEditorOpen)
        {
            const bool r = fEditor.mouse(static_cast<int>(ev.button), ev.press, x, y, ev.mod);
            repaint();
            return r;
        }

        if (ev.button != 1)
            return false;

        if (!ev.press)
        {
            fList.bar.release();
            if (fKeyDown >= 0)
            {
                sendNote(0, static_cast<uint8_t>(fKeyDown), 0);
                fKeyDown = -1;
            }
            repaint();
            return true;
        }

        if (fListOpen)
        {
            const int count = listCount();
            if (fList.bar.press(fList.area, count, fList.visible(), x, y, fList.scroll))
            {
                repaint();
                return true;
            }
            const int row = fList.rowAt(x, y, count);
            if (row >= 0 && fMode == 0)
                selectPatch(row);
            else if (row >= 0)
            {
                // Drum kit / Sound FX: the row is a patch number (= MIDI note).
                // Select that patch and play it while the button is held.
                const int p = fBank.table[static_cast<size_t>(row)];
                if (p >= 0)
                {
                    fTableRow = row;
                    selectPatch(p);
                    fKeyDown = row;
                    sendNote(0, static_cast<uint8_t>(row), 100);
                }
            }
            if (row >= 0 || !fList.area.contains(x, y))
                fListOpen = false; // a row was chosen, or clicked outside
            repaint();
            return true;
        }

        if (kLoadBox.contains(x, y))
        {
            openBrowser(Browse::Patches, "Load Uzebox patch file (.inc)", fFilePath);
            return true;
        }
        if (!fFilePath.empty() && kAddBox.contains(x, y))
        {
            openBrowser(Browse::AddFile, "Add a file (e.g. PCM sample data)", fFilePath);
            return true;
        }
        if (fWaveCount > 0 && kResetBox.contains(x, y))
        {
            fWaveCount = 0;
            fWavesPath.clear();
            fEditor.waveCount = 0;
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
            setParam(kParamPlayMode, gui::segmentAt(kModeBox, 3, x));
            fMode = gui::segmentAt(kModeBox, 3, x);
            fEditor.playMode = fMode;
            repaint();
            return true;
        }
        if (kMixerBox.contains(x, y))
        {
            fMixer = gui::segmentAt(kMixerBox, 2, x);
            setParam(kParamMixer, fMixer);
            repaint();
            return true;
        }
        if (kEditBox.contains(x, y))
        {
            fEditorOpen = true;
            fEditor.opened();
            repaint();
            return true;
        }
        if (kComboBox.contains(x, y) && listCount() > 0)
        {
            fListOpen = true;
            fList.centre(fMode == 0 ? std::max(0, fBank.indexOf(fPatchName)) : std::max(0, currentTableRow()), listCount());
            repaint();
            return true;
        }
        if (kOctDown.contains(x, y) && octave() > -1) { --octave(); repaint(); return true; }
        if (kOctUp.contains(x, y) && octave() < 8) { ++octave(); repaint(); return true; }
        const int key = keyAt(x, y);
        if (key >= 0)
        {
            fKeyDown = key;
            sendNote(0, static_cast<uint8_t>(key), 100);
            repaint();
            return true;
        }
        return false;
    }

    bool onMotion(const MotionEvent& ev) override
    {
        const double x = ev.pos.getX() / drawScale(), y = ev.pos.getY() / drawScale();
        if (fEditorOpen)
        {
            fEditor.motion(x, y, ev.mod);
            repaint();
            return true;
        }

        if (fListOpen)
        {
            if (fList.bar.motion(fList.area, listCount(), fList.visible(), y, fList.scroll))
            {
                repaint();
                return true;
            }
            const int h = fList.rowAt(x, y, listCount());
            if (h != fList.hover) { fList.hover = h; repaint(); }
            return true;
        }

        // glide across the keyboard while held
        if (fKeyDown >= 0)
        {
            const int key = keyAt(x, y);
            if (key >= 0 && key != fKeyDown)
            {
                sendNote(0, static_cast<uint8_t>(fKeyDown), 0);
                sendNote(0, static_cast<uint8_t>(key), 100);
                fKeyDown = key;
                repaint();
            }
        }

        Hover h = Hover::None;
        if (kLoadBox.contains(x, y)) h = Hover::Load;
        else if (!fFilePath.empty() && kAddBox.contains(x, y)) h = Hover::Add;
        else if (kWavesBox.contains(x, y)) h = Hover::Waves;
        else if (fWaveCount > 0 && kResetBox.contains(x, y)) h = Hover::Reset;
        else if (kEditBox.contains(x, y)) h = Hover::Edit;
        if (h != fHover) { fHover = h; repaint(); }
        return false;
    }

    bool onScroll(const ScrollEvent& ev) override
    {
        const double x = ev.pos.getX() / drawScale(), y = ev.pos.getY() / drawScale();
        if (fEditorOpen)
        {
            const bool r = fEditor.scroll(x, y, ev.delta.getY(), ev.mod);
            repaint();
            return r;
        }

        const int dir = ev.delta.getY() > 0 ? -1 : (ev.delta.getY() < 0 ? 1 : 0);
        if (dir == 0) return false;
        if (fListOpen)
        {
            fList.scrollBy(dir * 2, listCount());
            repaint();
            return true;
        }
        if (fMode == 0 && kComboBox.contains(x, y) && !fBank.patches.empty())
        {
            int i = fBank.indexOf(fPatchName);
            i = i < 0 ? 0 : i + dir;
            if (i >= 0 && i < static_cast<int>(fBank.patches.size()))
                selectPatch(i);
            return true;
        }
        return false;
    }

    bool onKeyboard(const KeyboardEvent& ev) override
    {
        if (!fEditorOpen) return false;
        const bool r = fEditor.keyboard(ev.press, ev.key, ev.mod);
        if (r) repaint();
        return r;
    }

    bool onCharacterInput(const CharacterInputEvent& ev) override
    {
        if (!fEditorOpen) return false;
        const bool r = fEditor.character(ev.string, ev.character);
        if (r) repaint();
        return r;
    }

    // ------------------------------------------------------------------------
    // High-DPI scaling

    void uiScaleFactorChanged(const double scaleFactor) override
    {
        applyScaleFactor(scaleFactor);
    }

    void applyScaleFactor(double scaleFactor)
    {
        if (!(scaleFactor > 0.0))
            scaleFactor = 1.0;
        const uint w = static_cast<uint>(DISTRHO_UI_DEFAULT_WIDTH * scaleFactor + 0.5);
        const uint h = static_cast<uint>(DISTRHO_UI_DEFAULT_HEIGHT * scaleFactor + 0.5);
        setGeometryConstraints(w, h, true);
        if (getWidth() != w || getHeight() != h)
        {
            setSize(w, h);
            // On Windows the window can already have this size (DPF creates it
            // scaled), so no resize event follows and DPF would keep reporting
            // the unscaled size, putting clicks in the wrong places. Set the
            // widget size directly too.
            if (getWidth() != w || getHeight() != h)
                Widget::setSize(w, h);
        }
    }

    // window pixels per design unit
    double drawScale() const
    {
        const double sx = static_cast<double>(getWidth()) / DISTRHO_UI_DEFAULT_WIDTH;
        const double sy = static_cast<double>(getHeight()) / DISTRHO_UI_DEFAULT_HEIGHT;
        const double sc = sx < sy ? sx : sy;
        return sc > 0.0 ? sc : 1.0;
    }

    // ------------------------------------------------------------------------
    // PatchEditor::Host

    void editorChanged(bool structural) override
    {
        fModified = true;
        fStatus = missingSamplesMessage();
        fCommitPending = true;
        fIdleTicks = 0;
        if (structural)
            commit();
    }

    void editorSelected(const std::string& name) override
    {
        fPatchName = name;
        setState(UZESYNTH_STATE_PATCH_NAME, fPatchName.c_str());
    }

    void editorPreview(bool on) override
    {
        if (on && fCommitPending)
            commit(); // make sure the DSP has the latest edit
        int note = fEditor.previewNote;
        if (fMode != 0)
        {
            note = -1;
            const int i = fBank.indexOf(fPatchName);
            for (size_t n = 0; n < fBank.table.size(); ++n)
                if (fBank.table[n] == i) { note = static_cast<int>(n); break; }
            if (note < 0 || note > 127) return;
        }
        if (on)
        {
            fPreviewNote = note;
            sendNote(0, static_cast<uint8_t>(note), 100);
        }
        else if (fPreviewNote >= 0)
        {
            sendNote(0, static_cast<uint8_t>(fPreviewNote), 0);
            fPreviewNote = -1;
        }
    }

    void editorSave() override
    {
        if (fCommitPending)
            commit();
        const std::vector<std::string> files = splitLines(fFilePath);
        fSaveName = files.empty() ? std::string("patches.inc") : baseName(files.front());
        fBrowse = Browse::Save;
        FileBrowserOptions opts;
        opts.saving = true;
        opts.title = "Save Uzebox patch file";
        opts.defaultName = fSaveName.c_str();
        fSaveDir = files.empty() ? std::string() : dirName(files.front());
        if (!fSaveDir.empty())
            opts.startDir = fSaveDir.c_str();
        if (openFileBrowser(opts))
            return;

        // No save dialog available (on Linux DPF needs the XDG desktop portal for
        // saving). Save next to the loaded file without overwriting anything.
        fBrowse = Browse::None;
        std::string dir = fSaveDir;
        if (dir.empty())
        {
            const char* home = std::getenv("HOME");
            if (home == nullptr) home = std::getenv("USERPROFILE");
            dir = home != nullptr ? home : ".";
        }
        std::string stem = fSaveName;
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".inc") == 0)
            stem.resize(stem.size() - 4);
        std::string path;
        if (stem.size() > 7 && stem.compare(stem.size() - 7, 7, "-edited") == 0)
        {
            path = dir + "/" + fSaveName; // our own earlier save: update it
        }
        else
        {
            for (int n = 1; n < 1000; ++n)
            {
                path = dir + "/" + stem + "-edited" + (n == 1 ? std::string() : "-" + std::to_string(n)) + ".inc";
                if (FILE* f = std::fopen(path.c_str(), "rb")) std::fclose(f);
                else break;
            }
        }
        if (saveTo(path))
            fEditor.setMessage("No save dialog available - saved as " + path, false);
        repaint();
    }

    void editorWantsKeyboard() override
    {
        getWindow().focus();
    }

    void editorClose() override
    {
        if (fCommitPending)
            commit();
        fEditorOpen = false;
        repaint();
    }

private:
    enum class Hover { None, Load, Add, Waves, Reset, Edit };

    uze::PatchBank fBank;
    std::string fFileText;
    std::string fFilePath;          // one path per line
    std::string fPatchName, fWavesPath, fStatus, fSaveName, fSaveDir;
    int fWaveCount = 0;             // 0 = kernel default waves
    int fMode = 0;
    int fMixer = 0;
    bool fModified = false;
    Browse fBrowse = Browse::None;

    gui::ListView fList;
    bool fListOpen = false;
    Hover fHover = Hover::None;

    int fTableRow = -1;             // Drum kit / Sound FX: chosen patch number

    // keyboard octave: melodic mode starts at C4; Drum kit / Sound FX start at
    // note 0, since there the note number is the patch number
    int fOctaves[2] = { 4, -1 };
    int& octave() { return fOctaves[fMode != 0 ? 1 : 0]; }
    int octave() const { return fOctaves[fMode != 0 ? 1 : 0]; }
    int fKeyDown = -1;
    int fPreviewNote = -1;
    bool fMouseDown = false;

    PatchEditor fEditor;
    bool fEditorOpen = false;
    bool fCommitPending = false;
    int fIdleTicks = 0;

    // ---- helpers -------------------------------------------------------------

    void setParam(uint32_t index, int value)
    {
        editParameter(index, true);
        setParameterValue(index, static_cast<float>(value));
        editParameter(index, false);
    }

    // Regenerate the .inc text from the edited bank and send it to the DSP
    void commit()
    {
        fCommitPending = false;
        fIdleTicks = 0;
        fFileText = uze::generatePatchFile(fBank);
        setState(UZESYNTH_STATE_FILE_TEXT, fFileText.c_str());
        setState(UZESYNTH_STATE_PATCH_NAME, fPatchName.c_str());
        setState(UZESYNTH_STATE_MODIFIED, "1");
        // a new bank silences voices; keep a held preview note sounding
        if (fPreviewNote >= 0)
            sendNote(0, static_cast<uint8_t>(fPreviewNote), 100);
        repaint();
    }

    bool saveTo(const std::string& path)
    {
        if (fCommitPending)
            commit();
        const std::string text = fModified || fFileText.empty() ? uze::generatePatchFile(fBank) : fFileText;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr || std::fwrite(text.data(), 1, text.size(), f) != text.size())
        {
            if (f) std::fclose(f);
            fStatus = "Could not write " + path;
            fEditor.setMessage(fStatus, true);
            return false;
        }
        std::fclose(f);
        fFilePath = path;
        fModified = false;
        fStatus.clear();
        setState(UZESYNTH_STATE_FILE_NAME, fFilePath.c_str());
        setState(UZESYNTH_STATE_MODIFIED, "");
        return true;
    }

    void openBrowser(Browse what, const char* title, const std::string& near)
    {
        fBrowse = what;
        const std::vector<std::string> lines = splitLines(near);
        fSaveDir = dirName(lines.empty() ? near : lines.front());
        FileBrowserOptions opts;
        opts.title = title;
        if (!fSaveDir.empty())
            opts.startDir = fSaveDir.c_str();
        openFileBrowser(opts);
    }

    void loadPatches(const std::string& path, const std::string& firstText)
    {
        std::string text = firstText;
        std::string paths = path;
        uze::PatchBank bank = uze::parsePatchFile(text);
        if (bank.patches.empty())
        {
            fStatus = "No Uzebox patches found in " + baseName(path);
            return;
        }
        // PCM samples often live in another .inc next to the patch file
        if (!bank.missingSamples.empty())
        {
            for (const std::string& f : uze::findSampleFiles(path, bank.missingSamples))
            {
                std::string more;
                if (uze::readTextFile(f, more))
                {
                    text += "\n" + more;
                    paths += "\n" + f;
                }
            }
            bank = uze::parsePatchFile(text);
        }
        fModified = false;
        commitPatchText(text, paths, bank);
    }

    void addFile(const std::string& path, const std::string& more)
    {
        // if the patches were edited, the current text is the generated one
        const std::string base = fModified ? uze::generatePatchFile(fBank) : fFileText;
        const std::string text = base + "\n" + more;
        uze::PatchBank bank = uze::parsePatchFile(text);
        if (bank.patches.empty())
        {
            fStatus = "No Uzebox patches found";
            return;
        }
        commitPatchText(text, fFilePath + "\n" + path, bank);
    }

    void commitPatchText(const std::string& text, const std::string& paths, const uze::PatchBank& bank)
    {
        fBank = bank;
        fFileText = text;
        fFilePath = paths;
        fCommitPending = false;
        fStatus = missingSamplesMessage();
        if (fBank.indexOf(fPatchName) < 0)
            fPatchName = fBank.patches.front().name;
        fEditor.bankReplaced();

        setState(UZESYNTH_STATE_FILE_TEXT, fFileText.c_str());
        setState(UZESYNTH_STATE_FILE_NAME, fFilePath.c_str());
        setState(UZESYNTH_STATE_PATCH_NAME, fPatchName.c_str());
        setState(UZESYNTH_STATE_MODIFIED, fModified ? "1" : "");
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
        fEditor.waveCount = fWaveCount;
        fStatus = warning;
        setState(UZESYNTH_STATE_WAVES, uze::toHex(table.data).c_str());
        setState(UZESYNTH_STATE_WAVES_NAME, fWavesPath.c_str());
    }

    std::string missingSamplesMessage() const
    {
        if (fBank.missingSamples.empty())
            return std::string();
        std::string s = "Sample data not found: " + fBank.missingSamples.front();
        if (fBank.missingSamples.size() > 1)
            s += " (+" + std::to_string(fBank.missingSamples.size() - 1) + ")";
        return s + "  -  use + File";
    }

    std::string filesLabel() const
    {
        const std::vector<std::string> files = splitLines(fFilePath);
        std::string s = files.empty() ? std::string("untitled.inc") : baseName(files.front());
        for (size_t i = 1; i < files.size() && i < 3; ++i)
            s += " + " + baseName(files[i]);
        if (files.size() > 3)
            s += " + " + std::to_string(files.size() - 3) + " more";
        if (fModified)
            s += "  (edited, unsaved)";
        return s;
    }

    std::string patchInfo() const
    {
        const int i = fBank.indexOf(fPatchName);
        if (i < 0) return std::string();
        const uze::Patch& p = fBank.patches[static_cast<size_t>(i)];
        char buf[160];
        if (p.data.type == uze::PatchType::Pcm)
            std::snprintf(buf, sizeof(buf), "PCM channel  -  sample %u bytes, loop %d-%d  -  %u patches",
                          static_cast<unsigned>(p.data.sample.size()), static_cast<int>(p.data.loopStart),
                          static_cast<int>(p.data.loopEnd), static_cast<unsigned>(fBank.patches.size()));
        else
            std::snprintf(buf, sizeof(buf), "%s channel  -  %u bytes  -  %u patches in file",
                          p.data.type == uze::PatchType::Noise ? "Noise" : "Wave",
                          static_cast<unsigned>(p.data.stream.size()), static_cast<unsigned>(fBank.patches.size()));
        return buf;
    }

    void selectPatch(int index)
    {
        fPatchName = fBank.patches[static_cast<size_t>(index)].name;
        setState(UZESYNTH_STATE_PATCH_NAME, fPatchName.c_str());
        repaint();
    }

    // Drum kit / Sound FX: the patch number (MIDI note) of the selected patch:
    // the row last chosen if it still holds that patch, else its first entry
    int currentTableRow() const
    {
        const int i = fBank.indexOf(fPatchName);
        if (i < 0) return -1;
        if (fTableRow >= 0 && fTableRow < static_cast<int>(fBank.table.size())
            && fBank.table[static_cast<size_t>(fTableRow)] == i)
            return fTableRow;
        for (size_t n = 0; n < fBank.table.size() && n < 128; ++n)
            if (fBank.table[n] == i) return static_cast<int>(n);
        return -1;
    }

    // Melodic: all patches. Drum/FX: the patch table, which MIDI notes index.
    int listCount() const
    {
        return static_cast<int>(fMode == 0 ? fBank.patches.size() : std::min<size_t>(fBank.table.size(), 128));
    }

    static std::string patchLabel(const uze::Patch& p)
    {
        std::string s = p.name;
        if (p.data.type == uze::PatchType::Pcm)
            s += p.sampleMissing ? "  [PCM, sample missing]" : "  [PCM]";
        if (!p.comment.empty())
            s += "  -  " + p.comment;
        return s;
    }

    Color patchColour(int patchIndex) const
    {
        if (fMode == 1) return gui::colour::noise(); // drum kit: noise channel
        if (patchIndex < 0) return gui::colour::faint();
        const uze::Patch& p = fBank.patches[static_cast<size_t>(patchIndex)];
        switch (p.data.type)
        {
        case uze::PatchType::Noise: return gui::colour::noise();
        case uze::PatchType::Pcm:   return p.sampleMissing ? gui::colour::error() : gui::colour::pcm();
        default:                    return gui::colour::text();
        }
    }

    // ---- drawing -------------------------------------------------------------

    void drawRowLabel(const Box& b, const std::string& text, bool dim, float rightReserve)
    {
        const float tx = b.x + b.w + 12;
        gui::clippedLabel(*this, { tx, b.y, kW - tx - kPad - rightReserve, b.h }, tx, b.cy(), text,
                          dim ? gui::colour::dim() : gui::colour::text(), 14);
    }

    void drawIcon(const Box& b, bool hover, bool waveIcon)
    {
        gui::fillBox(*this, b, hover ? gui::colour::hover() : gui::colour::control(), 6);
        const float x = b.x + 10, y = b.y + 11, w = b.w - 20, h = b.h - 22;
        if (waveIcon)
        {
            beginPath();
            for (int i = 0; i <= 12; ++i)
            {
                const float px = x + w * i / 12;
                const float py = y + h / 2 - std::sin(6.2831853f * i / 12) * h / 2;
                if (i == 0) moveTo(px, py);
                else lineTo(px, std::round(py));
            }
            strokeColor(gui::colour::noise());
            strokeWidth(2.0f);
            stroke();
            return;
        }
        beginPath();
        moveTo(x, y + 2);
        lineTo(x + w * 0.38f, y + 2);
        lineTo(x + w * 0.48f, y + 6);
        lineTo(x + w, y + 6);
        lineTo(x + w, y + h);
        lineTo(x, y + h);
        closePath();
        fillColor(gui::colour::accent());
        fill();
        gui::fillBox(*this, { x, y + 9, w, h - 9 }, Color(255, 220, 110), 0);
    }

    void drawCombo()
    {
        const Box& b = kComboBox;
        gui::fillBox(*this, b, gui::colour::control());
        gui::strokeBox(*this, b, fListOpen ? gui::colour::accent() : gui::colour::border());
        beginPath();
        const float ax = b.x + b.w - 18, ay = b.cy();
        moveTo(ax - 6, ay - 3);
        lineTo(ax + 6, ay - 3);
        lineTo(ax, ay + 4);
        closePath();
        fillColor(Color(200, 204, 214));
        fill();

        std::string label;
        bool dim = false;
        const int i = fBank.indexOf(fPatchName);
        if (fBank.patches.empty()) { label = "No patches loaded"; dim = true; }
        else if (fMode != 0)
        {
            const int row = currentTableRow();
            if (row >= 0)
                label = "Note " + std::to_string(row) + " (" + uze::noteName(row) + "):  "
                      + patchLabel(fBank.patches[static_cast<size_t>(fBank.table[static_cast<size_t>(row)])]);
            else
                label = "Patch table: notes 0-" + std::to_string(listCount() - 1) + "  (click to choose)";
        }
        else if (i >= 0) label = patchLabel(fBank.patches[static_cast<size_t>(i)]);
        else { label = "Choose a patch"; dim = true; }
        gui::clippedLabel(*this, { b.x + 8, b.y, b.w - 40, b.h }, b.x + 10, b.cy(), label,
                          dim ? gui::colour::faint() : gui::colour::text(), 14);
    }

    void drawList()
    {
        const int count = listCount();
        const int current = fMode == 0 ? fBank.indexOf(fPatchName) : currentTableRow();
        fList.draw(*this, count, current, [&](int i, const Box& r) {
            if (fMode == 0)
            {
                gui::label(*this, r.x + 8, r.cy(), patchLabel(fBank.patches[static_cast<size_t>(i)]), patchColour(i), 13);
                return;
            }
            const int p = fBank.table[static_cast<size_t>(i)];
            gui::label(*this, r.x + 30, r.cy(), std::to_string(i), patchColour(p), 13, ALIGN_RIGHT | ALIGN_MIDDLE);
            gui::label(*this, r.x + 40, r.cy(), uze::noteName(i), gui::colour::dim(), 13);
            gui::label(*this, r.x + 82, r.cy(), p < 0 ? std::string("(empty)") : patchLabel(fBank.patches[static_cast<size_t>(p)]),
                       patchColour(p), 13);
        }, true);
    }

    // ---- on-screen keyboard (two octaves + C) ------------------------------------

    static bool isBlack(int n) { const int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

    int firstNote() const { return (octave() + 1) * 12; } // C of the octave

    // returns the note under (x,y) or -1
    int keyAt(double x, double y) const
    {
        if (!kKeys.contains(x, y)) return -1;
        const int whites = 15;
        const float ww = kKeys.w / whites;
        // black keys first (upper 60%)
        if (y < kKeys.y + kKeys.h * 0.6f)
        {
            int white = 0;
            for (int n = firstNote(); n <= firstNote() + 24; ++n)
            {
                if (isBlack(n))
                {
                    const float bx = kKeys.x + white * ww - ww * 0.3f;
                    if (x >= bx && x < bx + ww * 0.6f) return std::min(n, 126);
                }
                else
                    ++white;
            }
        }
        int white = static_cast<int>((x - kKeys.x) / ww);
        for (int n = firstNote(); n <= firstNote() + 24; ++n)
            if (!isBlack(n) && white-- == 0) return std::min(n, 126);
        return -1;
    }

    void drawKeyboard()
    {
        gui::button(*this, kOctDown, "<", false, octave() > -1);
        gui::button(*this, kOctUp, ">", false, octave() < 8);
        gui::label(*this, kOctUp.x + kOctUp.w + 10, kOctUp.cy(),
                   fMode == 0 ? "Play: " + uze::noteName(firstNote()) + " - " + uze::noteName(firstNote() + 24)
                              : "Play: notes " + std::to_string(firstNote()) + "-" + std::to_string(firstNote() + 24)
                                + " (= patch numbers)",
                   gui::colour::dim(), 12);

        const int whites = 15;
        const float ww = kKeys.w / whites;
        int white = 0;
        for (int n = firstNote(); n <= firstNote() + 24; ++n)
        {
            if (isBlack(n)) continue;
            const Box k { kKeys.x + white * ww + 1, kKeys.y, ww - 2, kKeys.h };
            gui::fillBox(*this, k, n == fKeyDown ? gui::colour::accent() : Color(225, 228, 236), 3);
            if (n % 12 == 0)
                gui::label(*this, k.cx(), k.y + k.h - 10, uze::noteName(n), Color(90, 96, 110), 10,
                           ALIGN_CENTER | ALIGN_MIDDLE);
            ++white;
        }
        white = 0;
        for (int n = firstNote(); n <= firstNote() + 24; ++n)
        {
            if (!isBlack(n)) { ++white; continue; }
            const Box k { kKeys.x + white * ww - ww * 0.3f, kKeys.y, ww * 0.6f, kKeys.h * 0.6f };
            gui::fillBox(*this, k, n == fKeyDown ? gui::colour::accent() : Color(30, 32, 40), 3);
        }
    }

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UzeSynthUI)
};

UI* createUI()
{
    return new UzeSynthUI();
}

END_NAMESPACE_DISTRHO
