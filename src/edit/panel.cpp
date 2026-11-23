#include "panel.h"
#include "segcards.h"
#include <commctrl.h>
#include <algorithm>
#include <map>

namespace edit {

namespace {
const uint32_t kNone = 0xFFFFFFFF;

void Fill(HDC dc, const RECT& r, uint32_t c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void RoundFill(HDC dc, const RECT& r, int radius, uint32_t c) {
    SetDCBrushColor(dc, c);
    SetDCPenColor(dc, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

uint32_t Mix(uint32_t a, uint32_t b, double t) {
    auto ch = [&](int s) { return (uint32_t)std::lround(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t) << s; };
    return ch(0) | ch(8) | ch(16);
}
}  // namespace

HFONT IconFont(int dpi, bool large) {
    static std::map<int, HFONT> cache;
    int key = dpi * 2 + (large ? 1 : 0);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    HFONT f = CreateFontW(-MulDiv(large ? 14 : 11, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    cache[key] = f;
    return f;
}

void EnableDarkMenus() {
    static bool done = false;
    if (done) return;
    done = true;
    // uxtheme ordinal 135 = SetPreferredAppMode (Windows 10 1903+), 136 = FlushMenuThemes. Undocumented but stable;
    // without them popup menus stay light.
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetMode = int(WINAPI*)(int);
        using Flush = void(WINAPI*)();
        if (auto set = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135))) set(1 /* AllowDark */);
        if (auto flush = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136))) flush();
    }
}

void DarkTitleBar(HWND h, bool on) {
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static SetAttr set = [] {
        HMODULE m = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? (SetAttr)GetProcAddress(m, "DwmSetWindowAttribute") : nullptr;
    }();
    BOOL v = on;
    if (set) set(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof v);
}

bool Panel::Create(HINSTANCE inst, HWND parent, uint32_t background) {
    bg_ = background;
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FastPlay.EditPanel";
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                            nullptr, inst, this);
    editBrush_ = CreateSolidBrush(theme::Card);
    return hwnd_ != nullptr;
}

Panel::~Panel() {
    if (editBrush_) DeleteObject(editBrush_);
    if (hwnd_) DestroyWindow(hwnd_);
}

void Panel::SetDpi(int dpi) {
    dpi_ = dpi;
    Relayout();
}

void Panel::Relayout() {
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (layout) layout(*this, rc);
    Invalidate();
}

void Panel::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int Panel::TextWidth(const std::wstring& g, const std::wstring& text, bool bold, bool largeIcon) const {
    HDC dc = GetDC(hwnd_);
    int w = 0;
    SIZE sz;
    if (!g.empty()) {
        HGDIOBJ o = SelectObject(dc, IconFont(dpi_, largeIcon));
        GetTextExtentPoint32W(dc, g.c_str(), (int)g.size(), &sz);
        w += sz.cx;
        SelectObject(dc, o);
    }
    if (!text.empty()) {
        HGDIOBJ o = SelectObject(dc, theme::UiFont(dpi_, bold));
        GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
        w += sz.cx + (g.empty() ? 0 : S(6));
        SelectObject(dc, o);
    }
    ReleaseDC(hwnd_, dc);
    return w;
}

int Panel::ButtonWidth(const PanelItem& it) const {
    int w = TextWidth(it.glyph, it.text, it.bold, it.largeIcon) + S(it.text.empty() ? 16 : 20);
    if (it.dropdown) w += S(12);
    return std::max(w, S(28));
}

RECT Panel::ItemScreenRect(const PanelItem& it) const {
    RECT r = it.rc;
    MapWindowPoints(hwnd_, nullptr, (POINT*)&r, 2);
    return r;
}

LRESULT CALLBACK Panel::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Panel* self;
    if (m == WM_NCCREATE) {
        self = (Panel*)((CREATESTRUCTW*)l)->lpCreateParams;
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (Panel*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    return self ? self->On(m, w, l) : DefWindowProcW(h, m, w, l);
}

int Panel::HitTest(POINT p) const {
    for (int i = (int)items.size() - 1; i >= 0; i--) {
        const auto& it = items[i];
        if (!it.visible || it.kind == PanelItem::Progress) continue;
        if (it.kind == PanelItem::Label && !it.click) continue;
        if (PtInRect(&it.rc, p)) return i;
    }
    return -1;
}

void Panel::UpdateTip(int item) {
    if (item == tipItem_) return;
    tipItem_ = item;
    if (!tip_) {
        tip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               0, 0, 0, 0, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!tip_) return;
        TTTOOLINFOW ti = {sizeof ti};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = 1;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, S(420));
    }
    TTTOOLINFOW ti = {sizeof ti};
    ti.hwnd = hwnd_;
    ti.uId = 1;
    if (item >= 0 && !items[item].tip.empty()) ti.rect = items[item].rc;
    SendMessageW(tip_, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    SendMessageW(tip_, TTM_POP, 0, 0);
}

LRESULT Panel::On(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        Relayout();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)l;
        if (tip_ && nm->hwndFrom == tip_ && nm->code == TTN_GETDISPINFOW) {
            auto* di = (NMTTDISPINFOW*)l;
            static std::wstring text;
            text = tipItem_ >= 0 && tipItem_ < (int)items.size() ? items[tipItem_].tip : L"";
            di->lpszText = text.data();
        }
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetTextColor(dc, theme::Text);
        SetBkColor(dc, theme::Card);
        return (LRESULT)editBrush_;
    }
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&t);
        if (sliderDrag_ && pressed_ >= 0) {
            auto& it = items[pressed_];
            double f = std::clamp((double)(p.x - it.rc.left - S(6)) / std::max(1L, it.rc.right - it.rc.left - S(12)), 0.0, 1.0);
            it.value = f * it.max;
            if (it.changed) it.changed(it.value);
            Invalidate();
            return 0;
        }
        int h = HitTest(p);
        if (h != hot_) {
            hot_ = h;
            Invalidate();
        }
        UpdateTip(h);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hot_ >= 0) {
            hot_ = -1;
            Invalidate();
        }
        UpdateTip(-1);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        int h = HitTest(p);
        if (h < 0 || !items[h].enabled) return 0;
        pressed_ = h;
        SetCapture(hwnd_);
        if (items[h].kind == PanelItem::Slider) {
            sliderDrag_ = true;
            SendMessageW(hwnd_, WM_MOUSEMOVE, 0, l);
        }
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        int pr = pressed_;
        bool wasSlider = sliderDrag_;
        pressed_ = -1;
        sliderDrag_ = false;
        ReleaseCapture();
        Invalidate();
        if (!wasSlider && pr >= 0 && pr < (int)items.size() && PtInRect(&items[pr].rc, p) && items[pr].enabled && items[pr].click) {
            auto fn = items[pr].click;  // the click may rebuild the items
            fn();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (util::KeepDragOnCaptureLoss()) return 0;
        pressed_ = -1;
        sliderDrag_ = false;
        return 0;
    case WM_RBUTTONUP:
        if (rightClick) {
            POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
            ClientToScreen(hwnd_, &p);
            rightClick(p);
        }
        return 0;
    case WM_DESTROY:
        if (tip_) DestroyWindow(tip_);
        tip_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void Panel::Paint(HDC dc, const RECT& rc) {
    Fill(dc, rc, bg_);
    SetBkMode(dc, TRANSPARENT);
    if (paintExtra) paintExtra(*this, dc, rc);  // backgrounds such as the transport "pill"
    for (int i = 0; i < (int)items.size(); i++) {
        const auto& it = items[i];
        if (!it.visible) continue;
        RECT r = it.rc;
        bool hot = i == hot_ && it.enabled, down = i == pressed_;
        uint32_t textColor = !it.enabled ? theme::Faint : it.color != kNone ? it.color : theme::Text;
        switch (it.kind) {
        case PanelItem::Button: {
            uint32_t fill = it.fill;
            if (fill == kNone) fill = hot ? theme::CardHover : kNone;
            else if (hot) fill = Mix(fill, 0xFFFFFF, 0.12);
            if (down && fill != kNone) fill = Mix(fill, 0, 0.15);
            if (fill != kNone) {
                if (it.round) {
                    SetDCBrushColor(dc, fill);
                    SetDCPenColor(dc, fill);
                    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
                    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
                    Ellipse(dc, r.left, r.top, r.right, r.bottom);
                    SelectObject(dc, ob);
                    SelectObject(dc, op);
                } else {
                    RoundFill(dc, r, S(8), fill);
                }
            }
            if (it.checked) {
                RECT u = {r.left + S(8), r.bottom - S(3), r.right - S(8), r.bottom - S(1)};
                Fill(dc, u, theme::Accent);
            }
            int gw = 0, tw = 0;
            SIZE sz;
            if (!it.glyph.empty()) {
                SelectObject(dc, IconFont(dpi_, it.largeIcon));
                GetTextExtentPoint32W(dc, it.glyph.c_str(), (int)it.glyph.size(), &sz);
                gw = sz.cx;
            }
            HFONT tf = it.mono ? theme::MonoFont(dpi_) : theme::UiFont(dpi_, it.bold);
            if (!it.text.empty()) {
                SelectObject(dc, tf);
                GetTextExtentPoint32W(dc, it.text.c_str(), (int)it.text.size(), &sz);
                tw = sz.cx;
            }
            int total = gw + tw + (gw && tw ? S(6) : 0) + (it.dropdown ? S(12) : 0);
            int x = (r.left + r.right - total) / 2;
            SetTextColor(dc, textColor);
            if (gw) {
                SelectObject(dc, IconFont(dpi_, it.largeIcon));
                RECT g = {x, r.top, x + gw, r.bottom};
                DrawTextW(dc, it.glyph.c_str(), -1, &g, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                x += gw + (tw ? S(6) : 0);
            }
            if (tw) {
                SelectObject(dc, tf);
                RECT t = {x, r.top, x + tw, r.bottom};
                DrawTextW(dc, it.text.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                x += tw;
            }
            if (it.dropdown) {
                SelectObject(dc, theme::UiFont(dpi_));
                RECT t = {x + S(4), r.top, x + S(14), r.bottom};
                SetTextColor(dc, theme::Dim);
                DrawTextW(dc, L"▾", -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            break;
        }
        case PanelItem::Label: {
            if (it.fill != kNone) RoundFill(dc, r, S(8), it.fill);
            RECT t = r;
            int gx = 0;
            if (!it.glyph.empty()) {
                SelectObject(dc, IconFont(dpi_, it.largeIcon));
                SetTextColor(dc, it.color != kNone ? it.color : theme::Dim);
                RECT g = r;
                DrawTextW(dc, it.glyph.c_str(), -1, &g, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                gx = S(22);
            }
            t.left += gx;
            SelectObject(dc, it.mono ? theme::MonoFont(dpi_) : theme::UiFont(dpi_, it.bold));
            SetTextColor(dc, textColor == theme::Text && it.color == kNone ? theme::Dim : textColor);
            DrawTextW(dc, it.text.c_str(), -1, &t,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (it.alignRight ? DT_RIGHT : DT_LEFT));
            break;
        }
        case PanelItem::Slider: {
            int cy = (r.top + r.bottom) / 2, l = r.left + S(6), rr = r.right - S(6);
            RECT track = {l, cy - S(2), rr, cy + S(2)};
            Fill(dc, track, theme::Border);
            int x = l + (int)((rr - l) * std::clamp(it.value / std::max(1e-9, it.max), 0.0, 1.0));
            RECT done = {l, cy - S(2), x, cy + S(2)};
            Fill(dc, done, it.enabled ? theme::Accent : theme::Faint);
            SetDCBrushColor(dc, theme::Text);
            SetDCPenColor(dc, theme::Text);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
            HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
            Ellipse(dc, x - S(6), cy - S(6), x + S(6), cy + S(6));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            break;
        }
        case PanelItem::Progress: {
            int cy = (r.top + r.bottom) / 2;
            RECT track = {r.left, cy - S(3), r.right, cy + S(3)};
            Fill(dc, track, theme::Border);
            if (it.value >= 0) {
                RECT done = track;
                done.right = track.left + (int)((track.right - track.left) * std::clamp(it.value, 0.0, 1.0));
                Fill(dc, done, theme::Accent);
            } else {
                // Indeterminate: a block sliding back and forth.
                int span = track.right - track.left, block = std::max(S(30), span / 4);
                double ph = (GetTickCount() % 1600) / 1600.0;
                double f = ph < 0.5 ? ph * 2 : 2 - ph * 2;
                RECT b = track;
                b.left = track.left + (int)((span - block) * f);
                b.right = b.left + block;
                Fill(dc, b, theme::Accent);
            }
            break;
        }
        }
    }
}

}  // namespace edit
