#include "controls.h"
#include "app.h"
#include <commctrl.h>
#include <algorithm>

namespace {
const COLORREF kBg = RGB(24, 24, 24);
const COLORREF kHot = RGB(52, 52, 52);
const COLORREF kText = RGB(222, 222, 222);
const COLORREF kDim = RGB(150, 150, 150);
const COLORREF kTrack = RGB(70, 70, 70);
const COLORREF kAccent = RGB(58, 142, 230);
const COLORREF kAb = RGB(240, 190, 60);

void Fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void Poly(HDC dc, const POINT* p, int n, COLORREF c) {
    SetDCBrushColor(dc, c);
    SetDCPenColor(dc, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    Polygon(dc, p, n);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

void Ellipse2(HDC dc, int cx, int cy, int r, COLORREF c) {
    SetDCBrushColor(dc, c);
    SetDCPenColor(dc, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

HFONT MakeFont(int dpi, int pt = 9) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
}  // namespace

// ------------------------------------------------------------------------------------------------ ControlBar

bool ControlBar::Create(HINSTANCE inst, HWND parent, App* app) {
    app_ = app;
    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FastPlay.Bar";
    RegisterClassExW(&wc);
    dpi_ = app->Dpi();
    font_ = MakeFont(dpi_);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                            nullptr, inst, this);
    return hwnd_ != nullptr;
}

void ControlBar::OnDpi(int dpi) {
    dpi_ = dpi;
    if (font_) DeleteObject(font_);
    font_ = MakeFont(dpi_);
    Invalidate();
}

void ControlBar::SetOverlay(bool overlay, HWND owner) {
    if (overlay == overlay_) return;
    overlay_ = overlay;
    ShowWindow(hwnd_, SW_HIDE);
    LONG_PTR st = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    if (overlay) {
        SetParent(hwnd_, nullptr);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (st & ~WS_CHILD) | WS_POPUP);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, (LONG_PTR)owner);  // owner: stays above the fullscreen window
    } else {
        SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, 0);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (st & ~WS_POPUP) | WS_CHILD);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, 0);
        SetParent(hwnd_, owner);
    }
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

LRESULT CALLBACK ControlBar::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ControlBar* self;
    if (m == WM_NCCREATE) {
        self = (ControlBar*)((CREATESTRUCTW*)l)->lpCreateParams;
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (ControlBar*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    return self ? self->On(m, w, l) : DefWindowProcW(h, m, w, l);
}

void ControlBar::LayoutParts(const RECT& rc) {
    int w = rc.right - rc.left;
    seek_ = {S(8), S(2), w - S(8), S(20)};
    int y0 = S(22), y1 = S(48), x = S(6), b = S(26);
    play_ = {x, y0, x + b, y1}; x += b + S(2);
    stop_ = {x, y0, x + b, y1}; x += b + S(2);
    prev_ = {x, y0, x + b, y1}; x += b + S(2);
    next_ = {x, y0, x + b, y1}; x += b + S(2);
    step_ = {x, y0, x + b, y1}; x += b + S(10);
    time_ = {x, y0, x + S(170), y1};
    vol_ = {w - S(10) - S(84), y0, w - S(10), y1};
    mute_ = {vol_.left - S(4) - b, y0, vol_.left - S(4), y1};
    if (time_.right > mute_.left - S(60)) time_.right = std::max(time_.left, (LONG)(mute_.left - S(60)));
}

ControlBar::Part ControlBar::HitTest(POINT p) const {
    auto in = [&](const RECT& r) { return PtInRect(&r, p) != 0; };
    if (in(seek_)) return Part::Seek;
    if (in(play_)) return Part::Play;
    if (in(stop_)) return Part::Stop;
    if (in(prev_)) return Part::Prev;
    if (in(next_)) return Part::Next;
    if (in(step_)) return Part::Step;
    if (in(time_)) return Part::Time;
    if (in(mute_)) return Part::Mute;
    if (in(vol_)) return Part::Volume;
    return Part::None;
}

double ControlBar::SeekTimeAt(int x) const {
    const BarState& b = app_->Bar();
    int l = seek_.left + S(4), r = seek_.right - S(4);
    if (r <= l || b.dur <= 0) return -1;
    double f = std::clamp((double)(x - l) / (r - l), 0.0, 1.0);
    return f * b.dur;
}

int ControlBar::VolumeAt(int x) const {
    int l = vol_.left + S(4), r = vol_.right - S(4);
    return (int)std::lround(std::clamp((double)(x - l) / std::max(1, r - l), 0.0, 1.0) * app_->Bar().volumeMax);
}

LRESULT ControlBar::On(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        LayoutParts(rc);
        UpdateTipRects();
        Invalidate();
        return 0;
    }
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)l;
        if (tip_ && nm->hwndFrom == tip_ && nm->code == TTN_GETDISPINFOW) {
            auto* di = (NMTTDISPINFOW*)l;
            static std::wstring text;  // must outlive the notification
            text = TipText((int)di->hdr.idFrom);
            di->lpszText = text.data();
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        app_->NoteMouseActivity();
        EnsureTips();
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&t);
        Part hp = dragging_ != Part::None ? dragging_ : HitTest(p);
        int hx = hp == Part::Seek ? p.x : -1;
        if (hp != hot_ || hx != hoverX_) {
            hot_ = hp;
            hoverX_ = hx;
            Invalidate();
        }
        if (dragging_ == Part::Seek) {
            double t = SeekTimeAt(p.x);
            DWORD now = GetTickCount();
            // Keyframe seeks while dragging (fast scrubbing); throttled so mpv isn't flooded on slow disks.
            if (t >= 0 && t != lastDragSeek_ && now - lastDragTick_ >= 30) {
                app_->SeekTo(t, false);
                lastDragSeek_ = t;
                lastDragTick_ = now;
            }
        } else if (dragging_ == Part::Volume) {
            app_->SetVolume(VolumeAt(p.x));
        }
        if (hp == Part::Seek) {
            POINT a = {p.x, seek_.top};
            ClientToScreen(hwnd_, &a);
            app_->HoverSeek(SeekTimeAt(p.x), a);
        } else {
            app_->HoverSeek(-1, {});
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (dragging_ == Part::None) {
            hot_ = Part::None;
            hoverX_ = -1;
            app_->HoverSeek(-1, {});
            Invalidate();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        Part hp = HitTest(p);
        switch (hp) {
        case Part::Seek: {
            double t = SeekTimeAt(p.x);
            if (t >= 0) {
                dragging_ = Part::Seek;
                SetCapture(hwnd_);
                app_->SeekTo(t, false);
                lastDragSeek_ = t;
                lastDragTick_ = GetTickCount();
            }
            break;
        }
        case Part::Volume:
            dragging_ = Part::Volume;
            SetCapture(hwnd_);
            app_->SetVolume(VolumeAt(p.x));
            break;
        case Part::Play: app_->DoAction(Act::PlayPause); break;
        case Part::Stop: app_->DoAction(Act::Stop); break;
        case Part::Prev: app_->DoAction(Act::PrevFile); break;
        case Part::Next: app_->DoAction(Act::NextFile); break;
        case Part::Step: app_->DoAction(Act::FrameStep); break;
        case Part::Time: app_->ToggleRemaining(); break;
        case Part::Mute: app_->DoAction(Act::Mute); break;
        default: break;
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (dragging_ == Part::Seek) {
            double t = SeekTimeAt(p.x);
            if (t >= 0) app_->SeekTo(t, true);  // land exactly where released
        }
        dragging_ = Part::None;
        ReleaseCapture();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (!util::KeepDragOnCaptureLoss()) dragging_ = Part::None;
        return 0;
    case WM_MOUSEWHEEL:
        app_->DoAction(GET_WHEEL_DELTA_WPARAM(w) > 0 ? Act::VolUp : Act::VolDown);
        return 0;
    case WM_RBUTTONUP: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        ClientToScreen(hwnd_, &p);
        app_->ShowContextMenu(p);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return overlay_ ? MA_NOACTIVATE : MA_ACTIVATE;
    case WM_DESTROY:
        if (font_) DeleteObject(font_);
        font_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void ControlBar::EnsureTips() {
    if (tip_) return;
    INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&ic);
    tip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                           0, 0, 0, 0, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tip_) return;
    const RECT* rects[] = {&play_, &stop_, &prev_, &next_, &step_, &time_, &mute_, &vol_};
    const Part parts[] = {Part::Play, Part::Stop, Part::Prev, Part::Next, Part::Step, Part::Time, Part::Mute, Part::Volume};
    for (int i = 0; i < 8; i++) {
        TTTOOLINFOW ti = {sizeof ti};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = (UINT_PTR)parts[i];
        ti.rect = *rects[i];
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

void ControlBar::UpdateTipRects() {
    if (!tip_) return;
    const RECT* rects[] = {&play_, &stop_, &prev_, &next_, &step_, &time_, &mute_, &vol_};
    const Part parts[] = {Part::Play, Part::Stop, Part::Prev, Part::Next, Part::Step, Part::Time, Part::Mute, Part::Volume};
    for (int i = 0; i < 8; i++) {
        TTTOOLINFOW ti = {sizeof ti};
        ti.hwnd = hwnd_;
        ti.uId = (UINT_PTR)parts[i];
        ti.rect = *rects[i];
        SendMessageW(tip_, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

std::wstring ControlBar::TipText(int part) {
    auto with = [&](const wchar_t* label, Act a) {
        std::wstring k = app_->ShortcutText(a);
        return k.empty() ? std::wstring(label) : std::wstring(label) + L"  (" + k + L")";
    };
    switch ((Part)part) {
    case Part::Play: return with(app_->Bar().paused ? L"Play" : L"Pause", Act::PlayPause);
    case Part::Stop: return with(L"Stop", Act::Stop);
    case Part::Prev: return with(L"Previous file", Act::PrevFile);
    case Part::Next: return with(L"Next file", Act::NextFile);
    case Part::Step: return with(L"Frame step", Act::FrameStep);
    case Part::Time: return L"Click: elapsed / remaining";
    case Part::Mute: return with(app_->Bar().mute ? L"Unmute" : L"Mute", Act::Mute);
    case Part::Volume: return L"Volume " + std::to_wstring(app_->Bar().volume) + L"%  (wheel, Up/Down)";
    default: return L"";
    }
}

bool ControlBar::PosChangesPixels(double a, double b) const {
    const BarState& s = app_->Bar();
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return false;
    if (s.dur > 0) {
        int l = seek_.left + S(4), r = seek_.right - S(4);
        auto xAt = [&](double t) { return l + (int)((r - l) * std::clamp(t / s.dur, 0.0, 1.0)); };
        if (xAt(a) != xAt(b)) return true;
    }
    auto text = [&](double t) { return s.remaining ? util::FormatTime(std::max(0.0, s.dur - t)) : util::FormatTime(t); };
    return text(a) != text(b);
}

void ControlBar::Paint(HDC dc, const RECT& rc) {
    const BarState& b = app_->Bar();
    Fill(dc, rc, kBg);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font_);

    // Seekbar
    int l = seek_.left + S(4), r = seek_.right - S(4);
    int cy = (seek_.top + seek_.bottom) / 2;
    int th = hot_ == Part::Seek || dragging_ == Part::Seek ? S(6) : S(4);
    RECT track = {l, cy - th / 2, r, cy - th / 2 + th};
    Fill(dc, track, kTrack);
    auto xAt = [&](double t) { return b.dur > 0 ? l + (int)((r - l) * std::clamp(t / b.dur, 0.0, 1.0)) : l; };
    if (b.dur > 0 && b.abA >= 0) {
        int xa = xAt(b.abA), xb = b.abB >= 0 ? xAt(b.abB) : r;
        RECT ab = {xa, track.top - S(2), std::max(xa + 1, xb), track.bottom + S(2)};
        Fill(dc, ab, RGB(90, 75, 30));
    }
    if (b.hasFile && b.dur > 0) {
        RECT played = {l, track.top, xAt(b.pos), track.bottom};
        Fill(dc, played, kAccent);
    }
    for (double c : b.chapters) {
        RECT tick = {xAt(c), track.top - S(3), xAt(c) + std::max(1, S(1)), track.top};
        Fill(dc, tick, kDim);
    }
    if (b.abA >= 0) { RECT m1 = {xAt(b.abA), seek_.top, xAt(b.abA) + S(2), seek_.bottom}; Fill(dc, m1, kAb); }
    if (b.abB >= 0) { RECT m2 = {xAt(b.abB), seek_.top, xAt(b.abB) + S(2), seek_.bottom}; Fill(dc, m2, kAb); }
    if (b.hasFile && b.dur > 0) Ellipse2(dc, xAt(b.pos), cy, hot_ == Part::Seek ? S(6) : S(5), kText);
    if (hoverX_ >= 0) {
        RECT hl = {hoverX_, track.top - S(3), hoverX_ + std::max(1, S(1)), track.bottom + S(3)};
        Fill(dc, hl, kText);
    }

    // Buttons
    auto btnBg = [&](const RECT& br, Part p) { if (hot_ == p) Fill(dc, br, kHot); };
    btnBg(play_, Part::Play); btnBg(stop_, Part::Stop); btnBg(prev_, Part::Prev); btnBg(next_, Part::Next);
    btnBg(step_, Part::Step); btnBg(mute_, Part::Mute);
    auto center = [](const RECT& br) { return POINT{(br.left + br.right) / 2, (br.top + br.bottom) / 2}; };
    int u = S(6);
    {   // play / pause
        POINT c = center(play_);
        if (b.paused || !b.hasFile) {
            POINT t[] = {{c.x - u + S(1), c.y - u}, {c.x - u + S(1), c.y + u}, {c.x + u, c.y}};
            Poly(dc, t, 3, kText);
        } else {
            RECT a = {c.x - u + S(1), c.y - u, c.x - S(1), c.y + u}, bb = {c.x + S(2), c.y - u, c.x + u + S(1), c.y + u};
            Fill(dc, a, kText);
            Fill(dc, bb, kText);
        }
    }
    {   POINT c = center(stop_); RECT q = {c.x - S(5), c.y - S(5), c.x + S(5), c.y + S(5)}; Fill(dc, q, kText); }
    {   POINT c = center(prev_);
        RECT bar = {c.x - u, c.y - u, c.x - u + S(2), c.y + u}; Fill(dc, bar, kText);
        POINT t[] = {{c.x + u, c.y - u}, {c.x + u, c.y + u}, {c.x - u + S(2), c.y}}; Poly(dc, t, 3, kText); }
    {   POINT c = center(next_);
        RECT bar = {c.x + u - S(2), c.y - u, c.x + u, c.y + u}; Fill(dc, bar, kText);
        POINT t[] = {{c.x - u, c.y - u}, {c.x - u, c.y + u}, {c.x + u - S(2), c.y}}; Poly(dc, t, 3, kText); }
    {   POINT c = center(step_);
        POINT t[] = {{c.x - u, c.y - u}, {c.x - u, c.y + u}, {c.x + S(2), c.y}}; Poly(dc, t, 3, kText);
        RECT bar = {c.x + S(3), c.y - u, c.x + S(5), c.y + u}; Fill(dc, bar, kText); }

    // Time
    SetTextColor(dc, kText);
    std::wstring tt;
    if (b.hasFile) {
        tt = b.remaining ? L"-" + util::FormatTime(std::max(0.0, b.dur - b.pos)) : util::FormatTime(b.pos);
        tt += L" / " + util::FormatTime(b.dur);
    }
    RECT trc = time_;
    DrawTextW(dc, tt.c_str(), -1, &trc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    // Speed / loop indicators, right-aligned before the mute button
    std::wstring ind;
    if (b.speed != 1.0) {
        wchar_t sp[32];
        swprintf(sp, 32, L"%.2g×", b.speed);
        ind += sp;
    }
    if (b.abA >= 0) ind += ind.empty() ? L"A-B" : L"  A-B";
    if (!ind.empty()) {
        RECT irc = {time_.right, mute_.top, mute_.left - S(6), mute_.bottom};
        SetTextColor(dc, kAb);
        DrawTextW(dc, ind.c_str(), -1, &irc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Mute icon (speaker; crossed when muted)
    {
        POINT c = center(mute_);
        COLORREF col = b.mute ? kDim : kText;
        RECT box = {c.x - S(7), c.y - S(3), c.x - S(3), c.y + S(3)};
        Fill(dc, box, col);
        POINT cone[] = {{c.x - S(3), c.y - S(3)}, {c.x + S(2), c.y - S(7)}, {c.x + S(2), c.y + S(7)}, {c.x - S(3), c.y + S(3)}};
        Poly(dc, cone, 4, col);
        if (b.mute) {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), RGB(220, 80, 80));
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, c.x + S(4), c.y - S(4), nullptr);
            LineTo(dc, c.x + S(10), c.y + S(4));
            MoveToEx(dc, c.x + S(10), c.y - S(4), nullptr);
            LineTo(dc, c.x + S(4), c.y + S(4));
            SelectObject(dc, op);
            DeleteObject(pen);
        }
    }

    // Volume slider
    {
        int vl = vol_.left + S(4), vr = vol_.right - S(4), vy = (vol_.top + vol_.bottom) / 2;
        RECT vt = {vl, vy - S(2), vr, vy + S(2)};
        Fill(dc, vt, kTrack);
        int vmax = std::max(1, b.volumeMax);
        int vx = vl + (vr - vl) * std::clamp(b.volume, 0, vmax) / vmax;
        RECT vf = {vl, vy - S(2), vx, vy + S(2)};
        Fill(dc, vf, b.mute ? kDim : kAccent);
        Ellipse2(dc, vx, vy, S(5), kText);
    }
    SelectObject(dc, oldFont);
}

// ------------------------------------------------------------------------------------------------ ThumbPopup

bool ThumbPopup::Create(HINSTANCE inst, HWND owner) {
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FastPlay.Thumb";
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"", WS_POPUP,
                            0, 0, 10, 10, owner, nullptr, inst, this);
    return hwnd_ != nullptr;
}

void ThumbPopup::Show(std::shared_ptr<const Thumb> t, double time, POINT anchor, int dpi) {
    if (dpi != dpi_ || !font_) {
        dpi_ = dpi;
        if (font_) DeleteObject(font_);
        font_ = MakeFont(dpi_);
    }
    thumb_ = std::move(t);
    time_ = time;
    int pad = MulDiv(2, dpi_, 96), label = MulDiv(20, dpi_, 96);
    int w = thumb_ ? thumb_->w + 2 * pad : MulDiv(80, dpi_, 96);
    int h = (thumb_ ? thumb_->h + pad : 0) + label;
    int x = anchor.x - w / 2, y = anchor.y - h - MulDiv(6, dpi_, 96);
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    x = std::clamp(x, (int)mi.rcMonitor.left, (int)mi.rcMonitor.right - w);
    y = std::max(y, (int)mi.rcMonitor.top);
    SetWindowPos(hwnd_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ThumbPopup::Hide() {
    if (!visible_) return;
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    thumb_.reset();
}

LRESULT CALLBACK ThumbPopup::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ThumbPopup* self;
    if (m == WM_NCCREATE) {
        self = (ThumbPopup*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (ThumbPopup*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        Fill(dc, rc, RGB(16, 16, 16));
        int pad = MulDiv(2, self->dpi_, 96);
        int labelTop = pad;
        if (auto& t = self->thumb_) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = t->w;
            bi.bmiHeader.biHeight = -t->h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(dc, pad, pad, t->w, t->h, 0, 0, 0, t->h, t->px.data(), &bi, DIB_RGB_COLORS);
            labelTop = pad + t->h;
        }
        RECT lr = {0, labelTop, rc.right, rc.bottom};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        HGDIOBJ of = SelectObject(dc, self->font_);
        std::wstring s = util::FormatTime(self->time_);
        DrawTextW(dc, s.c_str(), -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (self->font_) DeleteObject(self->font_);
        self->font_ = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
