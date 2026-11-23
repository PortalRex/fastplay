// Owner-drawn control bar (seekbar, buttons, time, volume) and the seekbar thumbnail popup. Plain GDI, double-buffered.
#pragma once
#include "util.h"
#include "thumbs.h"
#include <memory>

class App;

class ControlBar {
public:
    bool Create(HINSTANCE inst, HWND parent, App* app);
    HWND Hwnd() const { return hwnd_; }
    static int HeightFor(int dpi) { return MulDiv(50, dpi, 96); }
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    void OnDpi(int dpi);
    // Becomes a top-level overlay (fullscreen) or a child at the bottom of the window.
    void SetOverlay(bool overlay, HWND owner);
    bool Overlay() const { return overlay_; }
    // Would moving the playhead from a to b change any pixel (seekbar fill or time text)? Playback ticks repaint only
    // then (about once a second) instead of on every 100 ms tick.
    bool PosChangesPixels(double a, double b) const;
    bool MouseInside() const { return hot_ != Part::None || dragging_ != Part::None; }

private:
    enum class Part { None, Seek, Play, Stop, Prev, Next, Step, Time, Mute, Volume };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT On(UINT m, WPARAM w, LPARAM l);
    void Paint(HDC dc, const RECT& rc);
    void LayoutParts(const RECT& rc);
    Part HitTest(POINT p) const;
    double SeekTimeAt(int x) const;
    int VolumeAt(int x) const;
    int S(int v) const { return MulDiv(v, dpi_, 96); }

    HWND hwnd_ = nullptr;
    App* app_ = nullptr;
    int dpi_ = 96;
    HFONT font_ = nullptr;
    HWND tip_ = nullptr;          // button tooltips, created on first hover (keeps comctl32 off the start-up path)
    void EnsureTips();
    void UpdateTipRects();
    std::wstring TipText(int part);
    bool overlay_ = false;
    Part hot_ = Part::None, dragging_ = Part::None;
    int hoverX_ = -1;
    RECT seek_ = {}, play_ = {}, stop_ = {}, prev_ = {}, next_ = {}, step_ = {}, time_ = {}, mute_ = {}, vol_ = {};
    double lastDragSeek_ = -1;
    DWORD lastDragTick_ = 0;
};

class ThumbPopup {
public:
    bool Create(HINSTANCE inst, HWND owner);
    // Shows thumbnail (may be null: time label only) centered above anchor (screen coords, top of seekbar).
    void Show(std::shared_ptr<const Thumb> t, double time, POINT anchor, int dpi);
    void Hide();
    bool Visible() const { return visible_; }

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    HWND hwnd_ = nullptr;
    std::shared_ptr<const Thumb> thumb_;
    double time_ = 0;
    int dpi_ = 96;
    HFONT font_ = nullptr;
    bool visible_ = false;
};
