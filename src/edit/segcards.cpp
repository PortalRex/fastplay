// SegmentCards (Theme.cs) port: the segment list as owner-drawn cards in a virtualized, scrolling Win32 child window,
// plus the edit-mode font cache.
#include "segcards.h"
#include "uicanvas.h"
#include <algorithm>
#include <map>

namespace edit {

// ------------------------------------------------------------------------------------------------ fonts

namespace {

enum FontKind { kUi, kUiBold, kMono, kMonoLarge, kSmall, kIconsSmall };

// One font per (DPI, kind) for the life of the process: a handful of handles, no ownership questions for callers.
HFONT CachedFont(int dpi, FontKind kind) {
    static std::map<std::pair<int, int>, HFONT> cache;
    if (dpi <= 0) dpi = 96;
    HFONT& f = cache[{dpi, (int)kind}];
    if (f) return f;
    const wchar_t* face = L"Segoe UI";
    float pt = 9;
    int weight = FW_NORMAL;
    switch (kind) {
    case kUi: break;
    case kUiBold: weight = FW_SEMIBOLD; break;
    case kMono: face = L"Consolas"; pt = 10; break;
    case kMonoLarge: face = L"Consolas"; pt = 12.5f; weight = FW_BOLD; break;
    case kSmall: pt = 8; break;
    case kIconsSmall: face = L"Segoe MDL2 Assets"; pt = 8; break;
    }
    f = CreateFontW(-(int)(pt * dpi / 72.0 + 0.5), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    return f;
}

const wchar_t kCardsClass[] = L"FastPlay.EditSegmentCards";
const wchar_t kCheckGlyph[] = L"\xE73E";  // Segoe MDL2 Assets "CheckMark"

// Dark scrollbar where the OS has the theme (Windows 10 1809+); uxtheme is looked up so FastPlay needn't link it.
void DarkScrollbar(HWND h) {
    using SetThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static SetThemeFn fn = [] {
        HMODULE m = LoadLibraryW(L"uxtheme.dll");
        return m ? (SetThemeFn)GetProcAddress(m, "SetWindowTheme") : nullptr;
    }();
    if (fn) fn(h, L"DarkMode_Explorer", nullptr);
}

}  // namespace

namespace theme {
HFONT UiFont(int dpi, bool bold) { return CachedFont(dpi, bold ? kUiBold : kUi); }
HFONT MonoFont(int dpi, bool large) { return CachedFont(dpi, large ? kMonoLarge : kMono); }
}  // namespace theme

// ------------------------------------------------------------------------------------------------ SegmentCards

struct SegmentCards::Impl {
    SegmentCards& sc;
    SegmentCardsCallbacks cb;
    HWND hwnd = nullptr;
    int dpi = 96, w = 0, h = 0;
    ui::Canvas canvas;
    int count = 0, scrollY = 0, hoverIndex = -1, dragFrom = -1, dropAt = -1, downY = 0;
    bool dragging = false, tracking = false;

    Impl(SegmentCards& s, SegmentCardsCallbacks c) : sc(s), cb(std::move(c)) {}

    int S(int v) const { return MulDiv(v, dpi, 96); }
    float F(float v) const { return v * dpi / 96.f; }
    int ItemH() const { return S(58); }
    int Top() const { return S(4); }
    int ContentH() const { return count * ItemH() + S(8); }
    int MaxScroll() const { return std::max(0, ContentH() - h); }

    RECT ItemRect(int i) const {
        int y = Top() + i * ItemH() - scrollY;
        return {0, y, w, y + ItemH()};
    }
    int IndexAt(int y) const {
        int off = y - Top() + scrollY;
        int i = off / ItemH();
        return off >= 0 && i < count ? i : -1;
    }
    RECT ToggleRect(const RECT& item) const {
        int s = S(20), x = item.right - s - S(14), y = item.top + (ItemH() - s) / 2;
        return {x, y, x + s, y + s};
    }
    bool OnToggle(int i, POINT p) const {
        RECT r = ToggleRect(ItemRect(i));
        return PtInRect(&r, p) != 0;
    }

    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

    void UpdateScrollbar() {
        scrollY = std::clamp(scrollY, 0, MaxScroll());
        SCROLLINFO si = {sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS};
        si.nMin = 0;
        si.nMax = ContentH() - 1;
        si.nPage = (UINT)std::max(0, h);
        si.nPos = scrollY;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    }

    void ScrollTo(int y) {
        y = std::clamp(y, 0, MaxScroll());
        if (y == scrollY) return;
        scrollY = y;
        SetScrollPos(hwnd, SB_VERT, scrollY, TRUE);
        Invalidate();
    }

    // ---- painting ----
    void Paint() {
        canvas.Fill(0, 0, w, h, theme::Panel);
        const auto* segs = sc.segments;
        if (!segs) return;
        int first = std::max(0, IndexAt(0)), last = IndexAt(h);
        if (last < 0) last = count - 1;
        for (int i = first; i <= last && i < count && i < (int)segs->size(); i++) DrawCard(i, ItemRect(i));
        if (dragging && dropAt >= 0) {
            float y = (float)(Top() + dropAt * ItemH() - scrollY);
            canvas.Round({F(10), y - F(2), w - F(10), y + F(2)}, F(2), theme::Accent);
        }
    }

    void DrawCard(int index, const RECT& b) {
        const Segment& s = (*sc.segments)[index];
        HDC dc = canvas.Dc();
        bool current = index == sc.current, marked = current || (sc.marked && sc.marked->count(index));
        const auto& pal = Palette();
        int n = (int)pal.size();
        COLORREF color = n == 0 ? theme::Accent : pal[((s.color % n) + n) % n];
        COLORREF mark = s.enabled ? color : theme::Faint;

        ui::RectF card = {b.left + F(8), b.top + F(3), b.right - F(8), b.bottom - F(3)};
        float cardH = card.y1 - card.y0;
        COLORREF bg = marked ? theme::CardSelected : index == hoverIndex ? theme::CardHover : theme::Card;
        canvas.Round(card, F(8), bg);
        if (marked) canvas.Round(card, F(8), current ? color : theme::Dim, 255, current ? F(1.6f) : F(1));
        canvas.Round({card.x0, card.y0, card.x0 + F(5), card.y1}, F(2.5f), mark);  // color strip

        float bs = (float)S(24), bx = card.x0 + F(14), by = card.y0 + (cardH - bs) / 2;
        canvas.Circle(bx + bs / 2, by + bs / 2, bs / 2, mark);
        RECT badge = {(LONG)bx, (LONG)by, (LONG)(bx + bs), (LONG)(by + bs)};
        canvas.Text(theme::UiFont(dpi, true), std::to_wstring(index + 1), badge, RGB(255, 255, 255),
                 DT_CENTER | DT_VCENTER);

        COLORREF fg = s.enabled ? theme::Text : theme::Faint;
        int tx = (int)(bx + bs) + S(10);
        RECT toggle = ToggleRect(b);
        int textRight = toggle.left - S(8);
        RECT line1 = {tx, (int)card.y0 + S(5), textRight, (int)card.y0 + S(5) + (int)(cardH / 2) - S(3)};
        RECT line2 = {tx, line1.bottom, textRight, line1.bottom + (int)(cardH / 2) - S(6)};
        const UINT flags = DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS;

        auto fmt = [&](double t) { return sc.fmt ? sc.fmt(t) : Fmt(t); };
        std::wstring times = fmt(s.start) + L"  \x2192  " + fmt(s.end);
        HFONT uiFont = theme::UiFont(dpi), bold = theme::UiFont(dpi, true), small = CachedFont(dpi, kSmall);
        canvas.Text(uiFont, times, line1, fg, flags);
        if (sc.warning) {
            std::wstring warn = sc.warning(index);
            if (!warn.empty()) {
                int tw = ui::TextSize(dc, uiFont, times).cx + S(10);
                RECT wr = {line1.left + tw, line1.top, line1.right, line1.bottom};
                canvas.Text(small, L"\x26A0 " + warn, wr, theme::Warn, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        std::wstring dur = ui::StripZeroHours(Fmt(s.Duration()));
        canvas.Text(bold, dur, line2, fg, DT_RIGHT | DT_VCENTER);
        int durW = ui::TextSize(dc, bold, dur).cx + S(8);
        bool named = !s.name.empty();
        std::wstring label = named ? s.name : s.enabled ? L"No label" : L"Excluded";
        if (!s.tags.empty()) {
            std::wstring tags = s.tags;
            for (size_t p = 0; (p = tags.find(L", ", p)) != std::wstring::npos; p += 3) tags.replace(p, 2, L"  #");
            label += L"   #" + tags;
        }
        RECT lr = {line2.left, line2.top, line2.right - durW, line2.bottom};
        canvas.Text(named ? uiFont : small, label, lr, named ? fg : theme::Faint, flags);

        float tcx = (toggle.left + toggle.right) / 2.f, tcy = (toggle.top + toggle.bottom) / 2.f;
        float tr = (toggle.right - toggle.left) / 2.f;
        if (s.enabled) {
            canvas.Circle(tcx, tcy, tr, color);
            canvas.Text(CachedFont(dpi, kIconsSmall), kCheckGlyph, toggle, RGB(255, 255, 255), DT_CENTER | DT_VCENTER);
        } else {
            canvas.Circle(tcx, tcy, tr - F(1), theme::Faint, 255, F(1.5f));
        }
    }

    // ---- mouse ----
    void OnDown(POINT p) {
        int i = IndexAt(p.y);
        if (i < 0) return;
        if (OnToggle(i, p)) {
            if (cb.toggleRequested) cb.toggleRequested(i);
            return;
        }
        dragFrom = i;
        downY = p.y;
        SetCapture(hwnd);
    }

    void OnMove(POINT p, WPARAM mk) {
        if (dragFrom >= 0 && (mk & MK_LBUTTON)) {
            if (!dragging && std::abs(p.y - downY) > S(6)) dragging = true;
            if (dragging) {
                if (p.y < S(10)) ScrollTo(scrollY - ItemH() / 3);
                else if (p.y > h - S(10)) ScrollTo(scrollY + ItemH() / 3);
                dropAt = std::clamp((int)(p.y - Top() + scrollY + ItemH() / 2) / ItemH(), 0, count);
                Invalidate();
            }
            return;
        }
        if (!tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            tracking = TrackMouseEvent(&t) != 0;
        }
        int hi = IndexAt(p.y);
        if (hi != hoverIndex) {
            hoverIndex = hi;
            Invalidate();
        }
    }

    void OnUp(WPARAM mk) {
        int from = dragFrom, drop = dropAt;
        bool wasDragging = dragging;
        dragFrom = dropAt = -1;  // before ReleaseCapture, so WM_CAPTURECHANGED sees no drag
        dragging = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (from < 0) return;
        if (wasDragging) {
            Invalidate();
            int to = drop > from ? drop - 1 : drop;
            if (to != from && to >= 0 && cb.reorder) cb.reorder(from, to);
        } else if (cb.itemClicked) {
            cb.itemClicked(from, (uint32_t)(mk & (MK_CONTROL | MK_SHIFT)));
        }
    }

    void OnWheel(int delta) {
        ScrollTo(scrollY - delta * ItemH() / WHEEL_DELTA);  // one card per notch; touchpads send smaller deltas
    }

    void OnVScroll(int code) {
        int y = scrollY;
        switch (code) {
        case SB_LINEUP: y -= ItemH() / 3; break;
        case SB_LINEDOWN: y += ItemH() / 3; break;
        case SB_PAGEUP: y -= h; break;
        case SB_PAGEDOWN: y += h; break;
        case SB_TOP: y = 0; break;
        case SB_BOTTOM: y = MaxScroll(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si = {sizeof si, SIF_TRACKPOS};
            GetScrollInfo(hwnd, SB_VERT, &si);
            y = si.nTrackPos;
            break;
        }
        default: return;
        }
        ScrollTo(y);
    }

    // ---- window ----
    static LRESULT CALLBACK Proc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
        Impl* self;
        if (m == WM_NCCREATE) {
            self = (Impl*)((CREATESTRUCTW*)lp)->lpCreateParams;
            self->hwnd = hw;
            SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = (Impl*)GetWindowLongPtrW(hw, GWLP_USERDATA);
        }
        return self ? self->On(m, wp, lp) : DefWindowProcW(hw, m, wp, lp);
    }

    LRESULT On(UINT m, WPARAM wp, LPARAM lp) {
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (canvas.Ensure(w, h)) {
                Paint();
                canvas.Present(dc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            if (canvas.Ensure(w, h)) {
                Paint();
                canvas.Present((HDC)wp);
            }
            return 0;
        case WM_SIZE:
            w = LOWORD(lp);
            h = HIWORD(lp);
            UpdateScrollbar();
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
            OnDown(p);
            return 0;
        case WM_LBUTTONDBLCLK: {  // the first click already selected; a double-click edits (or toggles again)
            int i = IndexAt(p.y);
            if (i < 0) return 0;
            if (OnToggle(i, p)) {
                if (cb.toggleRequested) cb.toggleRequested(i);
            } else if (cb.editRequested) {
                cb.editRequested(i);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            OnMove(p, wp);
            return 0;
        case WM_LBUTTONUP:
            OnUp(wp);
            return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd && dragFrom >= 0 && !util::KeepDragOnCaptureLoss()) {
                dragFrom = dropAt = -1;
                dragging = false;
                Invalidate();
            }
            return 0;
        case WM_RBUTTONUP:
            if (cb.contextMenu) {
                int i = IndexAt(p.y);
                ClientToScreen(hwnd, &p);
                cb.contextMenu(p, i);
            }
            return 0;
        case WM_MOUSELEAVE:
            tracking = false;
            if (hoverIndex != -1) {
                hoverIndex = -1;
                Invalidate();
            }
            return 0;
        case WM_MOUSEWHEEL:
            OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_VSCROLL:
            OnVScroll(LOWORD(wp));
            return 0;
        case WM_DESTROY:
            canvas.Free();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            hwnd = nullptr;
            sc.hwnd_ = nullptr;
            break;
        }
        return DefWindowProcW(hwnd, m, wp, lp);
    }
};

bool SegmentCards::Create(HINSTANCE inst, HWND parent, SegmentCardsCallbacks cb) {
    impl_ = std::make_unique<Impl>(*this, std::move(cb));
    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Impl::Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kCardsClass;
    RegisterClassExW(&wc);  // fails harmlessly when already registered
    UINT d = parent ? GetDpiForWindow(parent) : 0;
    impl_->dpi = d ? (int)d : 96;
    hwnd_ = CreateWindowExW(0, kCardsClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL, 0, 0, 10, 10, parent,
                            nullptr, inst, impl_.get());
    if (!hwnd_) return false;
    DarkScrollbar(hwnd_);
    Refresh();
    return true;
}

SegmentCards::SegmentCards() = default;

SegmentCards::~SegmentCards() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
}

void SegmentCards::SetDpi(int dpi) {
    if (!impl_ || dpi <= 0 || dpi == impl_->dpi) return;
    impl_->dpi = dpi;
    impl_->UpdateScrollbar();
    impl_->Invalidate();
}

void SegmentCards::Refresh() {
    if (!impl_ || !hwnd_) return;
    Impl& i = *impl_;
    i.count = segments ? (int)segments->size() : 0;
    if (i.hoverIndex >= i.count) i.hoverIndex = -1;
    i.UpdateScrollbar();
    i.Invalidate();
}

void SegmentCards::EnsureVisible(int index) {
    if (!impl_ || !hwnd_) return;
    Impl& i = *impl_;
    if (index < 0 || index >= i.count) return;
    int top = i.Top() + index * i.ItemH(), scroll = i.scrollY;
    if (top < scroll) scroll = top - i.Top();
    else if (top + i.ItemH() > scroll + i.h) scroll = top + i.ItemH() - i.h + i.Top();
    i.ScrollTo(std::max(0, scroll));
    i.Invalidate();
}

}  // namespace edit
