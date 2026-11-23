// Off-screen render test for the edit-mode Timeline and SegmentCards. Creates a never-activated popup beyond every
// monitor, fills both controls with fake data, renders them with PrintWindow into BMPs under bench\edit-tests\ui\,
// drives a few interactions by calling the controls' own handlers directly (no synthesized input), and times paints.
#define _ALLOW_KEYWORD_MACROS
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../src/util.h"

// White-box access to the controls' private Impl so the test can call their mouse handlers directly.
#define private public
#include "../../src/edit/timeline.h"
#include "../../src/edit/segcards.h"
#include "../../src/edit/timeline.cpp"
#include "../../src/edit/segcards.cpp"
#undef private

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// Stand-ins for common.cpp (written elsewhere).
namespace edit {
std::wstring Fmt(double s, bool millis) {
    if (!std::isfinite(s)) s = 0;
    bool neg = s < 0;
    long long total = std::llround(std::fabs(s) * 1000);
    long long h = total / 3600000, m = total / 60000 % 60, sec = total / 1000 % 60, ms = total % 1000;
    wchar_t buf[64];
    if (millis) swprintf(buf, 64, L"%s%02lld:%02lld:%02lld.%03lld", neg ? L"-" : L"", h, m, sec, ms);
    else swprintf(buf, 64, L"%s%02lld:%02lld:%02lld", neg ? L"-" : L"", h, m, sec);
    return buf;
}
const std::vector<uint32_t>& Palette() {
    static const std::vector<uint32_t> p = {RGB(0x3b, 0x82, 0xf6), RGB(0x22, 0xc5, 0x5e), RGB(0xf5, 0x9e, 0x0b),
                                            RGB(0xec, 0x48, 0x99), RGB(0x8b, 0x5c, 0xf6), RGB(0x14, 0xb8, 0xa6),
                                            RGB(0xef, 0x44, 0x44), RGB(0x84, 0xcc, 0x16)};
    return p;
}
}  // namespace edit

using namespace edit;

namespace {

const wchar_t kOutDir[] = L"H:\\Claude\\fastplay\\bench\\edit-tests\\ui\\";
int failures = 0;

void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

struct FakeThumbs : TimelineThumbs {
    bool ready = true;
    int wants = 0;
    size_t lastWantCount = 0;
    std::map<long long, std::shared_ptr<Thumb>> cache;

    std::shared_ptr<const Thumb> Get(double t, double tol) override {
        if (!ready) return nullptr;
        double snapped = std::floor(t / 4.17) * 4.17;  // like a keyframe decoder: nearest earlier keyframe
        if (std::fabs(snapped - t) > tol) return nullptr;
        long long key = std::llround(snapped * 1000);
        auto& slot = cache[key];
        if (!slot) {
            slot = std::make_shared<Thumb>();
            slot->w = 160;
            slot->h = 90;
            slot->time = snapped;
            slot->px.resize(160 * 90);
            double hue = std::fmod(snapped / 60.0, 1.0);
            for (int y = 0; y < 90; y++)
                for (int x = 0; x < 160; x++) {
                    int r = (int)(120 + 120 * std::sin(6.283 * hue)) * x / 160;
                    int g = (int)(120 + 120 * std::sin(6.283 * (hue + 0.33))) * (90 - y) / 90;
                    int b = (int)(120 + 120 * std::sin(6.283 * (hue + 0.66)));
                    slot->px[y * 160 + x] = (std::clamp(r, 0, 255) << 16) | (std::clamp(g, 0, 255) << 8) | std::clamp(b, 0, 255);
                }
        }
        return slot;
    }
    void Want(const std::vector<double>& times, double, double) override {
        wants++;
        lastWantCount = times.size();
    }
};

bool SaveBmp(HDC mem, HBITMAP bmp, int w, int h, const std::wstring& name) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint32_t> px((size_t)w * h);
    GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + (DWORD)(px.size() * 4);
    FILE* f = _wfopen((std::wstring(kOutDir) + name + L".bmp").c_str(), L"wb");
    if (!f) return false;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
    fwrite(px.data(), 4, px.size(), f);
    fclose(f);
    return true;
}

void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

// Renders one window with PrintWindow and writes <name>.bmp.
void Capture(HWND hwnd, const std::wstring& name, bool fullContent = false) {
    UpdateWindow(hwnd);
    Pump();
    RECT r;
    GetWindowRect(hwnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    // PW_RENDERFULLCONTENT returns DWM's last composed frame, which lags behind a repaint that just happened for a
    // child window; flag 0 renders now through WM_PRINT -> WM_PRINTCLIENT (both controls handle it).
    BOOL ok = PrintWindow(hwnd, mem, fullContent ? PW_RENDERFULLCONTENT : 0);
    SelectObject(mem, old);
    bool saved = SaveBmp(mem, bmp, w, h, name);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    printf("capture %-22ls %dx%d printwindow=%d saved=%d\n", name.c_str(), w, h, ok, saved);
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) {
        RECT r;
        GetClientRect(h, &r);
        SetDCBrushColor((HDC)w, RGB(24, 24, 26));
        FillRect((HDC)w, &r, (HBRUSH)GetStockObject(DC_BRUSH));
        return 1;
    }
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

std::vector<Segment> MakeSegments() {
    auto seg = [](double a, double b, const wchar_t* name, int color, bool on = true, const wchar_t* tags = L"") {
        Segment s;
        s.start = a;
        s.end = b;
        s.name = name;
        s.color = color;
        s.enabled = on;
        s.tags = tags;
        return s;
    };
    return {seg(10, 45, L"Intro", 0), seg(62.3, 130.8, L"Boss fight", 1, true, L"boss=1, best"),
            seg(200, 260, L"", 2, false), seg(300, 310.5, L"Tiny", 3), seg(420, 590, L"Finale and credits", 4),
            seg(592, 598, L"", 5), seg(150.2, 151.9, L"Blink", 6)};
}

}  // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CreateDirectoryW(L"H:\\Claude\\fastplay\\bench\\edit-tests", nullptr);
    CreateDirectoryW(kOutDir, nullptr);
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FastPlay.EditUiTestHost";
    RegisterClassExW(&wc);
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200;
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"edit ui test",
                                WS_POPUP | WS_CLIPCHILDREN, x, y, 1200, 700, nullptr, nullptr, inst, nullptr);
    ShowWindow(host, SW_SHOWNOACTIVATE);

    // ---- data ----
    std::vector<Segment> segs = MakeSegments();
    std::set<int> marked = {1, 3, 4};
    std::vector<double> keyframes;
    for (double t = 0; t < 600; t += 4.17) keyframes.push_back(t);
    std::vector<float> wave(600 * kWaveRate);
    for (size_t i = 0; i < wave.size(); i++) {
        double t = i / (double)kWaveRate;
        double env = (0.25 + 0.2 * std::sin(t / 23.0)) * (0.3 + 0.7 * std::fabs(std::sin(t * 1.7) * std::sin(t * 0.31))) *
                     (t > 300 && t < 340 ? 0.1 : 1);
        double noise = std::fabs(std::sin(i * 12.9898) * 43758.5453 - std::floor(std::sin(i * 12.9898) * 43758.5453));
        wave[i] = (float)std::max(0.0, env * (0.5 + 0.5 * noise));
    }
    FakeThumbs thumbs;

    // ---- timeline ----
    Timeline tl;
    int seeks = 0, begins = 0, ends = 0, hovers = 0, hoverEnds = 0;
    std::vector<std::pair<int, uint32_t>> selects;
    int nextColor = 7;
    TimelineCallbacks tcb;
    tcb.seek = [&](double t) { seeks++; tl.position = t; };
    tcb.hover = [&](double, POINT) { hovers++; };
    tcb.hoverEnd = [&] { hoverEnds++; };
    tcb.selectSegment = [&](int i, uint32_t mods) {
        selects.push_back({i, mods});
        if (mods & MK_CONTROL) marked.count(i) ? (void)marked.erase(i) : (void)marked.insert(i);
        tl.selected = i;
    };
    tcb.editBegin = [&] { begins++; };
    tcb.editEnd = [&] { ends++; };
    tcb.newSegment = [&](double a, double b) {
        Segment s;
        s.start = a;
        s.end = b;
        s.color = nextColor++;
        return s;
    };
    tcb.timeText = [](double t) { return Fmt(t); };
    Check(tl.Create(inst, host, tcb), "timeline created");
    tl.SetDpi(96);
    tl.duration = 600;
    tl.position = 125.4;
    tl.segments = &segs;
    tl.selected = 1;
    tl.marked = &marked;
    tl.keyframes = keyframes;
    tl.wave = wave.data();
    tl.waveCount = wave.size();
    tl.thumbs = &thumbs;
    tl.hasMedia = true;
    tl.ResetView();
    SetWindowPos(tl.Hwnd(), nullptr, 0, 0, 1200, tl.PreferredHeight(), SWP_NOZORDER | SWP_NOACTIVATE);
    Check(tl.PreferredHeight() == 20 + 50 + 34 + 34 + 4, "preferred height at 96 dpi");

    // ---- cards ----
    SegmentCards cards;
    int clicks = 0, toggles = 0, edits = 0, reorders = 0;
    SegmentCardsCallbacks ccb;
    ccb.itemClicked = [&](int, uint32_t) { clicks++; };
    ccb.toggleRequested = [&](int) { toggles++; };
    ccb.editRequested = [&](int) { edits++; };
    ccb.reorder = [&](int, int) { reorders++; };
    Check(cards.Create(inst, host, ccb), "cards created");
    cards.SetDpi(96);
    cards.segments = &segs;
    cards.marked = &marked;
    cards.current = 1;
    cards.fmt = [](double t) { return Fmt(t); };
    cards.warning = [](int i) { return i == 1 ? std::wstring(L"starts 2.13 s early") : std::wstring(); };
    SetWindowPos(cards.Hwnd(), nullptr, 0, 200, 420, 300, SWP_NOZORDER | SWP_NOACTIVATE);
    cards.Refresh();
    Pump();

    Capture(tl.Hwnd(), L"tl_full_dwm", true);
    Capture(tl.Hwnd(), L"tl_full");
    Capture(cards.Hwnd(), L"cards");

    // Zoomed to a segment: keyframe marks and the keyframe-cut lead-in stripes show.
    tl.ZoomTo(segs[1].start - 10, segs[1].start + 30);
    Capture(tl.Hwnd(), L"tl_zoom_keyframe");
    tl.cutMode = L"exact";
    Capture(tl.Hwnd(), L"tl_zoom_exact");
    tl.cutMode = L"keyframe";

    // Wheel zoom around a point (the handler's own math), then reset.
    tl.ResetView();
    double anchor = 300;
    tl.Zoom(std::pow(1.35, 3), anchor);
    Check(tl.ViewStart() <= anchor && tl.ViewStart() + tl.ViewLength() >= anchor, "zoom keeps the anchor in view");
    Check(std::fabs(tl.ViewLength() - 600 / std::pow(1.35, 3)) < 1e-6, "zoom factor applied");
    tl.Zoom(1e9, anchor);
    Check(std::fabs(tl.ViewLength() - 0.05) < 1e-9, "zoom clamps at 0.05 s");
    tl.Zoom(1e-9, anchor);
    Check(tl.ViewStart() == 0 && std::fabs(tl.ViewLength() - 600) < 1e-9, "zoom out clamps to the file");
    tl.ZoomTo(100, 200);
    tl.Follow(500);
    Check(tl.ViewStart() <= 500 && tl.ViewStart() + tl.ViewLength() >= 500, "follow pages to the playhead");
    tl.ResetView();

    // Loading state: no waveform yet, keyframe scan running, thumbnails not decoded.
    {
        thumbs.ready = false;
        auto savedKf = tl.keyframes;
        tl.wave = nullptr;
        tl.waveCount = 0;
        tl.waveProgress = 0.3;
        tl.keyframeProgress = 0.42;
        tl.keyframes.clear();
        Capture(tl.Hwnd(), L"tl_loading");
        tl.wave = wave.data();
        tl.waveCount = wave.size();
        tl.waveProgress = 0.8;
        tl.keyframeProgress = -1;
        tl.keyframes = savedKf;
        thumbs.ready = true;
        Capture(tl.Hwnd(), L"tl_wave_partial");
        tl.waveProgress = -1;
    }

    // ---- interactions through the timeline's own handlers ----
    Timeline::Impl& ti = *tl.impl_;
    int segY = ti.SegTop() + ti.SegH() / 2;
    auto X = [&](double t) { return (int)std::lround(ti.T2X(t)); };
    tl.ZoomTo(0, 600);
    tl.ResetView();

    // Click on the ruler: seek.
    seeks = 0;
    ti.OnButtonDown(X(77), ti.RulerH() / 2, 0, false);
    ti.OnMouseMove(X(88), ti.RulerH() / 2, MK_LBUTTON);
    ti.EndDrag(true);
    Check(seeks == 2 && std::fabs(tl.position - 88) < 1, "ruler click + scrub seeks");

    // Drag on empty segment row creates a segment.
    size_t before = segs.size();
    begins = ends = 0;
    ti.OnButtonDown(X(160), segY, 0, false);
    ti.OnMouseMove(X(170), segY, MK_LBUTTON);
    ti.OnMouseMove(X(185), segY, MK_LBUTTON);
    ti.EndDrag(true);
    Check(segs.size() == before + 1, "drag on empty row creates a segment");
    Check(std::fabs(segs.back().start - 160) < 1 && std::fabs(segs.back().end - 185) < 1, "created segment spans the drag");
    Check(begins == 1 && ends == 1, "create drag wrapped in editBegin/editEnd");
    Check(tl.selected == (int)segs.size() - 1, "created segment selected");

    // Click (no drag) on empty row only seeks.
    before = segs.size();
    begins = ends = 0;
    ti.OnButtonDown(X(280), segY, 0, false);
    ti.EndDrag(true);
    Check(segs.size() == before && begins == 0 && ends == 0 && std::fabs(tl.position - 280) < 1, "click on empty row seeks only");

    // Move segment 0 by its body.
    double len0 = segs[0].Duration();
    ti.OnButtonDown(X(25), segY, 0, false);
    ti.OnMouseMove(X(35), segY, MK_LBUTTON);
    ti.EndDrag(true);
    Check(std::fabs(segs[0].start - 20) < 1 && std::fabs(segs[0].Duration() - len0) < 1e-9, "body drag moves segment");
    Check(begins == 1 && ends == 1, "move wrapped in editBegin/editEnd");

    // Resize segment 0's end edge with Shift snapping to a keyframe.
    tl.selected = 0;
    ti.OnButtonDown(X(segs[0].end), segY, 0, false);
    Check(ti.drag == Timeline::Impl::Drag::Edge, "edge grab starts a resize");
    ti.OnMouseMove(X(70.6), segY, MK_LBUTTON | MK_SHIFT);
    ti.EndDrag(true);
    double snapped = 0;
    for (double k : keyframes) if (std::fabs(k - 70.6) < std::fabs(snapped - 70.6)) snapped = k;
    Check(std::fabs(segs[0].end - snapped) < 1e-9, "shift resize snaps to the nearest keyframe");

    // Ctrl+click marks without dragging.
    selects.clear();
    ti.OnButtonDown(X(450), segY, MK_CONTROL, false);
    Check(ti.drag == Timeline::Impl::Drag::None && !selects.empty() && selects.back().first == 4 &&
              selects.back().second == MK_CONTROL, "ctrl+click selects with modifiers, no drag");

    // Middle-drag pans when zoomed in.
    tl.ZoomTo(100, 200);
    double vs = tl.ViewStart();
    ti.OnButtonDown(600, segY, 0, true);
    ti.OnMouseMove(500, segY, MK_MBUTTON);
    Check(tl.ViewStart() > vs, "middle drag pans");
    ti.EndDrag(true);

    // Shift+wheel pans, plain wheel zooms around the cursor.
    POINT sp = {600, segY};
    ClientToScreen(tl.Hwnd(), &sp);
    vs = tl.ViewStart();
    ti.OnWheel(sp.x, sp.y, -WHEEL_DELTA, MK_SHIFT);
    Check(tl.ViewStart() > vs, "shift+wheel pans");
    double tAt = ti.X2T(600), len = tl.ViewLength();
    ti.OnWheel(sp.x, sp.y, WHEEL_DELTA, 0);
    Check(tl.ViewLength() < len && std::fabs(ti.X2T(600) - tAt) < 1e-6, "wheel zooms around the cursor");

    // Hover sets the hover line and fires the callback.
    hovers = 0;
    ti.OnMouseMove(700, ti.RulerH() + 5, 0);
    Check(hovers == 1 && ti.hoverX == 700, "hover callback");
    Pump();  // the cursor isn't really over the window: WM_MOUSELEAVE clears the hover line again
    Check(ti.hoverX == -1 && hoverEnds > 0, "mouse leave clears the hover");
    ti.hoverX = 700;  // put it back for the picture
    Capture(tl.Hwnd(), L"tl_hover_zoomed");
    ti.hoverX = -1;

    // WaveUpdated coalesces into a timer.
    tl.WaveUpdated();
    tl.WaveUpdated();
    Check(ti.waveTimer, "WaveUpdated arms one timer");
    Sleep(100);
    Pump();
    Check(!ti.waveTimer, "wave timer fired and stopped");

    // Paint cost at 1920 px wide, full view, playing (only the playhead moves).
    tl.ResetView();
    SetWindowPos(tl.Hwnd(), nullptr, 0, 0, 1920, tl.PreferredHeight(), SWP_NOZORDER | SWP_NOACTIVATE);
    Pump();
    ti.canvas.Ensure(ti.w, ti.h);
    ti.Paint();
    auto t0 = std::chrono::steady_clock::now();
    const int kPaints = 200;
    for (int i = 0; i < kPaints; i++) {
        tl.position = i * 3.0;
        ti.Paint();
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kPaints;
    printf("timeline paint 1920x%d: %.3f ms\n", ti.h, ms);
    {
        auto stage = [&](const char* name, const std::function<void()>& f) {
            auto s0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kPaints; i++) f();
            printf("  %-10s %.3f ms\n", name,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count() / kPaints);
        };
        stage("clear", [&] { ti.canvas.Fill(0, 0, ti.w, ti.h, theme::Bg); });
        stage("ruler", [&] { ti.DrawRuler(); });
        stage("thumbs", [&] { ti.DrawThumbs(); });
        stage("wave", [&] { ti.DrawWave(); });
        stage("tint", [&] { ti.TintSegments(); });
        stage("segments", [&] { ti.DrawSegments(); });
        stage("cutmode", [&] { ti.ShadeCutMode(); });
        stage("keyframes", [&] { ti.DrawKeyframes(); });
        stage("bubble", [&] { ti.Bubble(L"00:01:02.000", 300, theme::CardSelected, theme::Text); });
    }
    tl.ZoomTo(100, 140);
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPaints; i++) {
        tl.position = 100 + i * 0.2;
        ti.Paint();
    }
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kPaints;
    printf("timeline paint zoomed: %.3f ms\n", ms);
    SetWindowPos(tl.Hwnd(), nullptr, 0, 0, 1200, tl.PreferredHeight(), SWP_NOZORDER | SWP_NOACTIVATE);

    // ---- cards interactions ----
    SegmentCards::Impl& ci = *cards.impl_;
    int cardY = ci.Top() + ci.ItemH() / 2;
    ci.OnDown({100, cardY});
    ci.OnUp(0);
    Check(clicks == 1, "card click");
    RECT tog = ci.ToggleRect(ci.ItemRect(0));
    ci.OnDown({(tog.left + tog.right) / 2, (tog.top + tog.bottom) / 2});
    Check(toggles == 1, "toggle click");
    ci.OnDown({100, cardY});
    ci.OnMove({100, cardY + ci.ItemH() * 2}, MK_LBUTTON);
    Check(ci.dragging && ci.dropAt == 3, "card drag shows a drop line");
    Capture(cards.Hwnd(), L"cards_dragging");
    ci.OnUp(0);
    Check(reorders == 1, "card drag reorders");
    cards.Refresh();  // the timeline drag above added a segment
    cards.EnsureVisible((int)segs.size() - 1);
    Check(ci.scrollY == ci.MaxScroll(), "EnsureVisible scrolls to the last card");
    cards.current = (int)segs.size() - 1;
    Capture(cards.Hwnd(), L"cards_scrolled");
    ci.OnWheel(WHEEL_DELTA);
    Check(ci.scrollY == ci.MaxScroll() - ci.ItemH(), "wheel scrolls one card");

    // ---- 144 dpi ----
    tl.SetDpi(144);
    cards.SetDpi(144);
    cards.current = 1;
    tl.ResetView();
    SetWindowPos(tl.Hwnd(), nullptr, 0, 0, 1200, tl.PreferredHeight(), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(cards.Hwnd(), nullptr, 0, 260, 520, 420, SWP_NOZORDER | SWP_NOACTIVATE);
    cards.Refresh();
    cards.EnsureVisible(0);
    Check(tl.PreferredHeight() == 30 + 75 + 51 + 51 + 6, "preferred height at 144 dpi");
    Capture(tl.Hwnd(), L"tl_144dpi");
    Capture(cards.Hwnd(), L"cards_144dpi");

    // No media: plain background.
    tl.hasMedia = false;
    Capture(tl.Hwnd(), L"tl_nomedia");

    DestroyWindow(host);
    Pump();
    Check(tl.Hwnd() == nullptr && cards.Hwnd() == nullptr, "child windows destroyed with the host");
    printf("thumb wants: %d (last %zu times)\n", thumbs.wants, thumbs.lastWantCount);
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
