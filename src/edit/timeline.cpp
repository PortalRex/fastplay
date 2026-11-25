// Timeline.cs port: zoomable timeline with ruler, thumbnail strip, waveform, segment bars, keyframe marks and the
// playhead. Painted into a DIB-section back buffer (uicanvas.h) so translucent tints and hatches stay cheap.
#include "timeline.h"
#include "media.h"
#include "segcards.h"
#include "uicanvas.h"
#include <algorithm>
#include <cmath>

namespace edit {

namespace {

const wchar_t kTimelineClass[] = L"FastPlay.EditTimeline";
const UINT_PTR kWaveTimer = 1;
const UINT kWaveIntervalMs = 66;  // WaveUpdated repaints at most ~15x per second

const COLORREF kRow = RGB(30, 30, 33);
const COLORREF kWave = RGB(110, 160, 220);
const COLORREF kTick = RGB(90, 90, 100);
const COLORREF kPlay = RGB(235, 64, 64);
const COLORREF kLabel = RGB(200, 200, 205);
const COLORREF kPlaceholder = RGB(38, 38, 43);
const COLORREF kLoadFill = RGB(56, 132, 255);
const COLORREF kLoadText = RGB(170, 175, 185);
const COLORREF kWhite = RGB(255, 255, 255);
const COLORREF kBlack = RGB(0, 0, 0);
const COLORREF kMarker = RGB(70, 200, 120);  // same green as the seekbar's markers

const double kSteps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};

HFONT MakeFont(int dpi, float pt, int weight) {
    return CreateFontW(-(int)std::lround(pt * dpi / 72.0), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

// Area-average downscale (nearest-neighbour when enlarging) of a BGRX image.
void ScaleImage(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy0 = y * sh / dh, sy1 = std::max(sy0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; x++) {
            int sx0 = x * sw / dw, sx1 = std::max(sx0 + 1, (x + 1) * sw / dw);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const uint32_t* row = src + (size_t)yy * sw;
                for (int xx = sx0; xx < sx1; xx++) {
                    uint32_t p = row[xx];
                    r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF;
                    b += p & 0xFF;
                    n++;
                }
            }
            dst[(size_t)y * dw + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
}

}  // namespace

struct Timeline::Impl {
    Timeline& tl;
    TimelineCallbacks cb;
    HWND hwnd = nullptr;
    int dpi = 96, w = 0, h = 0;
    HFONT smallFont = nullptr, segFont = nullptr;
    ui::Canvas canvas;

    enum class Drag { None, Scrub, Edge, Pan, Create, Move };
    Drag drag = Drag::None;
    UINT dragButton = 0;  // WM_LBUTTONUP / WM_MBUTTONUP that ends it
    int dragSeg = -1;
    bool dragIsStart = false, dragStarted = false, editOpen = false;
    int panStartX = 0, downX = 0;
    double panStartView = 0, anchorT = 0, moveOffset = 0;
    int hoverX = -1;
    bool tracking = false;
    HCURSOR cursor = LoadCursorW(nullptr, IDC_ARROW);

    // Waveform: display gain and per-column peaks are cached until the wave or the view changes.
    bool waveTimer = false;
    uint64_t waveVersion = 0;
    float waveGain = 1;
    const float* gainWave = nullptr;
    size_t gainCount = 0;
    uint64_t gainVersion = ~0ull;
    std::vector<float> cols;
    const float* colWave = nullptr;
    size_t colCount = 0;
    uint64_t colVersion = ~0ull;
    double colStart = -1, colLen = -1;

    // Thumbnails scaled to the strip's slot size (scaling once instead of on every repaint).
    struct Scaled {
        std::weak_ptr<const Thumb> src;
        const Thumb* key = nullptr;
        int w = 0, h = 0;
        std::vector<uint32_t> px;
        uint64_t used = 0;
    };
    std::vector<Scaled> scaled;
    uint64_t scaleTick = 0;
    std::vector<double> lastWant, want;

    Impl(Timeline& t, TimelineCallbacks c) : tl(t), cb(std::move(c)) {}
    ~Impl() { FreeFonts(); }

    // ---- sizes (DPI scaled) ----
    int S(int v) const { return MulDiv(v, dpi, 96); }
    float F(float v) const { return v * dpi / 96.f; }
    int RulerH() const { return S(20); }
    int ThumbH() const { return S(50); }
    int WaveH() const { return S(34); }
    int SegH() const { return S(34); }
    bool ThumbRow() const { return tl.showThumbs && tl.hasVideo; }
    int ThumbTop() const { return RulerH(); }
    int WaveTop() const { return ThumbTop() + (ThumbRow() ? ThumbH() : 0); }
    int SegTop() const { return WaveTop() + (tl.showWave ? WaveH() : 0); }

    void FreeFonts() {
        if (smallFont) DeleteObject(smallFont);
        if (segFont) DeleteObject(segFont);
        smallFont = segFont = nullptr;
    }
    void MakeFonts() {
        FreeFonts();
        smallFont = MakeFont(dpi, 7.5f, FW_NORMAL);
        segFont = MakeFont(dpi, 8.5f, FW_BOLD);
    }

    // ---- data access ----
    size_t SegCount() const { return tl.segments ? tl.segments->size() : 0; }
    Segment& Seg(size_t i) { return (*tl.segments)[i]; }
    bool IsMarked(int i) const { return tl.marked && tl.marked->count(i); }
    COLORREF SegColor(const Segment& s) const {
        const auto& pal = Palette();
        if (pal.empty()) return RGB(59, 130, 246);
        int n = (int)pal.size();
        return pal[((s.color % n) + n) % n];
    }
    std::wstring TimeText(double t) const { return cb.timeText ? cb.timeText(t) : Fmt(t); }

    double& ViewStart() { return tl.viewStart_; }
    double& ViewLen() { return tl.viewLen_; }
    double X2T(double x) const { return tl.viewStart_ + x / std::max(1, w) * tl.viewLen_; }
    float T2X(double t) const { return (float)((t - tl.viewStart_) / tl.viewLen_ * w); }

    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    void ClampView() { tl.viewStart_ = std::clamp(tl.viewStart_, 0.0, std::max(0.0, tl.duration - tl.viewLen_)); }

    void Seek(double t) { if (cb.seek) cb.seek(t); }
    void Select(int i, uint32_t mods) { if (cb.selectSegment) cb.selectSegment(i, mods); }
    void BeginEdit() {
        if (editOpen) return;
        editOpen = true;
        if (cb.editBegin) cb.editBegin();
    }

    // ---- hit testing ----
    int HitEdge(int x, bool* isStart) {
        float tol = F(6);
        int sel = tl.selected;
        if (sel >= 0 && (size_t)sel < SegCount()) {
            const Segment& s = Seg(sel);
            if (std::fabs(T2X(s.start) - x) <= tol) { *isStart = true; return sel; }
            if (std::fabs(T2X(s.end) - x) <= tol) { *isStart = false; return sel; }
        }
        for (size_t i = 0; i < SegCount(); i++) {
            if (std::fabs(T2X(Seg(i).start) - x) <= tol) { *isStart = true; return (int)i; }
            if (std::fabs(T2X(Seg(i).end) - x) <= tol) { *isStart = false; return (int)i; }
        }
        return -1;
    }

    // Prefers the selected segment and then the shortest, so small segments inside big ones stay clickable.
    int HitSegment(double t) {
        int sel = tl.selected;
        if (sel >= 0 && (size_t)sel < SegCount() && t >= Seg(sel).start && t <= Seg(sel).end) return sel;
        int best = -1;
        for (size_t i = 0; i < SegCount(); i++)
            if (t >= Seg(i).start && t <= Seg(i).end && (best < 0 || Seg(i).Duration() < Seg(best).Duration()))
                best = (int)i;
        return best;
    }

    double Snap(double t, bool toKeyframe) const {
        t = std::clamp(t, 0.0, std::max(0.0, tl.duration));
        const auto& k = tl.keyframes;
        if (!toKeyframe || k.empty()) return t;
        auto it = std::lower_bound(k.begin(), k.end(), t);
        if (it == k.begin()) return *it;
        if (it == k.end()) return k.back();
        return std::fabs(*(it - 1) - t) < std::fabs(*it - t) ? *(it - 1) : *it;
    }

    // ---- mouse ----
    void StartCapture(Drag d, UINT upMsg) {
        drag = d;
        dragButton = upMsg;
        SetCapture(hwnd);
    }

    void OnButtonDown(int x, int y, WPARAM mk, bool middle) {
        bool alt = GetKeyState(VK_MENU) < 0;
        if (middle || alt) {
            panStartX = x;
            panStartView = tl.viewStart_;
            StartCapture(Drag::Pan, middle ? WM_MBUTTONUP : WM_LBUTTONUP);
            return;
        }
        downX = x;
        dragStarted = false;
        if (cb.hoverEnd) cb.hoverEnd();
        uint32_t mods = (uint32_t)(mk & (MK_CONTROL | MK_SHIFT));
        bool shift = (mk & MK_SHIFT) != 0;
        if (y >= SegTop()) {
            bool isStart = false;
            int seg = HitEdge(x, &isStart);
            if (seg >= 0 && mods == 0) {
                dragSeg = seg;
                dragIsStart = isStart;
                Select(seg, 0);
                BeginEdit();
                StartCapture(Drag::Edge, WM_LBUTTONUP);
                return;
            }
            int hit = HitSegment(X2T(x));
            if (hit >= 0) {
                Select(hit, mods);
                if (mods == 0 && (size_t)hit < SegCount()) {
                    dragSeg = hit;
                    moveOffset = X2T(x) - Seg(hit).start;
                    StartCapture(Drag::Move, WM_LBUTTONUP);
                }
                return;
            }
            // Empty segment row: a drag creates a segment, a click just seeks.
            anchorT = Snap(X2T(x), shift);
            StartCapture(Drag::Create, WM_LBUTTONUP);
            Seek(anchorT);
            return;
        }
        StartCapture(Drag::Scrub, WM_LBUTTONUP);
        Seek(Snap(X2T(x), shift));
    }

    void OnMouseMove(int x, int y, WPARAM mk) {
        bool shift = (mk & MK_SHIFT) != 0;
        int threshold = S(4);
        switch (drag) {
        case Drag::Scrub:
            Seek(Snap(X2T(x), shift));
            break;
        case Drag::Pan:
            tl.viewStart_ = panStartView - (double)(x - panStartX) / std::max(1, w) * tl.viewLen_;
            ClampView();
            Invalidate();
            break;
        case Drag::Create: {
            if (std::abs(x - downX) <= threshold && !dragStarted) break;
            double ct = Snap(X2T(x), shift);
            if (!dragStarted) {
                if (!cb.newSegment || !tl.segments) break;
                dragStarted = true;
                BeginEdit();
                tl.segments->push_back(cb.newSegment(std::min(anchorT, ct), std::max(anchorT, ct) + 0.001));
                dragSeg = (int)SegCount() - 1;
                Select(dragSeg, 0);
            }
            if (dragSeg < 0 || (size_t)dragSeg >= SegCount()) break;
            Segment& c = Seg(dragSeg);
            c.start = std::min(anchorT, ct);
            c.end = std::max(anchorT, ct);
            if (c.end - c.start < 0.001) c.end = c.start + 0.001;
            Seek(ct);
            Invalidate();
            break;
        }
        case Drag::Move: {
            if (dragSeg < 0 || (size_t)dragSeg >= SegCount()) break;
            if (!dragStarted && std::abs(x - downX) <= threshold) break;
            if (!dragStarted) {
                dragStarted = true;
                BeginEdit();
            }
            Segment& m = Seg(dragSeg);
            double len = m.Duration(), maxStart = std::max(0.0, tl.duration - len);
            double ns = std::clamp(X2T(x) - moveOffset, 0.0, maxStart);
            if (shift) ns = std::clamp(Snap(ns, true), 0.0, maxStart);
            m.start = ns;
            m.end = ns + len;
            Seek(ns);
            Invalidate();
            break;
        }
        case Drag::Edge: {
            if (dragSeg < 0 || (size_t)dragSeg >= SegCount()) break;
            Segment& s = Seg(dragSeg);
            double t = Snap(X2T(x), shift);
            if (dragIsStart) s.start = std::min(t, s.end - 0.001);
            else s.end = std::max(t, s.start + 0.001);
            Seek(dragIsStart ? s.start : s.end);
            Invalidate();
            break;
        }
        case Drag::None: {
            if (!tracking) {
                TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
                tracking = TrackMouseEvent(&t) != 0;
            }
            bool isStart;
            bool edge = y >= SegTop() && HitEdge(x, &isStart) >= 0;
            cursor = LoadCursorW(nullptr, edge ? IDC_SIZEWE : IDC_ARROW);
            SetCursor(cursor);
            if (tl.hasMedia) {
                hoverX = x;
                Invalidate();
                if (cb.hover) {
                    POINT p = {x, 0};
                    ClientToScreen(hwnd, &p);
                    cb.hover(std::clamp(X2T(x), 0.0, std::max(0.0, tl.duration)), p);
                }
            }
            break;
        }
        }
    }

    // Ends any drag; editEnd pairs with the editBegin a drag sent (also when capture is lost mid-drag).
    void EndDrag(bool release) {
        if (drag == Drag::None) return;
        bool end = editOpen;
        drag = Drag::None;
        editOpen = false;
        dragStarted = false;
        if (release && GetCapture() == hwnd) ReleaseCapture();
        if (end && cb.editEnd) cb.editEnd();
        Invalidate();
    }

    void OnWheel(int screenX, int screenY, int delta, WPARAM mk) {
        POINT p = {screenX, screenY};
        ScreenToClient(hwnd, &p);
        double notches = delta / (double)WHEEL_DELTA;  // fractional on precision touchpads
        if (mk & MK_SHIFT) {
            tl.viewStart_ -= notches * tl.viewLen_ * 0.15;
            ClampView();
            Invalidate();
        } else {
            tl.Zoom(std::pow(1.35, notches), X2T(p.x));
        }
    }

    // ---- painting ----
    void Paint() {
        ui::Canvas& cv = canvas;
        cv.Fill(0, 0, w, h, theme::Bg);
        if (w < 2 || !tl.hasMedia) return;
        if (tl.viewLen_ <= 0) tl.viewLen_ = std::max(tl.duration, 0.01);

        DrawRuler();
        if (ThumbRow()) DrawThumbs();
        else if (!lastWant.empty() && tl.thumbs) {
            tl.thumbs->Want({}, 0, 0);  // strip hidden: drop the old wish list
            lastWant.clear();
        }
        if (tl.showWave) DrawWave();
        TintSegments();
        DrawSegments();
        ShadeCutMode();
        if (tl.showKeyframes) DrawKeyframes();
        DrawMarkers();
        if (tl.keyframeProgress >= 0) DrawLoading(SegTop(), SegH() + S(4), tl.keyframeProgress, L"Scanning keyframes");

        int rulerH = RulerH();
        if (hoverX >= 0 && drag == Drag::None) {
            cv.Fill(hoverX, rulerH, hoverX + std::max(1, S(1)), h, kWhite, 110);
            Bubble(TimeText(X2T(hoverX)), (float)hoverX, theme::CardSelected, theme::Text);
        }
        float px = T2X(tl.position);
        if (px >= -2 && px <= w + 2) {
            cv.FillF(px - F(1), (float)rulerH, px + F(1), (float)h, kPlay);
            Bubble(TimeText(tl.position), px, kPlay, kWhite);
        }
    }

    // Time label in a rounded box on the ruler, centered on x.
    void Bubble(const std::wstring& text, float x, COLORREF fill, COLORREF fg) {
        SIZE sz = ui::TextSize(canvas.Dc(), smallFont, text);
        float bw = sz.cx + F(8);
        float bx = std::clamp(x - bw / 2, 0.f, std::max(0.f, w - bw));
        float top = F(2), bottom = RulerH() - F(2);
        canvas.Round({bx, top, bx + bw, bottom}, F(4), fill);
        RECT r = {(LONG)bx, (LONG)top, (LONG)(bx + bw), (LONG)bottom};
        canvas.Text(smallFont, text, r, fg, DT_CENTER | DT_VCENTER);
    }

    void DrawLoading(int top, int rowH, double progress, const wchar_t* label) {
        canvas.Fill(0, top + 1, (int)(w * std::clamp(progress, 0.0, 1.0)), top + rowH - 1, kLoadFill, 45);
        wchar_t buf[96];
        swprintf(buf, 96, L"%s  %.0f%%", label, progress * 100);
        RECT r = {S(8), top, w, top + rowH};
        canvas.Text(smallFont, buf, r, kLoadText, DT_LEFT | DT_VCENTER);
    }

    void DrawRuler() {
        int rulerH = RulerH();
        canvas.Fill(0, 0, w, rulerH, kRow);
        double minStep = tl.viewLen_ * F(90) / w;
        double step = 7200;
        for (double s : kSteps)
            if (s >= minStep) { step = s; break; }
        int tickW = std::max(1, S(1));
        long long first = (long long)std::floor(tl.viewStart_ / step);
        auto visible = [&](long long k) { return k * step <= tl.viewStart_ + tl.viewLen_; };
        // Ticks first, then all labels: one GDI flush instead of one per label.
        for (long long k = first; visible(k); k++) {
            double t = k * step;
            int x = (int)std::floor(T2X(t));
            canvas.Fill(x, rulerH - S(6), x + tickW, rulerH, kTick);
            for (int m = 1; m < 5; m++) {
                int sx = (int)std::floor(T2X(t + step * m / 5));
                canvas.Fill(sx, rulerH - S(3), sx + tickW, rulerH, kTick);
            }
        }
        for (long long k = first; visible(k); k++)
            canvas.TextAt(smallFont, RulerLabel(k * step, step), (int)std::floor(T2X(k * step)) + S(2), S(2), kLabel);
    }

    // Whole-second steps drop a ".000" fraction; files under an hour drop the "00:" hours.
    std::wstring RulerLabel(double t, double step) const {
        std::wstring s = TimeText(t);
        if (step >= 1) {
            size_t dot = s.rfind(L'.');
            if (dot != std::wstring::npos && s.find_first_not_of(L'0', dot + 1) == std::wstring::npos) s.erase(dot);
        }
        return tl.duration < 3600 ? ui::StripZeroHours(s) : s;
    }

    const Scaled* ScaledThumb(const std::shared_ptr<const Thumb>& t, int dw, int dh) {
        for (Scaled& s : scaled)
            if (s.key == t.get() && s.w == dw && s.h == dh && !s.src.expired()) {
                s.used = ++scaleTick;
                return &s;
            }
        const size_t kMax = 128;
        Scaled* slot;
        if (scaled.size() < kMax) {
            scaled.emplace_back();
            slot = &scaled.back();
        } else {
            slot = &*std::min_element(scaled.begin(), scaled.end(),
                                      [](const Scaled& a, const Scaled& b) { return a.used < b.used; });
        }
        slot->src = t;
        slot->key = t.get();
        slot->w = dw;
        slot->h = dh;
        slot->px.resize((size_t)dw * dh);
        ScaleImage(t->px.data(), t->w, t->h, slot->px.data(), dw, dh);
        slot->used = ++scaleTick;
        return slot;
    }

    void DrawThumbs() {
        if (!tl.thumbs) return;
        int th = ThumbH() - S(4), top = ThumbTop();
        float slotW = (float)(th * tl.thumbAspect);
        if (slotW < 4 || th < 4) return;
        double slotDur = tl.viewLen_ * slotW / w;
        if (!(slotDur > 0)) return;
        int dw = (int)std::ceil(slotW);
        want.clear();
        bool missing = false;
        for (long long i = (long long)std::floor(tl.viewStart_ / slotDur);; i++) {
            double t = (i + 0.5) * slotDur;
            float x = T2X(i * slotDur);
            if (x > w || t > tl.duration) break;
            want.push_back(t);
            auto img = tl.thumbs->Get(t, slotDur * 0.6);
            if (!img) {
                missing = true;
                img = tl.thumbs->Get(t, slotDur * 4);
            }
            if (img && img->w > 0 && img->h > 0 && img->px.size() >= (size_t)img->w * img->h) {
                const Scaled* s = ScaledThumb(img, dw, th);
                canvas.Blit(s->px.data(), dw, th, (int)std::floor(x), top + S(2));
            } else {  // still loading
                canvas.Fill((int)x + 1, top + S(3), (int)(x + slotW) - 1, top + S(3) + th - 2, kPlaceholder);
            }
        }
        if (missing || want != lastWant) {
            tl.thumbs->Want(want, slotDur * 0.6, tl.viewStart_ + tl.viewLen_ / 2);
            lastWant = want;
        }
    }

    void UpdateWaveCache() {
        const float* wave = tl.wave;
        size_t n = tl.waveCount;
        if (wave != gainWave || n != gainCount || waveVersion != gainVersion) {
            float mx = 0;
            for (size_t i = 0; i < n; i++) mx = std::max(mx, wave[i]);
            waveGain = mx > 0 ? std::min(8.f, 0.95f / mx) : 1.f;  // normalize so quiet audio stays visible
            gainWave = wave;
            gainCount = n;
            gainVersion = waveVersion;
            colVersion = ~0ull;
        }
        if (colWave == wave && colCount == n && colVersion == waveVersion && colStart == tl.viewStart_ &&
            colLen == tl.viewLen_ && (int)cols.size() == w)
            return;
        cols.assign(w, -1.f);  // -1: past the end of the wave
        double perPx = tl.viewLen_ / w * kWaveRate;
        for (int x = 0; x < w; x++) {
            long long a = (long long)((tl.viewStart_ + (double)x / w * tl.viewLen_) * kWaveRate);
            long long b = std::max(a + 1, (long long)(a + perPx));
            if (a >= (long long)n) break;
            float peak = 0;
            for (long long i = std::max(0LL, a); i < b && i < (long long)n; i++) peak = std::max(peak, wave[i]);
            cols[x] = peak;
        }
        colWave = wave;
        colCount = n;
        colVersion = waveVersion;
        colStart = tl.viewStart_;
        colLen = tl.viewLen_;
    }

    void DrawWave() {
        int top = WaveTop(), waveH = WaveH();
        canvas.Fill(0, top + 1, w, top + waveH - 1, kRow);
        if (!tl.wave || tl.waveCount == 0) {
            if (tl.waveProgress >= 0) DrawLoading(top, waveH, tl.waveProgress, L"Building waveform");
            return;
        }
        if (tl.waveProgress >= 0) {
            wchar_t buf[64];
            swprintf(buf, 64, L"Building waveform  %.0f%%", tl.waveProgress * 100);
            RECT r = {w - S(150), top + S(2), w, top + waveH};
            canvas.Text(smallFont, buf, r, kLoadText, DT_LEFT | DT_TOP);
        }
        UpdateWaveCache();
        float mid = top + waveH / 2.f, half = waveH / 2.f - F(2), amp = half * waveGain;
        canvas.Sync();
        uint32_t c = ui::Px(kWave);
        for (int x = 0; x < w; x++) {
            if (cols[x] < 0) break;
            float hgt = std::max(1.f, std::min(half, cols[x] * amp));
            int y0 = std::max(0, (int)std::lround(mid - hgt)), y1 = std::min(h, (int)std::lround(mid + hgt));
            for (int y = y0; y < y1; y++) canvas.Row(y)[x] = c;
        }
    }

    // Colors the thumbnail/waveform rows under each segment, like a selection highlight.
    void TintSegments() {
        int top = RulerH(), bottom = SegTop();
        if (bottom <= top) return;
        for (size_t i = 0; i < SegCount(); i++) {
            const Segment& s = Seg(i);
            if (!s.enabled) continue;
            float x0 = std::max(0.f, T2X(s.start)), x1 = std::min((float)w, T2X(s.end));
            if (x1 <= x0) continue;
            canvas.FillF(x0, (float)top, x1, (float)bottom, SegColor(s), (int)i == tl.selected ? 70 : 40);
        }
    }

    void DrawSegments() {
        int top = SegTop(), segH = SegH();
        canvas.Fill(0, top, w, top + segH + S(4), kRow);
        HDC dc = canvas.Dc();
        int n = (int)SegCount(), sel = tl.selected;
        float radius = F(5), margin = F(20);
        for (int k = 0; k <= n; k++) {
            // Draw the selected segment last so its outline sits on top.
            int i = k < n ? k : sel;
            if (i < 0 || i >= n || (k < n && i == sel)) continue;
            const Segment& s = Seg(i);
            float x0 = T2X(s.start), x1 = T2X(s.end);
            if (x1 < 0 || x0 > w) continue;
            bool hi = i == sel || IsMarked(i);
            COLORREF c = SegColor(s);
            ui::RectF rect = {x0, top + F(3), x0 + std::max(F(3), x1 - x0), (float)(top + segH)};
            // Offscreen parts are clipped (keeps the pixel loops to what's visible at deep zoom).
            ui::RectF vis = {std::max(rect.x0, -margin), rect.y0, std::min(rect.x1, w + margin), rect.y1};
            if (s.enabled) {
                canvas.Round(vis, radius, hi ? ui::Lighten(c, 0.1f) : c, hi ? 255 : 200);
            } else {
                int period = std::max(4, S(8)), line = std::max(1, S(1));
                canvas.RoundShaded(vis, radius, 0, [&](int x, int y, COLORREF* pc, int* pa) {
                    *pc = c;
                    *pa = (x + y) % period < line ? 110 : 35;
                });
            }
            if (hi) canvas.Round(vis, radius, kWhite, 255, F(2));
            if (i == sel) {  // drag handles
                float hh = rect.y1 - rect.y0;
                for (float hx : {rect.x0, rect.x1})
                    canvas.Round({hx - F(2.5f), rect.y0 + hh * 0.25f, hx + F(2.5f), rect.y0 + hh * 0.75f}, F(2.5f),
                                 kWhite);
            }
            float width = rect.x1 - rect.x0;
            if (width > F(22)) {
                std::wstring label = std::to_wstring(i + 1);
                if (!s.name.empty()) label += L"  " + s.name;
                std::wstring withDur = label + L"   " + ui::StripZeroHours(Fmt(s.Duration()));
                SIZE full = ui::TextSize(dc, segFont, withDur);
                if (width > full.cx + F(24)) label = withDur;
                float lx = std::max(rect.x0, 0.f) + F(8), rx = std::min(rect.x1, (float)w) - F(6);
                if (rx - lx > F(6)) {
                    SIZE sz = label.size() == withDur.size() ? full : ui::TextSize(dc, segFont, label);
                    if (sz.cx <= rx - lx) {  // fits: the cheap path
                        canvas.TextAt(segFont, label, (int)lx, (int)((rect.y0 + rect.y1 - sz.cy) / 2), kWhite);
                    } else {
                        RECT r = {(LONG)lx, (LONG)rect.y0, (LONG)rx, (LONG)rect.y1};
                        canvas.Text(segFont, label, r, kWhite, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
                    }
                }
            }
        }
    }

    // Shows what the export will really contain: a keyframe cut also takes the part from the previous keyframe
    // (striped before the segment); a normal cut has no video until the next keyframe (darkened inside it).
    void ShadeCutMode() {
        const auto& kf = tl.keyframes;
        if (kf.empty() || tl.cutMode == L"smart") return;
        int top = SegTop() + S(3), segH = SegH() - S(3);
        int period = std::max(4, S(8)), wide = std::max(2, S(3));
        bool keyframeMode = tl.cutMode == L"keyframe";
        for (size_t i = 0; i < SegCount(); i++) {
            const Segment& s = Seg(i);
            if (!s.enabled) continue;
            if (keyframeMode) {
                auto it = std::upper_bound(kf.begin(), kf.end(), s.start + 0.0005);  // last keyframe <= start
                if (it == kf.begin()) continue;
                double k = *(it - 1);
                if (s.start - k < 0.02) continue;
                float x0 = T2X(k), x1 = T2X(s.start);
                if (x1 < 0 || x0 > w || x1 - x0 < 1) continue;
                canvas.Hatch((int)x0, (int)std::lround(top + segH * 0.3f), (int)std::ceil(x1),
                             (int)std::lround(top + segH * 0.7f), period, wide, kWhite, 150, kWhite, 40);
            } else {
                auto it = std::lower_bound(kf.begin(), kf.end(), s.start - 0.0005);  // first keyframe >= start
                double until = it == kf.end() ? s.end : std::min(s.end, *it);
                if (until - s.start < 0.02) continue;
                float x0 = T2X(s.start), x1 = T2X(until);
                if (x1 < 0 || x0 > w || x1 - x0 < 1) continue;
                canvas.Hatch((int)x0, top, (int)std::ceil(x1), top + segH, period, wide, kBlack, 170, kBlack, 110);
            }
        }
    }

    void DrawKeyframes() {
        const auto& kf = tl.keyframes;
        if (kf.empty()) return;
        auto a = std::lower_bound(kf.begin(), kf.end(), tl.viewStart_);
        auto b = std::lower_bound(a, kf.end(), tl.viewStart_ + tl.viewLen_);
        if (b - a > w / 4) return;  // too dense to be useful
        int top = SegTop(), lw = std::max(1, S(1));
        for (; a != b; ++a) {
            int x = (int)std::floor(T2X(*a));
            canvas.Fill(x, top + S(1), x + lw, top + S(7), kWhite, 150);
        }
    }

    // Timeline markers: a thin line through the rows with a small flag on the ruler's bottom edge, and the label
    // beside the flag when there's room before the next marker.
    void DrawMarkers() {
        if (!tl.markers || tl.markers->empty()) return;
        int rulerH = RulerH(), lw = std::max(1, S(1)), fw = S(7), fh = S(6);
        const auto& ms = *tl.markers;
        for (size_t i = 0; i < ms.size(); i++) {
            float x = T2X(ms[i].t);
            if (x < -fw || x > w + 2) continue;
            int xi = (int)std::floor(x);
            canvas.Fill(xi, rulerH - fh, xi + lw, h, kMarker, 170);
            canvas.Fill(xi, rulerH - fh, xi + fw, rulerH - fh / 2, kMarker);
            if (ms[i].label.empty()) continue;
            float next = i + 1 < ms.size() ? T2X(ms[i + 1].t) : (float)w;
            RECT r = {xi + fw + S(2), rulerH - fh - S(14), (LONG)std::min<float>(next - S(2), (float)w), rulerH - fh / 2};
            if (r.right - r.left > S(24)) canvas.Text(smallFont, ms[i].label, r, kMarker, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    void PaintTo(HDC dst) {
        if (!canvas.Ensure(w, h)) return;
        Paint();
        canvas.Present(dst);
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
        switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            PaintTo(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintTo((HDC)wp);
            return 0;
        case WM_SIZE:
            w = LOWORD(lp);
            h = HIWORD(lp);
            Invalidate();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(cursor);
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            OnButtonDown((short)LOWORD(lp), (short)HIWORD(lp), wp, false);
            return 0;
        case WM_MBUTTONDOWN:
            OnButtonDown((short)LOWORD(lp), (short)HIWORD(lp), wp, true);
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp), wp);
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            if (drag != Drag::None && m == dragButton) EndDrag(true);
            return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd && !util::KeepDragOnCaptureLoss()) EndDrag(false);
            return 0;
        case WM_MOUSELEAVE:
            tracking = false;
            hoverX = -1;
            Invalidate();
            if (cb.hoverEnd) cb.hoverEnd();
            return 0;
        case WM_MOUSEWHEEL:
            OnWheel((short)LOWORD(lp), (short)HIWORD(lp), GET_WHEEL_DELTA_WPARAM(wp), GET_KEYSTATE_WPARAM(wp));
            return 0;
        case WM_MOUSEHWHEEL:
            tl.viewStart_ += GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA * tl.viewLen_ * 0.15;
            ClampView();
            Invalidate();
            return 0;
        case WM_TIMER:
            if (wp == kWaveTimer) {
                KillTimer(hwnd, kWaveTimer);
                waveTimer = false;
                waveVersion++;
                Invalidate();
                return 0;
            }
            break;
        case WM_DESTROY:
            if (waveTimer) KillTimer(hwnd, kWaveTimer);
            waveTimer = false;
            canvas.Free();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            hwnd = nullptr;
            tl.hwnd_ = nullptr;
            break;
        }
        return DefWindowProcW(hwnd, m, wp, lp);
    }
};

// ------------------------------------------------------------------------------------------------ Timeline

bool Timeline::Create(HINSTANCE inst, HWND parent, TimelineCallbacks cb) {
    impl_ = std::make_unique<Impl>(*this, std::move(cb));
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = Impl::Proc;
    wc.hInstance = inst;
    wc.hCursor = nullptr;  // WM_SETCURSOR picks arrow / resize
    wc.lpszClassName = kTimelineClass;
    RegisterClassExW(&wc);  // fails harmlessly when already registered
    UINT d = parent ? GetDpiForWindow(parent) : 0;
    impl_->dpi = d ? (int)d : 96;
    impl_->MakeFonts();
    hwnd_ = CreateWindowExW(0, kTimelineClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr,
                            inst, impl_.get());
    return hwnd_ != nullptr;
}

Timeline::Timeline() = default;

Timeline::~Timeline() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
}

void Timeline::SetDpi(int dpi) {
    if (!impl_ || dpi <= 0 || dpi == impl_->dpi) return;
    impl_->dpi = dpi;
    impl_->MakeFonts();
    impl_->scaled.clear();
    impl_->lastWant.clear();
    Invalidate();
}

int Timeline::PreferredHeight() const {
    if (!impl_) return 0;
    const Impl& i = *impl_;
    return i.RulerH() + (i.ThumbRow() ? i.ThumbH() : 0) + (showWave ? i.WaveH() : 0) + i.SegH() + i.S(4);
}

void Timeline::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Timeline::WaveUpdated() {
    if (!impl_ || !hwnd_ || impl_->waveTimer) return;
    impl_->waveTimer = SetTimer(hwnd_, kWaveTimer, kWaveIntervalMs, nullptr) != 0;
    if (!impl_->waveTimer) {  // no timer available: repaint now
        impl_->waveVersion++;
        Invalidate();
    }
}

void Timeline::ResetView() {
    viewStart_ = 0;
    viewLen_ = std::max(duration, 0.01);
    Invalidate();
}

void Timeline::Zoom(double factor, double anchor) {
    if (!(factor > 0)) return;
    double hi = std::max(duration, 0.01), lo = std::min(std::min(0.05, duration), hi);
    double nl = std::clamp(viewLen_ / factor, lo, hi);
    viewStart_ = anchor - (anchor - viewStart_) * nl / viewLen_;
    viewLen_ = nl;
    viewStart_ = std::clamp(viewStart_, 0.0, std::max(0.0, duration - viewLen_));
    Invalidate();
}

void Timeline::ZoomTo(double start, double end) {
    double pad = (end - start) * 0.05;
    viewStart_ = start - pad;
    double hi = std::max(duration, 0.01);
    viewLen_ = std::clamp(end - start + 2 * pad, std::min(0.05, hi), hi);
    viewStart_ = std::clamp(viewStart_, 0.0, std::max(0.0, duration - viewLen_));
    Invalidate();
}

void Timeline::Follow(double t) {
    if (t < viewStart_ || t > viewStart_ + viewLen_) {
        viewStart_ = std::clamp(t - viewLen_ * 0.05, 0.0, std::max(0.0, duration - viewLen_));
        Invalidate();
    }
}

}  // namespace edit
