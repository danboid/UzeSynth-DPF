/*
 * UzeSynth - small immediate-mode widgets drawn with DPF's NanoVG
 * This file is part of UzeSynth, licensed under the GNU GPL v3 or later.
 */
#pragma once

#include "NanoVG.hpp"

#include <algorithm>
#include <cmath>
#include <string>

START_NAMESPACE_DGL

namespace gui {

struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
};

namespace colour {
inline Color bg()       { return Color(24, 26, 32); }
inline Color header()   { return Color(36, 40, 52); }
inline Color control()  { return Color(44, 48, 62); }
inline Color hover()    { return Color(62, 68, 88); }
inline Color border()   { return Color(70, 76, 96); }
inline Color accent()   { return Color(255, 200, 60); }
inline Color text()     { return Color(225, 228, 236); }
inline Color dim()      { return Color(150, 156, 170); }
inline Color faint()    { return Color(100, 106, 120); }
inline Color error()    { return Color(255, 110, 100); }
inline Color noise()    { return Color(140, 200, 255); }
inline Color pcm()      { return Color(150, 230, 160); }
} // namespace colour

inline void fillBox(NanoVG& vg, const Box& b, const Color& c, float radius = 5)
{
    vg.beginPath();
    vg.roundedRect(b.x, b.y, b.w, b.h, radius);
    vg.fillColor(c);
    vg.fill();
}

inline void strokeBox(NanoVG& vg, const Box& b, const Color& c, float radius = 5)
{
    vg.beginPath();
    vg.roundedRect(b.x + 0.5f, b.y + 0.5f, b.w - 1, b.h - 1, radius);
    vg.strokeColor(c);
    vg.strokeWidth(1.0f);
    vg.stroke();
}

inline void label(NanoVG& vg, float x, float y, const std::string& s, const Color& c, float size = 13.0f,
                  int align = NanoVG::ALIGN_LEFT | NanoVG::ALIGN_MIDDLE)
{
    if (s.empty())
        return;
    vg.fontSize(size);
    vg.textAlign(align);
    vg.fillColor(c);
    vg.text(x, y, s.c_str(), nullptr);
}

// text clipped to a box
inline void clippedLabel(NanoVG& vg, const Box& clip, float x, float y, const std::string& s, const Color& c,
                         float size = 13.0f, int align = NanoVG::ALIGN_LEFT | NanoVG::ALIGN_MIDDLE)
{
    vg.save();
    vg.intersectScissor(clip.x, clip.y, clip.w, clip.h);
    label(vg, x, y, s, c, size, align);
    vg.restore();
}

inline void button(NanoVG& vg, const Box& b, const std::string& text, bool hover, bool enabled = true,
                   bool pressed = false, float size = 12.0f)
{
    fillBox(vg, b, pressed ? colour::accent() : (hover && enabled ? colour::hover() : colour::control()), 4);
    label(vg, b.cx(), b.cy(), text,
          pressed ? colour::bg() : (enabled ? Color(205, 209, 219) : colour::faint()),
          size, NanoVG::ALIGN_CENTER | NanoVG::ALIGN_MIDDLE);
}

inline void segments(NanoVG& vg, const Box& b, const char* const* names, int count, int selected,
                     float size = 13.0f)
{
    const float sw = b.w / count;
    for (int i = 0; i < count; ++i)
    {
        const Box s { b.x + i * sw + 1, b.y, sw - 2, b.h };
        fillBox(vg, s, i == selected ? colour::accent() : colour::control());
        label(vg, s.cx(), s.cy(), names[i], i == selected ? colour::bg() : Color(200, 204, 214), size,
              NanoVG::ALIGN_CENTER | NanoVG::ALIGN_MIDDLE);
    }
}

inline int segmentAt(const Box& b, int count, double x)
{
    const int i = static_cast<int>((x - b.x) / (b.w / count));
    return std::max(0, std::min(count - 1, i));
}

// An editable field (number or text): shows the value, an outline when focused
inline void field(NanoVG& vg, const Box& b, const std::string& text, bool focused, bool hover,
                  bool valid = true, bool caret = false, int align = NanoVG::ALIGN_LEFT)
{
    fillBox(vg, b, focused ? Color(30, 33, 42) : (hover ? colour::hover() : colour::control()), 4);
    if (focused || !valid)
        strokeBox(vg, b, valid ? colour::accent() : colour::error(), 4);
    const float tx = (align & NanoVG::ALIGN_RIGHT) ? b.x + b.w - 6 : b.x + 6;
    std::string shown = text;
    if (caret) shown += "|";
    clippedLabel(vg, b, tx, b.cy(), shown, valid ? colour::text() : colour::error(), 13.0f,
                 align | NanoVG::ALIGN_MIDDLE);
}

// --------------------------------------------------------------------------
// Vertical scrollbar that can be dragged, or clicked above/below the thumb to page

struct Scrollbar {
    bool dragging = false;
    double grab = 0;   // where on the thumb it was grabbed

    static constexpr float kWidth = 12.0f;

    static Box track(const Box& area) { return { area.x + area.w - kWidth - 2, area.y + 3, kWidth, area.h - 6 }; }

    static Box thumb(const Box& area, int count, int visible, int scroll)
    {
        const Box t = track(area);
        if (count <= visible) return t;
        const float h = std::max(20.0f, t.h * visible / count);
        const float y = t.y + (t.h - h) * scroll / static_cast<float>(count - visible);
        return { t.x, y, t.w, h };
    }

    void draw(NanoVG& vg, const Box& area, int count, int visible, int scroll) const
    {
        if (count <= visible) return;
        fillBox(vg, track(area), Color(255, 255, 255, 0.05f), 5);
        fillBox(vg, thumb(area, count, visible, scroll), dragging ? colour::accent() : Color(120, 126, 140), 5);
    }

    bool hit(const Box& area, int count, int visible, double x, double y) const
    {
        return count > visible && track(area).contains(x, y);
    }

    // mouse down on the track; returns true if handled
    bool press(const Box& area, int count, int visible, double x, double y, int& scroll)
    {
        if (!hit(area, count, visible, x, y)) return false;
        const Box th = thumb(area, count, visible, scroll);
        if (th.contains(x, y))
        {
            dragging = true;
            grab = y - th.y;
        }
        else
        {
            scroll += (y < th.y) ? -visible : visible; // page
            scroll = std::max(0, std::min(count - visible, scroll));
        }
        return true;
    }

    // mouse moved while dragging; returns true if scroll changed
    bool motion(const Box& area, int count, int visible, double y, int& scroll) const
    {
        if (!dragging || count <= visible) return false;
        const Box t = track(area);
        const Box th = thumb(area, count, visible, scroll);
        const float range = t.h - th.h;
        if (range <= 0) return false;
        const double pos = (y - grab - t.y) / range;
        const int s = std::max(0, std::min(count - visible, static_cast<int>(std::lround(pos * (count - visible)))));
        if (s == scroll) return false;
        scroll = s;
        return true;
    }

    void release() { dragging = false; }
};

// --------------------------------------------------------------------------
// Scrollable list shown as a popup (drop-down) or embedded

struct ListView {
    Box area;
    float rowH = 22.0f;
    int scroll = 0;
    int hover = -1;
    Scrollbar bar;

    int visible() const { return std::max(1, static_cast<int>((area.h - 8) / rowH)); }
    int clamp(int s, int count) const { return std::max(0, std::min(std::max(0, count - visible()), s)); }
    void show(int row, int count) { if (row < scroll) scroll = row; else if (row >= scroll + visible()) scroll = row - visible() + 1; scroll = clamp(scroll, count); }
    void centre(int row, int count) { scroll = clamp(row - visible() / 2, count); }
    Box rowBox(int row) const { return { area.x + 3, area.y + 4 + (row - scroll) * rowH, area.w - 6 - Scrollbar::kWidth - 2, rowH }; }

    // row under the pointer, or -1 (not over rows, or over the scrollbar)
    int rowAt(double x, double y, int count) const
    {
        if (!area.contains(x, y) || bar.hit(area, count, visible(), x, y)) return -1;
        const int r = static_cast<int>((y - area.y - 4) / rowH) + scroll;
        return (r >= scroll && r < count && r < scroll + visible()) ? r : -1;
    }

    bool scrollBy(int d, int count)
    {
        const int s = clamp(scroll + d, count);
        if (s == scroll) return false;
        scroll = s;
        return true;
    }

    template <class DrawRow>
    void draw(NanoVG& vg, int count, int selected, DrawRow drawRow, bool popup)
    {
        fillBox(vg, area, popup ? Color(32, 35, 45) : Color(30, 32, 40), 6);
        if (popup) strokeBox(vg, area, colour::accent(), 6);
        vg.save();
        vg.intersectScissor(area.x + 2, area.y + 2, area.w - 4, area.h - 4);
        for (int i = scroll; i < count && i < scroll + visible(); ++i)
        {
            const Box r = rowBox(i);
            if (i == selected || i == hover)
                fillBox(vg, r, i == selected ? Color(255, 200, 60, 0.22f) : Color(255, 255, 255, 0.07f), 3);
            drawRow(i, r);
        }
        vg.restore();
        bar.draw(vg, area, count, visible(), scroll);
    }
};

} // namespace gui

END_NAMESPACE_DGL
