/*
 * UzeSynth - integrated patch editor page
 *
 * Edits the plugin's PatchBank in place and tells its Host when something
 * changed; the host regenerates the .inc text and sends it to the DSP.
 *
 * Every value can be changed with the mouse alone (drag up/down, wheel,
 * Shift = x10). Clicking a value without dragging focuses it for typing.
 *
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#pragma once

#include "Gui.hpp"
#include "../engine/PatchEdit.hpp"
#include "../engine/PatchParser.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

START_NAMESPACE_DISTRHO

namespace gui = DGL_NAMESPACE::gui;
using gui::Box;

class PatchEditor
{
public:
    struct Host {
        virtual ~Host() {}
        virtual void editorChanged(bool structural) = 0;   // the bank was modified
        virtual void editorSelected(const std::string& name) = 0;
        virtual void editorPreview(bool on) = 0;           // hold the Play button
        virtual void editorSave() = 0;
        virtual void editorClose() = 0;
    };

    static constexpr float W = DISTRHO_UI_DEFAULT_WIDTH;
    static constexpr float H = DISTRHO_UI_DEFAULT_HEIGHT;

    PatchEditor(Host& host, uze::PatchBank& bank, std::string& selected)
        : fHost(host), fBank(bank), fSelected(selected)
    {
        fTable.area = { 16, 168, W - 32, 8 + 22 * 10 };
        fCmdPopup.area = { 120, 52, 300, 8 + 20 * uze::kNumCommands };
        fCmdPopup.rowH = 20;
        fPatchPopup.area = { 16, 80, W - 32, H - 96 };
    }

    int previewNote = 60;
    int playMode = 0;          // 0 melodic, 1 drum kit, 2 sound fx
    int waveCount = 0;         // 0 = kernel default waves
    bool playing = false;

    // ------------------------------------------------------------------------

    void opened()
    {
        ensureSelection();
        fRow = 0;
        fTable.scroll = 0;
        cancelFocus();
        fMessage.clear();
        fDeleteArmed = false;
    }

    // called after the bank was replaced from outside (e.g. a new file)
    void bankReplaced()
    {
        // Don't pick a patch here: when a session is restored the selected
        // patch name may arrive after the file. opened() picks one if needed.
        fUndo.clear();
        fUndoSelected.clear();
        cancelFocus();
        fRow = std::min(fRow, rowCount() - 1);
    }

    bool isTyping() const { return fFocus != F::None; }

    void setMessage(const std::string& m, bool isError)
    {
        fMessage = m;
        fMessageIsError = isError;
    }

    // ------------------------------------------------------------------------
    // drawing

    void draw(NanoVG& vg)
    {
        gui::fillBox(vg, { 0, 0, W, H }, gui::colour::bg(), 0);
        gui::fillBox(vg, { 0, 0, W, 40 }, gui::colour::header(), 0);
        gui::label(vg, 16, 20, "Patch editor", gui::colour::accent(), 18);
        gui::button(vg, kDone, "Done", fHover == kDoneId);

        const uze::Patch* p = patch();

        // patch selector + management
        gui::fillBox(vg, kCombo, gui::colour::control());
        gui::strokeBox(vg, kCombo, fPatchPopupOpen ? gui::colour::accent() : gui::colour::border());
        gui::clippedLabel(vg, { kCombo.x, kCombo.y, kCombo.w - 26, kCombo.h }, kCombo.x + 10, kCombo.cy(),
                          p ? numberedName(*p) : std::string("No patches - click New"),
                          p ? gui::colour::text() : gui::colour::dim(), 14);
        arrow(vg, kCombo.x + kCombo.w - 16, kCombo.cy());
        gui::button(vg, kNew, "New", fHover == kNewId);
        gui::button(vg, kCopy, "Copy", fHover == kCopyId, p != nullptr);
        gui::button(vg, kDelete, fDeleteArmed ? "Sure?" : "Delete", fHover == kDeleteId, p != nullptr, fDeleteArmed);

        if (p != nullptr)
        {
            drawPatchFields(vg, *p);
            drawTable(vg, *p);
        }

        // bottom row
        const bool hasRow = p != nullptr && fRow < static_cast<int>(stream().commands.size());
        gui::button(vg, kInsert, "Insert", fHover == kInsertId, p != nullptr);
        gui::button(vg, kDelRow, "Delete", fHover == kDelRowId, hasRow);
        gui::button(vg, kUp, "\xe2\x96\xb2", fHover == kUpId, hasRow && fRow > 0);      // ▲
        gui::button(vg, kDown, "\xe2\x96\xbc", fHover == kDownId,
                    hasRow && fRow + 1 < static_cast<int>(stream().commands.size())); // ▼
        gui::button(vg, kUndo, "Undo", fHover == kUndoId, !fUndo.empty());
        gui::button(vg, kPlay, "\xe2\x96\xb6 Play", fHover == kPlayId, p != nullptr, playing);
        if (playMode == 0)
            numberField(vg, kNote, F::Note, 0, uze::noteName(previewNote));
        else
            gui::label(vg, kNote.cx(), kNote.cy(), "note " + std::to_string(std::max(0, tableNumber())),
                       gui::colour::dim(), 12, NanoVG::ALIGN_CENTER | NanoVG::ALIGN_MIDDLE);
        gui::button(vg, kSave, "Save .inc...", fHover == kSaveId, !fBank.patches.empty());

        drawStatus(vg);

        if (fCmdPopupOpen)
            drawCommandPopup(vg);
        if (fPatchPopupOpen)
            drawPatchPopup(vg);
    }

    // ------------------------------------------------------------------------
    // input

    bool mouse(int button, bool press, double x, double y, uint mods)
    {
        if (button != 1)
            return false;

        if (!press)
        {
            bool handled = false;
            if (playing)
            {
                playing = false;
                fHost.editorPreview(false);
                handled = true;
            }
            if (fTable.bar.dragging || fCmdPopup.bar.dragging || fPatchPopup.bar.dragging)
            {
                fTable.bar.release();
                fCmdPopup.bar.release();
                fPatchPopup.bar.release();
                handled = true;
            }
            if (fDrag.active)
            {
                if (fDrag.moved)
                    fHost.editorChanged(false);
                else
                    focus(fDrag.field, fDrag.row); // click without drag: type a value
                fDrag.active = false;
                handled = true;
            }
            return handled;
        }

        // popups first
        if (fCmdPopupOpen)
        {
            if (fCmdPopup.bar.press(fCmdPopup.area, uze::kNumCommands, fCmdPopup.visible(), x, y, fCmdPopup.scroll))
                return true;
            const int r = fCmdPopup.rowAt(x, y, uze::kNumCommands);
            if (r >= 0)
                setCommand(fCmdRow, static_cast<uint8_t>(r));
            fCmdPopupOpen = false;
            return true;
        }
        if (fPatchPopupOpen)
        {
            const int count = static_cast<int>(fBank.patches.size());
            if (fPatchPopup.bar.press(fPatchPopup.area, count, fPatchPopup.visible(), x, y, fPatchPopup.scroll))
                return true;
            const int r = fPatchPopup.rowAt(x, y, count);
            if (r >= 0)
                select(r);
            fPatchPopupOpen = false;
            return true;
        }

        const bool wasArmed = fDeleteArmed;
        fDeleteArmed = false;
        commitFocus();
        fMessage.clear();

        if (kDone.contains(x, y)) { fHost.editorClose(); return true; }
        if (kCombo.contains(x, y) && !fBank.patches.empty())
        {
            fPatchPopupOpen = true;
            fPatchPopup.centre(patchIndex(), static_cast<int>(fBank.patches.size()));
            return true;
        }
        if (kNew.contains(x, y)) { newPatch(); return true; }
        if (kCopy.contains(x, y) && patch()) { copyPatch(); return true; }
        if (kDelete.contains(x, y) && patch())
        {
            if (wasArmed) deletePatch();
            else fDeleteArmed = true;
            return true;
        }
        if (kSave.contains(x, y) && !fBank.patches.empty()) { fHost.editorSave(); return true; }
        if (kUndo.contains(x, y)) { undo(); return true; }

        uze::Patch* p = patch();
        if (p == nullptr)
            return true;

        if (kPlay.contains(x, y))
        {
            playing = true;
            fHost.editorPreview(true);
            return true;
        }
        if (playMode == 0 && kNote.contains(x, y)) { startDrag(F::Note, 0, y); return true; }
        if (kName.contains(x, y)) { focus(F::Name, 0); return true; }
        if (kComment.contains(x, y)) { focus(F::Comment, 0); return true; }

        if (p->data.type == uze::PatchType::Pcm)
        {
            if (kLoopStart.contains(x, y)) { startDrag(F::LoopStart, 0, y); return true; }
            if (kLoopEnd.contains(x, y)) { startDrag(F::LoopEnd, 0, y); return true; }
        }
        else if (kChannel.contains(x, y))
        {
            const uze::PatchType t = gui::segmentAt(kChannel, 2, x) == 1 ? uze::PatchType::Noise : uze::PatchType::Wave;
            if (t != p->data.type)
            {
                pushUndo("channel");
                p->data.type = t;
                fHost.editorChanged(true);
            }
            return true;
        }

        const int n = static_cast<int>(stream().commands.size());
        if (kInsert.contains(x, y)) { insertRow(); return true; }
        if (kDelRow.contains(x, y) && fRow < n) { deleteRow(); return true; }
        if (kUp.contains(x, y) && fRow < n && fRow > 0) { moveRow(-1); return true; }
        if (kDown.contains(x, y) && fRow + 1 < n) { moveRow(1); return true; }

        // command table
        if (fTable.bar.press(fTable.area, rowCount(), fTable.visible(), x, y, fTable.scroll))
            return true;
        const int row = fTable.rowAt(x, y, rowCount());
        if (row >= 0)
        {
            fRow = row;
            const Box r = fTable.rowBox(row);
            if (cellDelta(r).contains(x, y))
                startDrag(row < n ? F::Delta : F::EndDelta, row, y);
            else if (row < n && cellCommand(r).contains(x, y))
            {
                fCmdRow = row;
                fCmdPopupOpen = true;
                fCmdPopup.scroll = 0;
                fCmdPopup.hover = stream().commands[static_cast<size_t>(row)].cmd;
            }
            else if (row < n && cellParam(r).contains(x, y)
                     && uze::commandInfo(stream().commands[static_cast<size_t>(row)].cmd).hasParam)
                startDrag(F::Param, row, y);
            return true;
        }
        (void)mods;
        return true; // the editor owns the whole window
    }

    bool motion(double x, double y, uint mods)
    {
        if (fDrag.active)
        {
            const double dy = fDrag.startY - y;
            if (!fDrag.moved && std::fabs(dy) >= 3)
            {
                fDrag.moved = true;
                pushUndo(fieldKey(fDrag.field, fDrag.row) + "#drag" + std::to_string(++fGesture));
            }
            if (fDrag.moved)
            {
                const int scale = (fDrag.field == F::LoopStart || fDrag.field == F::LoopEnd) ? 8 : 1;
                const int step = (mods & kModifierShift) ? 10 * scale : scale;
                setValue(fDrag.field, fDrag.row, fDrag.startValue + static_cast<int>(dy / 4) * step);
            }
            return true;
        }
        if (fTable.bar.dragging)
            return fTable.bar.motion(fTable.area, rowCount(), fTable.visible(), y, fTable.scroll), true;
        if (fCmdPopup.bar.dragging)
            return fCmdPopup.bar.motion(fCmdPopup.area, uze::kNumCommands, fCmdPopup.visible(), y, fCmdPopup.scroll), true;
        if (fPatchPopup.bar.dragging)
            return fPatchPopup.bar.motion(fPatchPopup.area, static_cast<int>(fBank.patches.size()),
                                          fPatchPopup.visible(), y, fPatchPopup.scroll), true;

        int hover = -1;
        const Box* ids[] = { &kDone, &kNew, &kCopy, &kDelete, &kInsert, &kDelRow, &kUp, &kDown, &kUndo, &kPlay, &kSave };
        for (int i = 0; i < 11; ++i)
            if (ids[i]->contains(x, y)) hover = i;
        fHover = hover;
        fTable.hover = (fCmdPopupOpen || fPatchPopupOpen) ? -1 : fTable.rowAt(x, y, rowCount());
        fCmdPopup.hover = fCmdPopupOpen ? fCmdPopup.rowAt(x, y, uze::kNumCommands) : -1;
        fPatchPopup.hover = fPatchPopupOpen ? fPatchPopup.rowAt(x, y, static_cast<int>(fBank.patches.size())) : -1;
        fMouseX = x;
        fMouseY = y;
        return true;
    }

    bool scroll(double x, double y, double dy, uint mods)
    {
        const int dir = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
        if (dir == 0) return false;

        if (fCmdPopupOpen) return fCmdPopup.scrollBy(-dir * 2, uze::kNumCommands), true;
        if (fPatchPopupOpen) return fPatchPopup.scrollBy(-dir * 2, static_cast<int>(fBank.patches.size())), true;

        uze::Patch* p = patch();
        if (p == nullptr) return false;
        const int step = (mods & kModifierShift) ? 10 : 1;

        F f = F::None;
        int row = 0;
        if (playMode == 0 && kNote.contains(x, y)) f = F::Note;
        else if (p->data.type == uze::PatchType::Pcm && kLoopStart.contains(x, y)) f = F::LoopStart;
        else if (p->data.type == uze::PatchType::Pcm && kLoopEnd.contains(x, y)) f = F::LoopEnd;
        else if (kCombo.contains(x, y))
        {
            const int i = patchIndex() - dir;
            if (i >= 0 && i < static_cast<int>(fBank.patches.size())) select(i);
            return true;
        }
        else
        {
            const int r = fTable.rowAt(x, y, rowCount());
            if (r >= 0)
            {
                const Box b = fTable.rowBox(r);
                const int n = static_cast<int>(stream().commands.size());
                if (cellDelta(b).contains(x, y)) { f = r < n ? F::Delta : F::EndDelta; row = r; }
                else if (r < n && cellParam(b).contains(x, y)
                         && uze::commandInfo(stream().commands[static_cast<size_t>(r)].cmd).hasParam)
                { f = F::Param; row = r; }
            }
            if (f == F::None)
            {
                if (fTable.area.contains(x, y))
                    return fTable.scrollBy(-dir, rowCount()), true;
                return false;
            }
        }

        // wheel edits: one undo step per field until something else happens
        const std::string key = fieldKey(f, row) + "#wheel";
        if (key != fLastUndoKey) pushUndo(key);
        setValue(f, row, getValue(f, row) + dir * step * (f == F::LoopStart || f == F::LoopEnd ? 1 : 1));
        fHost.editorChanged(false);
        return true;
    }

    bool keyboard(bool press, uint key, uint mods)
    {
        if (!press) return false;

        if (fFocus == F::Name || fFocus == F::Comment)
        {
            if (key == kKeyEnter || key == kKeyTab) { commitFocus(); return true; }
            if (key == kKeyEscape) { cancelFocus(); return true; }
            if (key == kKeyBackspace) { popChar(fBuffer); return true; }
            return key < 0xE000; // printable keys arrive via character()
        }
        if (fFocus != F::None)
        {
            if (key == kKeyEnter || key == kKeyTab) { commitFocus(); return true; }
            if (key == kKeyEscape) { cancelFocus(); return true; }
            if (key == kKeyBackspace) { popChar(fBuffer); return true; }
            if (key == kKeyUp || key == kKeyDown)
            {
                const int step = (mods & kModifierShift) ? 10 : 1;
                const std::string k = fieldKey(fFocus, fFocusRow) + "#key";
                if (k != fLastUndoKey) pushUndo(k);
                setValue(fFocus, fFocusRow, getValue(fFocus, fFocusRow) + (key == kKeyUp ? step : -step));
                fBuffer.clear();
                fHost.editorChanged(false);
                return true;
            }
            return key < 0xE000;
        }

        // no field focused
        if ((mods & kModifierControl) && (key == 'z' || key == 'Z')) { undo(); return true; }
        if (patch() == nullptr) return false;
        const int n = static_cast<int>(stream().commands.size());
        if (key == kKeyUp && fRow > 0) { --fRow; fTable.show(fRow, rowCount()); return true; }
        if (key == kKeyDown && fRow + 1 < rowCount()) { ++fRow; fTable.show(fRow, rowCount()); return true; }
        if (key == kKeyInsert) { insertRow(); return true; }
        if ((key == kKeyDelete) && fRow < n) { deleteRow(); return true; }
        return false;
    }

    bool character(const char* utf8, uint codepoint)
    {
        if (fFocus == F::None || codepoint < 32 || codepoint == 127) return false;
        if (fFocus == F::Name)
        {
            if (codepoint < 128 && (std::isalnum(static_cast<int>(codepoint)) || codepoint == '_') && fBuffer.size() < 48)
                fBuffer += static_cast<char>(codepoint);
            return true;
        }
        if (fFocus == F::Comment)
        {
            if (fBuffer.size() < 60) fBuffer += utf8;
            return true;
        }
        // numbers
        if ((codepoint >= '0' && codepoint <= '9') || (codepoint == '-' && fBuffer.empty()))
            if (fBuffer.size() < 6) fBuffer += static_cast<char>(codepoint);
        return true;
    }

private:
    enum class F { None, Name, Comment, Delta, Param, EndDelta, LoopStart, LoopEnd, Note };

    // layout
    const Box kDone      { W - 16 - 64, 9, 64, 22 };
    const Box kCombo     { 16, 48, 254, 28 };
    const Box kNew       { 278, 48, 70, 28 };
    const Box kCopy      { 352, 48, 70, 28 };
    const Box kDelete    { 426, 48, W - 16 - 426, 28 };
    const Box kName      { 64, 86, 190, 24 };
    const Box kComment   { 330, 86, W - 16 - 330, 24 };
    const Box kChannel   { 80, 120, 170, 24 };
    const Box kLoopStart { 316, 120, 90, 24 };
    const Box kLoopEnd   { 414, 120, W - 16 - 414, 24 };
    const Box kInsert    { 16, 404, 62, 26 };
    const Box kDelRow    { 82, 404, 62, 26 };
    const Box kUp        { 148, 404, 30, 26 };
    const Box kDown      { 182, 404, 30, 26 };
    const Box kUndo      { 216, 404, 56, 26 };
    const Box kPlay      { 286, 404, 62, 26 };
    const Box kNote      { 352, 404, 58, 26 };
    const Box kSave      { 420, 404, W - 16 - 420, 26 };
    enum { kDoneId, kNewId, kCopyId, kDeleteId, kInsertId, kDelRowId, kUpId, kDownId, kUndoId, kPlayId, kSaveId };

    static Box cellDelta(const Box& r)   { return { r.x + 28, r.y + 2, 46, r.h - 4 }; }
    static Box cellCommand(const Box& r) { return { r.x + 80, r.y + 2, 150, r.h - 4 }; }
    static Box cellParam(const Box& r)   { return { r.x + 236, r.y + 2, 56, r.h - 4 }; }

    Host& fHost;
    uze::PatchBank& fBank;
    std::string& fSelected;

    gui::ListView fTable, fCmdPopup, fPatchPopup;
    bool fCmdPopupOpen = false, fPatchPopupOpen = false;
    int fCmdRow = 0;
    int fRow = 0;
    int fHover = -1;
    double fMouseX = 0, fMouseY = 0;

    F fFocus = F::None;
    int fFocusRow = 0;
    std::string fBuffer;

    struct Drag { bool active = false, moved = false; F field = F::None; int row = 0; double startY = 0; int startValue = 0; } fDrag;

    std::vector<uze::PatchBank> fUndo;
    std::vector<std::string> fUndoSelected;
    std::string fLastUndoKey;
    int fGesture = 0;
    bool fDeleteArmed = false;
    std::string fMessage;
    bool fMessageIsError = false;

    // ---- model access --------------------------------------------------------

    int patchIndex() const { return fBank.indexOf(fSelected); }
    uze::Patch* patch() { const int i = patchIndex(); return i >= 0 ? &fBank.patches[static_cast<size_t>(i)] : nullptr; }
    const uze::Patch* patch() const { const int i = patchIndex(); return i >= 0 ? &fBank.patches[static_cast<size_t>(i)] : nullptr; }

    uze::EditStream stream() const
    {
        const uze::Patch* p = patch();
        return p ? uze::decodeStream(p->data.stream) : uze::EditStream();
    }
    void setStream(const uze::EditStream& s)
    {
        if (uze::Patch* p = patch())
            p->data.stream = uze::encodeStream(s);
    }
    int rowCount() const { return static_cast<int>(stream().commands.size()) + 1; } // + end row

    int tableNumber() const
    {
        const int i = patchIndex();
        for (size_t n = 0; n < fBank.table.size(); ++n)
            if (fBank.table[n] == i) return static_cast<int>(n);
        return -1;
    }

    std::string numberedName(const uze::Patch& p) const
    {
        const int i = fBank.indexOf(p.name);
        int num = -1;
        for (size_t n = 0; n < fBank.table.size(); ++n)
            if (fBank.table[n] == i) { num = static_cast<int>(n); break; }
        std::string s = (num >= 0 ? std::to_string(num) + ": " : std::string()) + p.name;
        if (p.data.type == uze::PatchType::Pcm) s += "  [PCM]";
        if (!p.comment.empty()) s += "  -  " + p.comment;
        return s;
    }

    void ensureSelection()
    {
        if (patchIndex() < 0 && !fBank.patches.empty())
        {
            fSelected = fBank.patches.front().name;
            fHost.editorSelected(fSelected);
        }
    }

    void select(int index)
    {
        commitFocus();
        fSelected = fBank.patches[static_cast<size_t>(index)].name;
        fRow = 0;
        fTable.scroll = 0;
        fLastUndoKey.clear();
        fHost.editorSelected(fSelected);
    }

    // ---- values --------------------------------------------------------------

    static std::string fieldKey(F f, int row) { return std::to_string(static_cast<int>(f)) + ":" + std::to_string(row); }

    int getValue(F f, int row) const
    {
        const uze::Patch* p = patch();
        if (f == F::Note) return previewNote;
        if (p == nullptr) return 0;
        const uze::EditStream s = stream();
        switch (f)
        {
        case F::Delta:     return row < static_cast<int>(s.commands.size()) ? s.commands[static_cast<size_t>(row)].delta : 0;
        case F::EndDelta:  return s.endDelta;
        case F::Param:
            if (row >= static_cast<int>(s.commands.size())) return 0;
            return uze::paramToDisplay(s.commands[static_cast<size_t>(row)].cmd, s.commands[static_cast<size_t>(row)].param);
        case F::LoopStart: return p->data.loopStart;
        case F::LoopEnd:   return p->data.loopEnd;
        default:           return 0;
        }
    }

    void setValue(F f, int row, int v)
    {
        if (f == F::Note)
        {
            previewNote = std::max(0, std::min(126, v));
            return;
        }
        uze::Patch* p = patch();
        if (p == nullptr) return;
        uze::EditStream s = stream();
        switch (f)
        {
        case F::Delta:
            if (row < static_cast<int>(s.commands.size()))
                s.commands[static_cast<size_t>(row)].delta = static_cast<uint8_t>(std::max(0, std::min(255, v)));
            break;
        case F::EndDelta:
            s.endDelta = static_cast<uint8_t>(std::max(0, std::min(255, v)));
            break;
        case F::Param:
            if (row < static_cast<int>(s.commands.size()))
            {
                uze::EditCommand& c = s.commands[static_cast<size_t>(row)];
                c.param = uze::displayToParam(c.cmd, v);
            }
            break;
        case F::LoopStart: p->data.loopStart = std::max(0, std::min(65535, v)); return;
        case F::LoopEnd:   p->data.loopEnd = std::max(0, std::min(65535, v)); return;
        default: return;
        }
        setStream(s);
    }

    std::string valueText(F f, int row) const
    {
        if (f == F::Note) return uze::noteName(previewNote);
        return std::to_string(getValue(f, row));
    }

    void startDrag(F f, int row, double y)
    {
        commitFocus();
        fDrag.active = true;
        fDrag.moved = false;
        fDrag.field = f;
        fDrag.row = row;
        fDrag.startY = y;
        fDrag.startValue = getValue(f, row);
    }

    // ---- focus / typing --------------------------------------------------------

    void focus(F f, int row)
    {
        fFocus = f;
        fFocusRow = row;
        const uze::Patch* p = patch();
        if (f == F::Name) fBuffer = p ? p->name : "";
        else if (f == F::Comment) fBuffer = p ? p->comment : "";
        else fBuffer.clear(); // numbers: start empty, shows the current value until typing
    }

    void cancelFocus()
    {
        fFocus = F::None;
        fBuffer.clear();
    }

    void commitFocus()
    {
        const F f = fFocus;
        if (f == F::None) return;
        uze::Patch* p = patch();
        fFocus = F::None;

        if (p != nullptr && f == F::Name)
        {
            if (fBuffer != p->name)
            {
                if (!uze::isValidIdentifier(fBuffer))
                    return showError("Names must be C identifiers (letters, digits, _)");
                if (fBank.indexOf(fBuffer) >= 0 || uze::uniqueName(fBank, fBuffer) != fBuffer)
                    return showError("Name '" + fBuffer + "' is already used");
                if (p->data.type == uze::PatchType::Pcm && p->sampleMissing)
                    return showError("Can't rename: its sample data is missing");
                pushUndo("rename");
                if (p->data.type == uze::PatchType::Pcm)
                    p->sampleName = fBuffer;   // a PCM patch is named after its sample
                else
                    p->cmdName = fBuffer;
                p->name = fBuffer;
                fSelected = fBuffer;
                fHost.editorSelected(fSelected);
                fHost.editorChanged(true);
            }
        }
        else if (p != nullptr && f == F::Comment)
        {
            if (fBuffer != p->comment)
            {
                pushUndo("comment");
                p->comment = fBuffer;
                fHost.editorChanged(true);
            }
        }
        else if (!fBuffer.empty() && fBuffer != "-")
        {
            pushUndo(fieldKey(f, fFocusRow) + "#typed");
            setValue(f, fFocusRow, std::atoi(fBuffer.c_str()));
            if (f != F::Note) fHost.editorChanged(false);
        }
        fBuffer.clear();
    }

    static void popChar(std::string& s)
    {
        if (s.empty()) return;
        // remove one UTF-8 character
        size_t n = s.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        s.erase(n);
    }

    void showError(const std::string& m) { fMessage = m; fMessageIsError = true; fBuffer.clear(); }

    // ---- undo ----------------------------------------------------------------

    void pushUndo(const std::string& key)
    {
        fLastUndoKey = key;
        fUndo.push_back(fBank);
        fUndoSelected.push_back(fSelected);
        if (fUndo.size() > 50)
        {
            fUndo.erase(fUndo.begin());
            fUndoSelected.erase(fUndoSelected.begin());
        }
    }

    void undo()
    {
        if (fUndo.empty()) return;
        cancelFocus();
        fBank = fUndo.back();
        fSelected = fUndoSelected.back();
        fUndo.pop_back();
        fUndoSelected.pop_back();
        fLastUndoKey.clear();
        fRow = std::min(fRow, rowCount() - 1);
        fHost.editorSelected(fSelected);
        fHost.editorChanged(true);
    }

    // ---- structural edits ------------------------------------------------------

    void insertRow()
    {
        if (patch() == nullptr) return;
        pushUndo("insert");
        uze::EditStream s = stream();
        uze::EditCommand c;
        const int at = std::min(fRow + 1, static_cast<int>(s.commands.size()));
        if (fRow < static_cast<int>(s.commands.size()))
            c.cmd = s.commands[static_cast<size_t>(fRow)].cmd; // same kind as the selected row
        else
            c.cmd = uze::PC_ENV_SPEED;
        s.commands.insert(s.commands.begin() + (s.commands.empty() ? 0 : at), c);
        setStream(s);
        fRow = s.commands.size() == 1 ? 0 : at;
        fTable.show(fRow, rowCount());
        fHost.editorChanged(true);
    }

    void deleteRow()
    {
        uze::EditStream s = stream();
        if (fRow >= static_cast<int>(s.commands.size())) return;
        pushUndo("delete");
        s.commands.erase(s.commands.begin() + fRow);
        setStream(s);
        fRow = std::min(fRow, rowCount() - 1);
        fHost.editorChanged(true);
    }

    void moveRow(int dir)
    {
        uze::EditStream s = stream();
        const int to = fRow + dir;
        if (fRow >= static_cast<int>(s.commands.size()) || to < 0 || to >= static_cast<int>(s.commands.size())) return;
        pushUndo("move");
        std::swap(s.commands[static_cast<size_t>(fRow)], s.commands[static_cast<size_t>(to)]);
        setStream(s);
        fRow = to;
        fTable.show(fRow, rowCount());
        fHost.editorChanged(true);
    }

    void setCommand(int row, uint8_t cmd)
    {
        uze::EditStream s = stream();
        if (row >= static_cast<int>(s.commands.size()) || s.commands[static_cast<size_t>(row)].cmd == cmd) return;
        pushUndo("command");
        uze::EditCommand& c = s.commands[static_cast<size_t>(row)];
        // keep the displayed value where it makes sense
        const int shown = uze::paramToDisplay(c.cmd, c.param);
        c.cmd = cmd;
        c.param = uze::commandInfo(cmd).hasParam ? uze::displayToParam(cmd, shown) : 0;
        if (cmd == uze::PC_NOISE_PARAMS && c.param == 0) c.param = 1;
        setStream(s);
        fHost.editorChanged(true);
    }

    void addToBank(uze::Patch p)
    {
        fBank.patches.push_back(std::move(p));
        fBank.table.push_back(static_cast<int>(fBank.patches.size()) - 1);
        fBank.error.clear();
        select(static_cast<int>(fBank.patches.size()) - 1);
        fHost.editorChanged(true);
    }

    void newPatch()
    {
        pushUndo("new");
        uze::Patch p;
        int n = static_cast<int>(fBank.table.size());
        char base[32];
        std::snprintf(base, sizeof(base), "patch%02d", n);
        p.name = uze::uniqueName(fBank, base);
        p.cmdName = p.name;
        p.data.type = uze::PatchType::Wave;
        // a simple starting point: square wave with a short decay
        uze::EditStream s;
        s.commands.push_back({ 0, uze::PC_WAVE, 4 });
        s.commands.push_back({ 0, uze::PC_ENV_SPEED, static_cast<uint8_t>(-8) });
        p.data.stream = uze::encodeStream(s);
        addToBank(p);
    }

    void copyPatch()
    {
        const uze::Patch* src = patch();
        if (src == nullptr) return;
        pushUndo("copy");
        uze::Patch p = *src;
        p.name = uze::uniqueName(fBank, src->name + "_copy");
        if (p.data.type == uze::PatchType::Pcm)
        {
            // share the sample data under the same array, new command stream
            p.cmdName = uze::uniqueName(fBank, p.name + "_cmd");
            p.name = uze::uniqueName(fBank, src->name + "_copy");
        }
        else
        {
            p.cmdName = p.name;
        }
        addToBank(p);
    }

    void deletePatch()
    {
        const int i = patchIndex();
        if (i < 0) return;
        pushUndo("delete patch");
        fBank.patches.erase(fBank.patches.begin() + i);
        // keep other patch numbers stable: the slot becomes empty, unless it was the last
        for (size_t n = 0; n < fBank.table.size(); ++n)
        {
            if (fBank.table[n] == i) fBank.table[n] = -1;
            else if (fBank.table[n] > i) --fBank.table[n];
        }
        while (!fBank.table.empty() && fBank.table.back() < 0)
            fBank.table.pop_back();
        if (fBank.patches.empty())
            fSelected.clear();
        else
            fSelected = fBank.patches[static_cast<size_t>(std::min(i, static_cast<int>(fBank.patches.size()) - 1))].name;
        fRow = 0;
        fHost.editorSelected(fSelected);
        fHost.editorChanged(true);
    }

    // ---- drawing helpers -------------------------------------------------------

    static void arrow(NanoVG& vg, float x, float y)
    {
        vg.beginPath();
        vg.moveTo(x - 5, y - 3);
        vg.lineTo(x + 5, y - 3);
        vg.lineTo(x, y + 3);
        vg.closePath();
        vg.fillColor(Color(200, 204, 214));
        vg.fill();
    }

    bool isFocused(F f, int row) const { return fFocus == f && (fFocusRow == row || f == F::Name || f == F::Comment); }

    void numberField(NanoVG& vg, const Box& b, F f, int row, const std::string& shownValue, bool valid = true)
    {
        const bool focused = isFocused(f, row);
        const bool dragging = fDrag.active && fDrag.moved && fDrag.field == f && fDrag.row == row;
        const std::string text = (focused && !fBuffer.empty()) ? fBuffer : shownValue;
        gui::field(vg, b, text, focused || dragging, b.contains(fMouseX, fMouseY), valid, focused,
                   NanoVG::ALIGN_RIGHT);
    }

    // empty if the PCM loop points make sense
    static std::string loopProblem(const uze::Patch& p)
    {
        if (p.data.type != uze::PatchType::Pcm || p.sampleMissing) return std::string();
        if (p.data.loopStart > p.data.loopEnd) return "Loop start is after loop end";
        if (p.data.loopEnd > static_cast<int32_t>(p.data.sample.size()))
            return "Loop end is past the end of the sample (" + std::to_string(p.data.sample.size()) + " bytes)";
        return std::string();
    }

    void drawPatchFields(NanoVG& vg, const uze::Patch& p)
    {
        gui::label(vg, 16, kName.cy(), "Name", gui::colour::dim(), 13);
        const bool nameFocus = fFocus == F::Name;
        gui::field(vg, kName, nameFocus ? fBuffer : p.name, nameFocus, kName.contains(fMouseX, fMouseY),
                   !nameFocus || fBuffer.empty() || uze::isValidIdentifier(fBuffer), nameFocus);

        gui::label(vg, 266, kComment.cy(), "Comment", gui::colour::dim(), 13);
        const bool comFocus = fFocus == F::Comment;
        gui::field(vg, kComment, comFocus ? fBuffer : p.comment, comFocus, kComment.contains(fMouseX, fMouseY),
                   true, comFocus);

        gui::label(vg, 16, kChannel.cy(), "Channel", gui::colour::dim(), 13);
        if (p.data.type == uze::PatchType::Pcm)
        {
            char buf[64];
            if (p.sampleMissing)
                std::snprintf(buf, sizeof(buf), "PCM - sample missing");
            else
                std::snprintf(buf, sizeof(buf), "PCM, %u-byte sample", static_cast<unsigned>(p.data.sample.size()));
            gui::label(vg, kChannel.x, kChannel.cy(), buf, p.sampleMissing ? gui::colour::error() : gui::colour::pcm(), 13);
            gui::label(vg, kLoopStart.x - 6, kLoopStart.cy(), "Loop", gui::colour::dim(), 13,
                       NanoVG::ALIGN_RIGHT | NanoVG::ALIGN_MIDDLE);
            const bool loopOk = loopProblem(p).empty();
            numberField(vg, kLoopStart, F::LoopStart, 0, std::to_string(p.data.loopStart), loopOk);
            numberField(vg, kLoopEnd, F::LoopEnd, 0, std::to_string(p.data.loopEnd), loopOk);
        }
        else
        {
            static const char* const names[2] = { "Wave", "Noise" };
            gui::segments(vg, kChannel, names, 2, p.data.type == uze::PatchType::Noise ? 1 : 0, 13);
        }
    }

    void drawTable(NanoVG& vg, const uze::Patch& p)
    {
        const uze::EditStream s = uze::decodeStream(p.data.stream);
        const int n = static_cast<int>(s.commands.size());
        const float hx = fTable.area.x + 3;
        const float hy = fTable.area.y - 8;
        gui::label(vg, hx + 28, hy, "Wait", gui::colour::faint(), 11);
        gui::label(vg, hx + 80, hy, "Command", gui::colour::faint(), 11);
        gui::label(vg, hx + 236, hy, "Value", gui::colour::faint(), 11);
        gui::label(vg, hx + 300, hy, "Meaning", gui::colour::faint(), 11);

        fTable.draw(vg, n + 1, fRow, [&](int i, const Box& r) {
            const bool end = i == n;
            gui::label(vg, r.x + 22, r.cy(), end ? "" : std::to_string(i + 1), gui::colour::faint(), 11,
                       NanoVG::ALIGN_RIGHT | NanoVG::ALIGN_MIDDLE);
            const F df = end ? F::EndDelta : F::Delta;
            numberField(vg, cellDelta(r), df, i, std::to_string(end ? s.endDelta : s.commands[static_cast<size_t>(i)].delta));
            if (end)
            {
                gui::label(vg, cellCommand(r).x + 6, r.cy(), "End of patch", gui::colour::dim(), 13);
                return;
            }
            const uze::EditCommand& c = s.commands[static_cast<size_t>(i)];
            const uze::CommandInfo& info = uze::commandInfo(c.cmd);
            const Box cb = cellCommand(r);
            gui::fillBox(vg, cb, cb.contains(fMouseX, fMouseY) && !fCmdPopupOpen ? gui::colour::hover() : gui::colour::control(), 4);
            gui::clippedLabel(vg, { cb.x, cb.y, cb.w - 16, cb.h }, cb.x + 6, cb.cy(),
                              c.cmd < uze::kNumCommands ? info.label : "Unknown " + std::to_string(c.cmd),
                              gui::colour::text(), 13);
            arrow(vg, cb.x + cb.w - 9, cb.cy());
            if (info.hasParam)
            {
                numberField(vg, cellParam(r), F::Param, i, std::to_string(uze::paramToDisplay(c.cmd, c.param)));
                gui::clippedLabel(vg, { r.x + 298, r.y, r.w - 298, r.h }, r.x + 300, r.cy(),
                                  uze::describeParam(c.cmd, c.param, waveCount), gui::colour::dim(), 12);
            }
        }, false);
    }

    void drawStatus(NanoVG& vg)
    {
        const float y = H - 16;
        if (!fMessage.empty())
        {
            gui::label(vg, 16, y, fMessage, fMessageIsError ? gui::colour::error() : gui::colour::dim(), 12);
            return;
        }
        const uze::Patch* p = patch();
        if (p == nullptr) return;
        const std::string loop = loopProblem(*p);
        if (!loop.empty())
        {
            gui::label(vg, 16, y, loop, gui::colour::error(), 12);
            return;
        }
        const uze::EditStream s = stream();
        std::string help;
        if (fRow < static_cast<int>(s.commands.size()))
            help = uze::commandInfo(s.commands[static_cast<size_t>(fRow)].cmd).help;
        else
            help = "Frames to wait before the patch ends";
        if (fFocus != F::None && fFocus != F::Name && fFocus != F::Comment)
            help = "Type a value, Enter to set, Esc to cancel";
        gui::clippedLabel(vg, { 16, y - 10, 330, 20 }, 16, y, help, gui::colour::dim(), 12);

        const int frames = uze::streamLengthFrames(s);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d frames (%.2f s), %u bytes", frames,
                      frames / (uze::kNativeSampleRate / uze::kSamplesPerFrame),
                      static_cast<unsigned>(p->data.stream.size()));
        gui::label(vg, W - 16, y, buf, gui::colour::faint(), 12, NanoVG::ALIGN_RIGHT | NanoVG::ALIGN_MIDDLE);
    }

    void drawCommandPopup(NanoVG& vg)
    {
        const uze::EditStream s = stream();
        const int current = fCmdRow < static_cast<int>(s.commands.size()) ? s.commands[static_cast<size_t>(fCmdRow)].cmd : -1;
        fCmdPopup.draw(vg, uze::kNumCommands, current, [&](int i, const Box& r) {
            const uze::CommandInfo& info = uze::commandInfo(static_cast<uint8_t>(i));
            gui::label(vg, r.x + 8, r.cy(), info.label, gui::colour::text(), 13);
            gui::label(vg, r.x + r.w - 6, r.cy(), info.id, gui::colour::faint(), 10,
                       NanoVG::ALIGN_RIGHT | NanoVG::ALIGN_MIDDLE);
        }, true);
    }

    void drawPatchPopup(NanoVG& vg)
    {
        const int count = static_cast<int>(fBank.patches.size());
        fPatchPopup.draw(vg, count, patchIndex(), [&](int i, const Box& r) {
            const uze::Patch& p = fBank.patches[static_cast<size_t>(i)];
            const Color c = p.data.type == uze::PatchType::Noise ? gui::colour::noise()
                          : p.data.type == uze::PatchType::Pcm ? gui::colour::pcm() : gui::colour::text();
            gui::label(vg, r.x + 8, r.cy(), numberedName(p), c, 13);
        }, true);
    }
};

END_NAMESPACE_DISTRHO
