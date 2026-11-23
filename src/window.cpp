// Main window, input, actions, menus, fullscreen, tray.
#include "app.h"
#include "controls.h"
#include "dialogs.h"
#include "edit/editor.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>

namespace {

enum : UINT {
    kTimerUi = 1, kTimerIdle = 2, kTimerThumbs = 3, kTimerStats = 4, kTimerFindChild = 5, kTimerBenchExit = 9,
};

struct TrackInfo {
    std::string type, title, lang, codec;
    int64_t id = 0;
    bool selected = false, external = false;
};

std::vector<TrackInfo> GetTracks(mpv_handle* h) {
    std::vector<TrackInfo> r;
    mpv_node n;
    if (!h || g_mpv.mpv_get_property(h, "track-list", MPV_FORMAT_NODE, &n) < 0) return r;
    if (n.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < n.u.list->num; i++) {
            mpv_node& t = n.u.list->values[i];
            if (t.format != MPV_FORMAT_NODE_MAP) continue;
            TrackInfo ti;
            for (int k = 0; k < t.u.list->num; k++) {
                const char* key = t.u.list->keys[k];
                mpv_node& v = t.u.list->values[k];
                if (v.format == MPV_FORMAT_STRING) {
                    if (!strcmp(key, "type")) ti.type = v.u.string;
                    else if (!strcmp(key, "title")) ti.title = v.u.string;
                    else if (!strcmp(key, "lang")) ti.lang = v.u.string;
                    else if (!strcmp(key, "codec")) ti.codec = v.u.string;
                } else if (v.format == MPV_FORMAT_INT64 && !strcmp(key, "id")) ti.id = v.u.int64;
                else if (v.format == MPV_FORMAT_FLAG) {
                    if (!strcmp(key, "selected")) ti.selected = v.u.flag != 0;
                    else if (!strcmp(key, "external")) ti.external = v.u.flag != 0;
                }
            }
            r.push_back(ti);
        }
    }
    g_mpv.mpv_free_node_contents(&n);
    return r;
}

std::wstring TrackLabel(const TrackInfo& t) {
    std::wstring s = std::to_wstring(t.id) + L": ";
    if (!t.title.empty()) s += util::Wide(t.title);
    if (!t.lang.empty()) s += (t.title.empty() ? L"" : L" ") + std::wstring(L"[") + util::Wide(t.lang) + L"]";
    if (!t.codec.empty()) s += L" (" + util::Wide(t.codec) + L")";
    if (t.external) s += L" external";
    std::wstring esc;  // '&' would become a mnemonic
    for (wchar_t c : s) {
        if (c == L'&') esc += L'&';
        esc += c;
    }
    return esc;
}

bool IsRepeatable(Act a) {
    switch (a) {
    case Act::FrameStep: case Act::FrameBack: case Act::SeekFwdSmall: case Act::SeekBackSmall: case Act::SeekFwdMed:
    case Act::SeekBackMed: case Act::SeekFwdLarge: case Act::SeekBackLarge: case Act::KeyframeFwd:
    case Act::KeyframeBack: case Act::SpeedUp: case Act::SpeedDown: case Act::AudioDelayUp: case Act::AudioDelayDown:
    case Act::SubDelayUp: case Act::SubDelayDown: case Act::PanLeft: case Act::PanRight: case Act::PanUp:
    case Act::PanDown: case Act::ZoomIn: case Act::ZoomOut: case Act::VolUp: case Act::VolDown:
    case Act::WidthUp: case Act::WidthDown: case Act::HeightUp: case Act::HeightDown: case Act::PanUpLeft:
    case Act::PanUpRight: case Act::PanDownLeft: case Act::PanDownRight: case Act::SubLinePrev: case Act::SubLineNext:
        return true;
    default:
        return false;
    }
}

// The window's monitor. Test runs get a same-sized "monitor" right of the desktop, so fullscreen stays off-screen.
MONITORINFO WindowMonitor(HWND h) {
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    if (util::TestOffScreen()) {
        int dx = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200 - mi.rcMonitor.left;
        OffsetRect(&mi.rcMonitor, dx, 0);
        OffsetRect(&mi.rcWork, dx, 0);
    }
    return mi;
}

const DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
const DWORD kBorderlessStyle = WS_POPUP | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN;

}  // namespace

// ------------------------------------------------------------------------------------------------ entry point

static std::vector<std::wstring> SplitArgs(const wchar_t* cmd) {
    // CommandLineToArgvW would pull shell32 into start-up; this handles quotes the same way for our needs.
    std::vector<std::wstring> out;
    const wchar_t* p = cmd;
    bool first = true;
    while (*p) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        std::wstring a;
        bool q = false;
        while (*p && (q || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'"') q = !q;
            else a += *p;
            p++;
        }
        if (!first) out.push_back(a);
        first = false;
    }
    return out;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    App app;
    g_app = &app;
    return app.Run(inst, SplitArgs(GetCommandLineW()));
}

// ------------------------------------------------------------------------------------------------ windows

int App::BarHeight() const { return ControlBar::HeightFor(dpi_); }

void App::CreateWindows(bool startHidden) {
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst_;
    wc.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    std::wstring mainClass = util::MainClassName();
    wc.lpszClassName = mainClass.c_str();
    RegisterClassExW(&wc);

    WNDCLASSEXW vc = {sizeof vc};
    vc.style = CS_DBLCLKS;
    vc.lpfnWndProc = VideoProc;
    vc.hInstance = inst_;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    vc.lpszClassName = L"FastPlay.Video";
    RegisterClassExW(&vc);

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    taskbarButtonMsg_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
    onTop_ = s_.onTop;
    captionOn_ = s_.showCaption;
    controlsOn_ = s_.showControls;

    // Initial size: last windowed size, or 960x540 video area; centred on the monitor under the cursor.
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    UINT mdx = 96, mdy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = (GetDpiForMonitorFn)GetProcAddress(sh, "GetDpiForMonitor")) fn(mon, 0, &mdx, &mdy);
    }
    dpi_ = (int)mdx;
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    int cw = s_.winW > 0 ? s_.winW : MulDiv(960, dpi_, 96);
    int ch = s_.winH > 0 ? s_.winH : MulDiv(540, dpi_, 96) + (controlsOn_ ? BarHeight() : 0);
    DWORD style = captionOn_ ? kWindowedStyle : kBorderlessStyle;
    RECT r = {0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi_);
    int w = r.right - r.left, h = r.bottom - r.top;
    const RECT& wa = mi.rcWork;
    int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
    if (util::TestOffScreen()) {
        // Benchmarks/tests: off every screen, never activated.
        x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200;
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    }
    DWORD ex = WS_EX_ACCEPTFILES | (onTop_ ? WS_EX_TOPMOST : 0);
    hwnd_ = CreateWindowExW(ex, wc.lpszClassName, L"FastPlay", style, x, y, w, h, nullptr, nullptr, inst_, this);
    dpi_ = (int)GetDpiForWindow(hwnd_);
    video_ = CreateWindowExW(0, vc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
                             10, 10, hwnd_, nullptr, inst_, this);
    barWnd_ = new ControlBar();
    barWnd_->Create(inst_, hwnd_, this);
    thumbWnd_ = new ThumbPopup();
    thumbWnd_->Create(inst_, hwnd_);
    Layout();
    UpdateTitle();
    if (startHidden) {
        inTray_ = true;
        AddTray();
    } else {
        ShowWindow(hwnd_, util::TestMode() ? SW_SHOWNOACTIVATE : SW_SHOW);
        UpdateWindow(hwnd_);
    }
    SetTimer(hwnd_, kTimerUi, 100, nullptr);
    SetTimer(hwnd_, kTimerIdle, 250, nullptr);
}

void App::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (EditActive()) {
        // Edit mode lays out the video and its own panels; FastPlay's control bar steps aside.
        if (!barWnd_->Overlay()) ShowWindow(barWnd_->Hwnd(), SW_HIDE);
        editor_->Layout(rc);
        return;
    }
    if (editor_) editor_->Layout(rc);  // hides the edit windows
    int barH = (controlsOn_ && !fullscreen_) ? BarHeight() : 0;
    MoveWindow(video_, 0, 0, rc.right, std::max(0L, rc.bottom - barH), TRUE);
    if (!barWnd_->Overlay()) {
        if (barH) {
            MoveWindow(barWnd_->Hwnd(), 0, rc.bottom - barH, rc.right, barH, TRUE);
            ShowWindow(barWnd_->Hwnd(), SW_SHOWNA);
        } else {
            ShowWindow(barWnd_->Hwnd(), SW_HIDE);
        }
    }
}

void App::ShowBarOverlay(bool show) {
    if (!fullscreen_ || show == barOverlayShown_) return;
    barOverlayShown_ = show;
    if (show) {
        MONITORINFO mi = WindowMonitor(hwnd_);
        int h = BarHeight();
        SetWindowPos(barWnd_->Hwnd(), HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.bottom - h,
                     mi.rcMonitor.right - mi.rcMonitor.left, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(barWnd_->Hwnd(), SW_HIDE);
        thumbWnd_->Hide();
    }
}

void App::SetFullscreen(bool on) {
    if (on == fullscreen_) return;
    fullscreen_ = on;
    if (on) {
        savedPlacement_.length = sizeof savedPlacement_;
        GetWindowPlacement(hwnd_, &savedPlacement_);
        GetWindowRect(hwnd_, &savedRect_);
        MONITORINFO mi = WindowMonitor(hwnd_);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        barWnd_->SetOverlay(true, hwnd_);
        barOverlayShown_ = false;
        const RECT& m = mi.rcMonitor;
        SetWindowPos(hwnd_, onTop_ ? HWND_TOPMOST : HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        barOverlayShown_ = false;
        barWnd_->SetOverlay(false, hwnd_);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (captionOn_ ? kWindowedStyle : kBorderlessStyle) | WS_VISIBLE);
        WINDOWPLACEMENT wp = savedPlacement_;
        if (wp.showCmd == SW_SHOWNORMAL) wp.showCmd = SW_SHOWNOACTIVATE;  // already visible: never (re)activate
        SetWindowPlacement(hwnd_, &wp);
        SetWindowPos(hwnd_, onTop_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        // Windows clamps a restored placement onto a monitor, which put a hidden test copy on the user's screen.
        if (util::TestOffScreen())
            SetWindowPos(hwnd_, nullptr, savedRect_.left, savedRect_.top, savedRect_.right - savedRect_.left,
                         savedRect_.bottom - savedRect_.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    thumbWnd_->Hide();
    Layout();
    cursorHidden_ = false;
    lastMouseMove_ = GetTickCount();
}

void App::SetCaption(bool on) {
    captionOn_ = on;
    if (fullscreen_) return;
    // Keep the client area (the video) where it is while the frame changes.
    RECT rc;
    GetClientRect(hwnd_, &rc);
    MapWindowPoints(hwnd_, nullptr, (POINT*)&rc, 2);
    DWORD style = (on ? kWindowedStyle : kBorderlessStyle) | (IsWindowVisible(hwnd_) ? WS_VISIBLE : 0);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    AdjustWindowRectExForDpi(&rc, style, FALSE, (DWORD)GetWindowLongPtrW(hwnd_, GWL_EXSTYLE), dpi_);
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    Layout();
}

void App::SetControls(bool on) {
    if (on == controlsOn_) return;
    controlsOn_ = on;
    if (fullscreen_) return;
    // Grow/shrink the window by the bar so the video keeps its size.
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    if (!IsZoomed(hwnd_)) {
        int d = on ? BarHeight() : -BarHeight();
        SetWindowPos(hwnd_, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top + d,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout();
}

void App::SetOnTop(bool on) {
    onTop_ = on;
    SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::ResizeToVideo(double scale, bool fitOnly) {
    if (fullscreen_ || vidW_ <= 0 || vidH_ <= 0 || !hwnd_) return;
    if (IsZoomed(hwnd_)) {
        if (fitOnly) return;
        ShowWindow(hwnd_, SW_RESTORE);
    }
    MONITORINFO mi = WindowMonitor(hwnd_);
    const RECT& wa = mi.rcWork;
    int barH = controlsOn_ ? BarHeight() : 0;
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    RECT frame = {0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, style, FALSE, ex, dpi_);
    int frameW = frame.right - frame.left, frameH = frame.bottom - frame.top;
    double f = scale;
    if (fitOnly) {
        // MPC-HC style auto-fit: the window takes autofit_percent of the work area (video aspect kept).
        double maxW = (wa.right - wa.left) * s_.autofitPercent / 100.0 - frameW;
        double maxH = (wa.bottom - wa.top) * s_.autofitPercent / 100.0 - frameH - barH;
        f = std::min(maxW / vidW_, maxH / vidH_);
    }
    int cw = (int)std::lround(vidW_ * f), ch = (int)std::lround(vidH_ * f) + barH;
    int w = cw + frameW, h = ch + frameH;
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    int x = (wr.left + wr.right) / 2 - w / 2, y = (wr.top + wr.bottom) / 2 - h / 2;
    if (!util::TestMode()) {
        x = std::clamp(x, (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
        y = std::clamp(y, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - h));
    } else {
        x = wr.left;
        y = wr.top;
    }
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    fitPending_ = false;
}

void App::NoteMouseActivity() {
    POINT p;
    GetCursorPos(&p);
    if (p.x == lastMousePt_.x && p.y == lastMousePt_.y) return;  // spurious WM_MOUSEMOVE
    lastMousePt_ = p;
    lastMouseMove_ = GetTickCount();
    if (cursorHidden_) {
        cursorHidden_ = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    if (fullscreen_) {
        MONITORINFO mi = WindowMonitor(hwnd_);
        bool nearBottom = p.y >= mi.rcMonitor.bottom - BarHeight() * 3 / 2;
        ShowBarOverlay(nearBottom || barWnd_->MouseInside());
    }
}

void App::UpdateCursor() {
    // Hide the pointer over the video after 2 s without movement (MPC-HC: "hide mouse pointer").
    if (cursorHidden_ || bar_.paused || GetTickCount() - lastMouseMove_ < 2000) return;
    POINT p;
    GetCursorPos(&p);
    HWND under = WindowFromPoint(p);
    if (under != video_ && under != mpvChild_ && under != hwnd_) return;
    cursorHidden_ = true;
    SetCursor(nullptr);
    if (fullscreen_ && !barWnd_->MouseInside()) ShowBarOverlay(false);
}

// ------------------------------------------------------------------------------------------------ window procs

LRESULT CALLBACK App::MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* self = (App*)((CREATESTRUCTW*)l)->lpCreateParams;
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    }
    auto* self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->OnMain(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK App::VideoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto* self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->OnVideo(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void App::HandleChord(uint32_t chord) {
    Act a = keys_.Lookup(chord);
    if (a != Act::None) DoAction(a);
}

LRESULT App::OnVideo(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PARENTNOTIFY:
        if (LOWORD(w) == WM_CREATE) PostMessageW(hwnd_, WM_APP_DISABLE_CHILD, 0, l);
        break;
    case WM_NCHITTEST:
        if (!captionOn_ && !fullscreen_ && !IsZoomed(hwnd_)) {
            // Borderless: let the main window offer resize edges along the video's border.
            POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
            RECT wr;
            GetWindowRect(hwnd_, &wr);
            int e = MulDiv(6, dpi_, 96);
            if (p.x < wr.left + e || p.x >= wr.right - e || p.y < wr.top + e || p.y >= wr.bottom - e) return HTTRANSPARENT;
        }
        break;
    case WM_SETCURSOR:
        SetCursor(cursorHidden_ ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE:
        NoteMouseActivity();
        if (clickPending_ && GetCapture() == h && !fullscreen_) {
            POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
            if (std::abs(p.x - downPt_.x) > GetSystemMetrics(SM_CXDRAG) ||
                std::abs(p.y - downPt_.y) > GetSystemMetrics(SM_CYDRAG)) {
                // Dragging the video moves the window (as in MPC-HC).
                clickPending_ = false;
                ReleaseCapture();
                SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
        NoteMouseActivity();
        util::FocusIfActive(hwnd_);
        downPt_ = {(short)LOWORD(l), (short)HIWORD(l)};
        clickPending_ = true;
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) ReleaseCapture();
        if (clickPending_) {
            clickPending_ = false;
            if (bar_.hasFile) DoAction(Act::PlayPause);
            else DoAction(Act::Open);
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        Act a = keys_.Lookup(CurrentMods() | kMouseLeftDbl);
        if (a != Act::None) {
            if (bar_.hasFile) DoAction(Act::PlayPause);  // undo the first click's play/pause
            DoAction(a);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        ClientToScreen(h, &p);
        ShowContextMenu(p);
        return 0;
    }
    case WM_MBUTTONUP:
        HandleChord(CurrentMods() | kMouseMiddle);
        return 0;
    case WM_XBUTTONUP:
        HandleChord(CurrentMods() | (GET_XBUTTON_WPARAM(w) == XBUTTON1 ? kMouseX1 : kMouseX2));
        return TRUE;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return OnMain(m, w, l);
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT App::OnMain(UINT m, WPARAM w, LPARAM l) {
    if (m == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
        if (trayIcon_) { trayIcon_ = false; AddTray(); }
        return 0;
    }
    if (m == taskbarButtonMsg_ && taskbarButtonMsg_) {
        OnTaskbarButtonCreated();
        return 0;
    }
    switch (m) {
    case edit::WM_APP_EDIT_LOADED:
    case edit::WM_APP_EDIT_KEYFRAMES:
    case edit::WM_APP_EDIT_JOBDONE:
    case edit::WM_APP_EDIT_UI:
        if (editor_) editor_->OnAppMessage(m, w, l);
        return 0;
    case WM_APP_CLIP_DONE:
        OnClipDone(w != 0, (std::wstring*)l);
        return 0;
    case WM_APP_RECYCLED: {
        std::unique_ptr<std::wstring> name((std::wstring*)l);
        mpvx::ShowText(mpv_, (w ? L"Moved to the Recycle Bin: " : L"Could not move to the Recycle Bin: ") + *name, 3000);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(w) == 0x1800 /* THBN_CLICKED */) ExtraMenuCommand(LOWORD(w));
        return 0;
    case WM_APP_MPV:
        DrainEvents();
        return 0;
    case WM_APP_MPV_READY:
        mpv_ = (mpv_handle*)l;
        OnMpvReady(w != 0);
        return 0;
    case WM_APP_INDEX:
        OnIndex((void*)l, (unsigned)w);
        return 0;
    case WM_APP_THUMB:
        if (hoverT_ >= 0 && thumbs_) thumbWnd_->Show(thumbs_->Get(hoverT_), hoverT_, hoverAnchor_, dpi_);
        return 0;
    case WM_APP_DISABLE_CHILD:
        // libmpv's video window sits on top of ours. Disabled, it passes mouse input to us (its parent).
        if (HWND c = l ? (HWND)l : FindWindowExW(video_, nullptr, nullptr, nullptr)) {
            mpvChild_ = c;
            EnableWindow(c, FALSE);
        }
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_LBUTTONDBLCLK) ShowFromTray();
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kCmdTrayShow, L"Show FastPlay");
            AppendMenuW(menu, MF_STRING, kCmdTrayOpen, L"Open file...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCmdTrayExit, L"Exit");
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(hwnd_);  // required for the tray menu to close when clicking elsewhere
            UINT id = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, hwnd_, nullptr);
            DestroyMenu(menu);
            if (id) OnMenuCommand(id);
        }
        return 0;
    case WM_TIMER:
        if (editor_ && editor_->OnAppMessage(m, w, l)) return 0;
        switch (w) {
        case kTimerUi:
            if (mpv_ && bar_.hasFile && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
                double p = mpvx::GetDouble(mpv_, "time-pos", bar_.pos);
                if (p != bar_.pos) {
                    double was = bar_.pos;
                    bar_.pos = p;
                    if (barWnd_ && barWnd_->PosChangesPixels(was, p)) RefreshBar();
                }
            }
            UpdateTaskbar(false);
            return 0;
        case kTimerIdle:
            UpdateCursor();
            if (!mpvChild_ && mpv_) PostMessageW(hwnd_, WM_APP_DISABLE_CHILD, 0, 0);
            return 0;
        case kTimerThumbs:
            KillTimer(hwnd_, kTimerThumbs);
            StartThumbnails();
            InitTaskbar();  // COM + taskbar buttons once playback has settled, never on the start-up path
            return 0;
        case kTimerStats:
            if (statsOn_) {  // re-show with fresh values
                ToggleStats();
                ToggleStats();
            }
            return 0;
        case kTimerBenchExit:
            KillTimer(hwnd_, kTimerBenchExit);
            Quit();
            return 0;
        }
        return 0;
    case WM_SIZE:
        Layout();
        if (!fullscreen_ && !IsZoomed(hwnd_) && !IsIconic(hwnd_)) {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            s_.winW = rc.right;
            s_.winH = rc.bottom;
        }
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(w);
        RECT* r = (RECT*)l;
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        barWnd_->OnDpi(dpi_);
        if (editor_) editor_->OnDpi(dpi_);
        Layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize = {MulDiv(320, dpi_, 96), MulDiv(200, dpi_, 96)};
        return 0;
    }
    case WM_NCHITTEST:
        if (!captionOn_ && !fullscreen_ && !IsZoomed(hwnd_)) {
            POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
            RECT wr;
            GetWindowRect(hwnd_, &wr);
            int e = MulDiv(6, dpi_, 96);
            bool L = p.x < wr.left + e, R = p.x >= wr.right - e, T = p.y < wr.top + e, B = p.y >= wr.bottom - e;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        uint32_t vk = (uint32_t)w;
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) break;
        bool repeat = (l & (1 << 30)) != 0;
        if (EditActive() && editor_->HandleChord(CurrentMods() | vk, repeat)) return 0;  // FastCut's keys first
        Act a = keys_.Lookup(CurrentMods() | vk);
        if (a != Act::None) {
            if (!repeat || IsRepeatable(a)) DoAction(a);
            return 0;
        }
        break;
    }
    case WM_SYSCHAR:
        if (w == L' ') break;  // Alt+Space: system menu
        return 0;                // mapped Alt+key combos would otherwise beep
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU && l == 0) return 0;  // a lone Alt press shouldn't enter menu mode
        if ((w & 0xFFF0) == SC_SCREENSAVE || (w & 0xFFF0) == SC_MONITORPOWER) {
            if (bar_.hasFile && !bar_.paused) return 0;
        }
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w);
        uint32_t code = m == WM_MOUSEWHEEL ? (d > 0 ? kWheelUp : kWheelDown) : (d > 0 ? kWheelRight : kWheelLeft);
        HandleChord(CurrentMods() | code);
        return 0;
    }
    case WM_APPCOMMAND: {
        Act a = Act::None;
        switch (GET_APPCOMMAND_LPARAM(l)) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: a = Act::PlayPause; break;
        case APPCOMMAND_MEDIA_PLAY: a = Act::Play; break;
        case APPCOMMAND_MEDIA_PAUSE: a = Act::Pause; break;
        case APPCOMMAND_MEDIA_STOP: a = Act::Stop; break;
        case APPCOMMAND_MEDIA_NEXTTRACK: a = Act::NextFile; break;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: a = Act::PrevFile; break;
        }
        if (a == Act::None) break;
        DoAction(a);
        return TRUE;
    }
    case WM_CONTEXTMENU: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (p.x == -1 && p.y == -1) GetCursorPos(&p);
        ShowContextMenu(p);
        return 0;
    }
    case WM_DROPFILES: {
        // Subtitle files load into the current video; media files play (several = playlist; Shift = append).
        HDROP drop = (HDROP)w;
        std::vector<std::wstring> media;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t path[MAX_PATH * 4];
            if (!DragQueryFileW(drop, i, path, (UINT)std::size(path))) continue;
            std::wstring p = path, ext = util::Lower(p.substr(p.find_last_of(L'.') + 1));
            if (ext == L"srt" || ext == L"ass" || ext == L"ssa" || ext == L"vtt" || ext == L"sub" || ext == L"sup") {
                std::string p8 = util::Utf8(p);
                mpvx::CmdAsync(mpv_, {"sub-add", p8.c_str(), "select"});
            } else {
                media.push_back(p);
            }
        }
        DragFinish(drop);
        bool append = GetKeyState(VK_SHIFT) < 0 && !path_.empty();
        if (append && plist_.empty()) {
            plist_ = {path_};
            plPos_ = 0;
        }
        if (!media.empty()) OpenFiles(media, append);
        return 0;
    }
    case WM_COPYDATA: {
        auto* cd = (COPYDATASTRUCT*)l;
        if (cd->dwData == kCopyDataCommand) {
            std::wstring c((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
            while (!c.empty() && c.back() == 0) c.pop_back();
            RunCommand(c);
            return TRUE;
        }
        if (cd->dwData != kCopyDataOpen) break;
        std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        while (!p.empty() && p.back() == 0) p.pop_back();
        if (inTray_) ShowFromTray();
        else if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        if (!util::TestMode()) SetForegroundWindow(hwnd_);
        std::vector<std::wstring> files;
        for (size_t pos = 0; pos < p.size();) {
            size_t nl = p.find(L'\n', pos);
            if (nl == std::wstring::npos) nl = p.size();
            if (nl > pos) files.push_back(p.substr(pos, nl - pos));
            pos = nl + 1;
        }
        if (!files.empty()) OpenFiles(files, false);
        return TRUE;
    }
    case WM_ACTIVATEAPP:
        if (w) ReloadSettingsIfChanged();
        break;
    case WM_CLOSE:
        if (editor_ && !editor_->ConfirmQuit()) return 0;
        if (s_.tray && !quitting_ && (!util::TestMode() || !util::InstanceSuffix().empty())) HideToTray();
        else Quit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ------------------------------------------------------------------------------------------------ actions

void App::SeekTo(double t, bool exact) {
    if (!mpv_ || !bar_.hasFile) return;
    char ts[32];
    snprintf(ts, sizeof ts, "%.3f", t);
    mpvx::CmdAsync(mpv_, {"seek", ts, exact ? "absolute+exact" : "absolute+keyframes"});
    bar_.pos = t;
    RefreshBar();
}

void App::SeekRelative(double secs, const char* flags) {
    char ts[32];
    snprintf(ts, sizeof ts, "%.3f", secs);
    mpvx::CmdAsync(mpv_, {"osd-msg", "seek", ts, flags});
}

void App::SetVolume(int v) {
    v = std::clamp(v, 0, s_.volumeMax);
    s_.volume = v;
    mpvx::SetDouble(mpv_, "volume", v);
    bar_.volume = v;
    mpvx::ShowText(mpv_, L"Volume: " + std::to_wstring(v) + L"%");
    RefreshBar();
}

void App::ChangeSpeed(int dir) {
    double sp = mpvx::GetDouble(mpv_, "speed", 1);
    if (dir == 0) sp = 1;
    else if (s_.speedStep <= 0) sp = dir > 0 ? sp * 2 : sp / 2;  // MPC-HC style doubling
    else sp = std::round((sp + dir * s_.speedStep) / s_.speedStep) * s_.speedStep;
    sp = std::clamp(sp, 0.0625, 16.0);
    mpvx::SetDouble(mpv_, "speed", sp);
    wchar_t b[48];
    swprintf(b, 48, L"Speed: %.2fx", sp);
    mpvx::ShowText(mpv_, b);
}

void App::PanZoom(double dx, double dy, double dz) {
    panX_ += dx;
    panY_ += dy;
    zoom_ += dz;
    if (dx == 0 && dy == 0 && dz == 0) {
        panX_ = panY_ = zoom_ = 0;
        scaleX_ = scaleY_ = 1;
        mpvx::SetDouble(mpv_, "video-scale-x", 1);
        mpvx::SetDouble(mpv_, "video-scale-y", 1);
        if (rotate_) {
            rotate_ = 0;
            mpvx::SetStr(mpv_, "video-rotate", "0");
        }
    }
    mpvx::SetDouble(mpv_, "video-pan-x", panX_);
    mpvx::SetDouble(mpv_, "video-pan-y", panY_);
    mpvx::SetDouble(mpv_, "video-zoom", zoom_);
    wchar_t b[64];
    swprintf(b, 64, L"Zoom %.0f%%  Pan %.2f, %.2f", std::pow(2.0, zoom_) * 100, panX_, panY_);
    mpvx::ShowText(mpv_, b);
}

void App::CycleTrack(const char* type, int dir) {
    mpvx::CmdAsync(mpv_, {"osd-msg", "cycle", type, dir > 0 ? "up" : "down"});
}

void App::ToggleStats() {
    statsOn_ = !statsOn_;
    if (!statsOn_) {
        KillTimer(hwnd_, kTimerStats);
        mpvx::SetDouble(mpv_, "osd-font-size", 30);
        mpvx::CmdAsync(mpv_, {"show-text", "", "1"});
        return;
    }
    SetTimer(hwnd_, kTimerStats, 1000, nullptr);
    mpvx::SetDouble(mpv_, "osd-font-size", 18);  // smaller text while the statistics are up
    // Commands from the client API don't expand ${properties} unless asked to.
    mpvx::CmdAsync(mpv_, {"expand-properties", "show-text",
                          "${?video-format:Video: ${video-format} ${width}x${height} ${container-fps} fps\n}"
                          "${?hwdec-current:Decoder: ${hwdec-current}  Output: ${current-vo}\n}"
                          "${?estimated-vf-fps:Displayed: ${estimated-vf-fps} fps  Dropped: ${frame-drop-count}/${decoder-frame-drop-count}\n}"
                          "${?audio-codec-name:Audio: ${audio-codec-name} ${audio-params/samplerate} Hz ${audio-params/hr-channels}\n}"
                          "${?video-bitrate:Bitrate: ${video-bitrate} video, ${audio-bitrate} audio\n}"
                          "Cache: ${demuxer-cache-duration} s  Speed: ${speed}x  A/V: ${avsync}",
                          "1100"});
}

void App::ShowProperties() {
    if (path_.empty()) return;
    auto g = [&](const char* p) { return util::Wide(mpvx::GetStr(mpv_, p)); };
    std::wstring s = L"File: " + path_ + L"\n";
    wchar_t sz[64];
    swprintf(sz, 64, L"Size: %.2f GB\n", fileSize_ / 1e9);
    s += sz;
    s += L"Container: " + g("file-format") + L"\nDuration: " + util::FormatTime(bar_.dur, true) + L"\n\n";
    if (vidW_) {
        s += L"Video: " + g("video-format") + L" " + std::to_wstring(mpvx::GetInt(mpv_, "width")) + L"x" +
             std::to_wstring(mpvx::GetInt(mpv_, "height")) + L", " + g("container-fps") + L" fps\n";
        s += L"Decoder: " + g("hwdec-current") + L"\n";
    }
    s += L"Audio: " + g("audio-codec-name") + L", " + g("audio-params/samplerate") + L" Hz, " +
         g("audio-params/hr-channels") + L"\n";
    if (indexReady_)
        s += L"\nKeyframe index: " + std::to_wstring(index_.keyTimes.size()) + L" entries" +
             (index_.fragmented ? L" (fragmented MP4, mfra)" : L"") + L"\n";
    MessageBoxW(hwnd_, s.c_str(), L"Properties", MB_OK);
}

void App::ScreenshotSaveAs() {
    if (path_.empty()) return;
    std::wstring base = util::FileName(path_);
    base = base.substr(0, base.find_last_of(L'.'));
    std::wstring t = util::FormatTime(bar_.pos, true);
    for (auto& c : t) if (c == L':') c = L'.';
    std::wstring name = base + L"_" + t + L"." + s_.screenshotFormat;
    std::wstring file = PickSaveFile(hwnd_, s_.screenshotDir, name,
                                     L"PNG\0*.png\0JPEG\0*.jpg\0WebP\0*.webp\0All files\0*.*\0", s_.screenshotFormat.c_str());
    if (file.empty()) return;
    std::string f8 = util::Utf8(file);
    mpvx::CmdAsync(mpv_, {"osd-msg", "screenshot-to-file", f8.c_str(), "subtitles"});
}

void App::GoToDialog() {
    if (path_.empty()) return;
    std::wstring v = util::FormatTime(bar_.pos, true);
    if (!InputBox(hwnd_, L"Go To...", L"Time (h:mm:ss.ms, mm:ss or seconds):", &v)) return;
    double t;
    if (util::ParseTime(v, &t)) SeekTo(t, true);
}

void App::ToggleRemaining() {
    bar_.remaining = !bar_.remaining;
    RefreshBar();
}

void App::HoverSeek(double t, POINT anchor) {
    hoverT_ = t;
    if (t < 0) {
        thumbWnd_->Hide();
        return;
    }
    hoverAnchor_ = anchor;
    std::shared_ptr<const Thumb> th;
    if (thumbs_) {
        thumbs_->Request(t);
        th = thumbs_->Get(t);
    } else if (s_.thumbnails && bar_.hasFile && !thumbs_ && firstFrameLogged_) {
        StartThumbnails();
        if (thumbs_) thumbs_->Request(t);
    }
    thumbWnd_->Show(th, t, anchor, dpi_);
}

void App::RunCommand(const std::wstring& cmd) {
    size_t sp = cmd.find(L' ');
    std::wstring verb = cmd.substr(0, sp), arg = sp == std::wstring::npos ? L"" : util::Trim(cmd.substr(sp + 1));
    if (verb == L"action") {
        util::TimingLog(("command: action " + util::Utf8(arg)).c_str());
        int n;
        const ActionInfo* acts = Actions(&n);
        for (int i = 0; i < n; i++)
            if (arg == acts[i].id) DoAction(acts[i].act);
    } else if (verb == L"seek") {
        util::TimingLog(("command: seek " + util::Utf8(arg)).c_str());
        SeekTo(_wtof(arg.c_str()), true);
    } else if (verb == L"seekkey") {
        util::TimingLog(("command: seek keyframes " + util::Utf8(arg)).c_str());
        SeekTo(_wtof(arg.c_str()), false);
    } else if (verb == L"hover") {
        // Same path as hovering the seekbar at that time; the preview sits above the bar's centre.
        RECT br;
        GetWindowRect(barWnd_->Hwnd(), &br);
        HoverSeek(_wtof(arg.c_str()), {(br.left + br.right) / 2, br.top});
    } else if (verb == L"open") {
        Open(arg);
    } else if (verb == L"clip") {
        if (!FindFfmpeg().empty() && bar_.abA >= 0 && bar_.abB > bar_.abA) StartClip(arg, FindFfmpeg());
    } else if (verb == L"edit") {
        if (!editor_) editor_ = new edit::Editor(this);
        util::TimingLog(("command: edit " + util::Utf8(arg)).c_str());
        editor_->RunAction(arg);
    } else if (verb == L"editset" && editor_) {
        size_t sp2 = arg.find(L' ');
        editor_->SetOption(arg.substr(0, sp2), sp2 == std::wstring::npos ? L"" : arg.substr(sp2 + 1));
    } else if (verb == L"editstate") {
        util::TimingLog(editor_ ? util::Utf8(editor_->DebugState()).c_str() : "edit: none");
    } else if (verb == L"menu") {
        OnMenuCommand((UINT)_wtoi(arg.c_str()));
    } else if (verb == L"get") {
        util::TimingLog(("property " + util::Utf8(arg) + " = " + mpvx::GetStr(mpv_, util::Utf8(arg).c_str())).c_str());
    } else if (verb == L"state") {
        char b[512];
        snprintf(b, sizeof b, "state: playlist %d/%d, after %d, volume %d/%d, duration %.3f, path %s", plPos_,
                 (int)plist_.size(), s_.afterPlayback, bar_.volume, bar_.volumeMax, bar_.dur,
                 util::Utf8(util::FileName(path_)).c_str());
        util::TimingLog(b);
    }
}

void App::DoAction(Act a) {
    if (!mpv_ && a != Act::Exit && a != Act::Open && a != Act::Menu && a != Act::EditSettings && a != Act::Playlist &&
        a != Act::OpenUrl && a != Act::Help)
        return;
    const char* relFlags = s_.exactSeek ? "relative+exact" : "relative";
    switch (a) {
    case Act::PlayPause:
        if (!bar_.hasFile) break;
        if (mpvx::GetFlag(mpv_, "eof-reached")) {
            mpvx::CmdAsync(mpv_, {"seek", "0", "absolute"});
            mpvx::SetFlag(mpv_, "pause", false);
        } else {
            mpvx::CmdAsync(mpv_, {"cycle", "pause"});
        }
        break;
    case Act::Play: mpvx::SetFlag(mpv_, "pause", false); break;
    case Act::Pause: mpvx::SetFlag(mpv_, "pause", true); break;
    case Act::Stop:
        mpvx::SetFlag(mpv_, "pause", true);
        mpvx::CmdAsync(mpv_, {"seek", "0", "absolute+exact"});
        mpvx::ShowText(mpv_, L"Stopped");
        break;
    case Act::FrameStep: mpvx::CmdAsync(mpv_, {"frame-step"}); break;
    case Act::FrameBack: mpvx::CmdAsync(mpv_, {"frame-back-step"}); break;
    case Act::SeekFwdSmall: SeekRelative(s_.seekSmall, relFlags); break;
    case Act::SeekBackSmall: SeekRelative(-s_.seekSmall, relFlags); break;
    case Act::SeekFwdMed: SeekRelative(s_.seekMedium, relFlags); break;
    case Act::SeekBackMed: SeekRelative(-s_.seekMedium, relFlags); break;
    case Act::SeekFwdLarge: SeekRelative(s_.seekLarge, relFlags); break;
    case Act::SeekBackLarge: SeekRelative(-s_.seekLarge, relFlags); break;
    case Act::KeyframeFwd:
    case Act::KeyframeBack: {
        double pos = mpvx::GetDouble(mpv_, "time-pos", 0);
        bool fwd = a == Act::KeyframeFwd;
        if (indexReady_ && !index_.keyTimes.empty()) {
            const auto& k = index_.keyTimes;
            double target = -1;
            if (fwd) {
                auto it = std::upper_bound(k.begin(), k.end(), pos + 0.01);
                if (it != k.end()) target = *it;
            } else {
                auto it = std::lower_bound(k.begin(), k.end(), pos - 0.01);
                if (it != k.begin()) target = *(it - 1);
            }
            if (target >= 0) {
                char ts[32];
                snprintf(ts, sizeof ts, "%.3f", target);
                mpvx::CmdAsync(mpv_, {"osd-msg", "seek", ts, "absolute+exact"});
            }
        } else {
            SeekRelative(fwd ? 0.01 : -0.01, "relative+keyframes");
        }
        break;
    }
    case Act::SeekStart: mpvx::CmdAsync(mpv_, {"osd-msg", "seek", "0", "absolute"}); break;
    case Act::GoTo: GoToDialog(); break;
    case Act::SpeedUp: ChangeSpeed(1); break;
    case Act::SpeedDown: ChangeSpeed(-1); break;
    case Act::SpeedReset: ChangeSpeed(0); break;
    case Act::AudioDelayUp: mpvx::CmdAsync(mpv_, {"osd-msg", "add", "audio-delay", "0.010"}); break;
    case Act::AudioDelayDown: mpvx::CmdAsync(mpv_, {"osd-msg", "add", "audio-delay", "-0.010"}); break;
    case Act::SubDelayUp: mpvx::CmdAsync(mpv_, {"osd-msg", "add", "sub-delay", "0.1"}); break;
    case Act::SubDelayDown: mpvx::CmdAsync(mpv_, {"osd-msg", "add", "sub-delay", "-0.1"}); break;
    case Act::NextChapter:
    case Act::PrevChapter:
        // MPC-HC: chapters first, then the playlist, then the folder.
        if (!bar_.chapters.empty()) mpvx::CmdAsync(mpv_, {"osd-msg", "add", "chapter", a == Act::NextChapter ? "1" : "-1"});
        else if (!StepPlaylist(a == Act::NextChapter ? 1 : -1)) StepFile(a == Act::NextChapter ? 1 : -1);
        break;
    case Act::NextFile: if (!StepPlaylist(1)) StepFile(1); break;
    case Act::PrevFile: if (!StepPlaylist(-1)) StepFile(-1); break;
    case Act::Open: {
        auto files = PickOpenFiles(hwnd_, path_.empty() ? L"" : util::DirName(path_));
        if (!files.empty()) OpenFiles(files, false);
        break;
    }
    case Act::Close: CloseFile(); break;
    case Act::Exit: Quit(); break;
    case Act::Fullscreen: SetFullscreen(!fullscreen_); break;
    case Act::ExitFullscreen: SetFullscreen(false); break;
    case Act::OnTop:
        SetOnTop(!onTop_);
        mpvx::ShowText(mpv_, onTop_ ? L"Always on top" : L"Not on top");
        break;
    case Act::ToggleCaption: SetCaption(!captionOn_); break;
    case Act::ToggleControls: SetControls(!controlsOn_); break;
    case Act::ViewMinimal: SetCaption(false); SetControls(false); break;
    case Act::ViewNormal: SetCaption(true); SetControls(true); break;
    case Act::ViewCompact: SetCaption(false); SetControls(true); break;
    case Act::Zoom50: ResizeToVideo(0.5, false); break;
    case Act::Zoom100: ResizeToVideo(1, false); break;
    case Act::Zoom200: ResizeToVideo(2, false); break;
    case Act::ZoomAutofit: ResizeToVideo(0, true); break;
    case Act::PanLeft: PanZoom(-0.02, 0, 0); break;
    case Act::PanRight: PanZoom(0.02, 0, 0); break;
    case Act::PanUp: PanZoom(0, -0.02, 0); break;
    case Act::PanDown: PanZoom(0, 0.02, 0); break;
    case Act::PanReset: PanZoom(0, 0, 0); break;
    case Act::ZoomIn: PanZoom(0, 0, 0.1); break;
    case Act::ZoomOut: PanZoom(0, 0, -0.1); break;
    case Act::VolUp: SetVolume(bar_.volume + s_.volumeStep); break;
    case Act::VolDown: SetVolume(bar_.volume - s_.volumeStep); break;
    case Act::Mute:
        mpvx::CmdAsync(mpv_, {"cycle", "mute"});
        mpvx::ShowText(mpv_, bar_.mute ? L"Sound on" : L"Mute");
        break;
    case Act::NextAudio: CycleTrack("aid", 1); break;
    case Act::PrevAudio: CycleTrack("aid", -1); break;
    case Act::NextSub: CycleTrack("sid", 1); break;
    case Act::PrevSub: CycleTrack("sid", -1); break;
    case Act::SubToggle: mpvx::CmdAsync(mpv_, {"osd-msg", "cycle", "sub-visibility"}); break;
    case Act::ScreenshotSave: ScreenshotSaveAs(); break;
    case Act::ScreenshotAuto: mpvx::CmdAsync(mpv_, {"osd-msg", "screenshot", "subtitles"}); break;
    case Act::AbStart:
        mpvx::SetDouble(mpv_, "ab-loop-a", mpvx::GetDouble(mpv_, "time-pos", 0));
        mpvx::ShowText(mpv_, L"A-B loop: A set");
        break;
    case Act::AbEnd:
        mpvx::SetDouble(mpv_, "ab-loop-b", mpvx::GetDouble(mpv_, "time-pos", 0));
        mpvx::ShowText(mpv_, L"A-B loop: B set");
        break;
    case Act::AbClear:
        mpvx::SetStr(mpv_, "ab-loop-a", "no");
        mpvx::SetStr(mpv_, "ab-loop-b", "no");
        mpvx::ShowText(mpv_, L"A-B loop cleared");
        break;
    case Act::Stats: ToggleStats(); break;
    case Act::Properties: ShowProperties(); break;
    case Act::EditSettings: {
        s_.Save(ini_);
        keys_.WriteDefaults(ini_);
        ini_.Save(iniPath_);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(iniPath_.c_str(), GetFileExInfoStandard, &fa)) iniTime_ = fa.ftLastWriteTime;
        ShellExecuteW(hwnd_, L"open", L"notepad.exe", (L"\"" + iniPath_ + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        mpvx::ShowText(mpv_, L"Settings open in Notepad; they apply when you come back to FastPlay", 3000);
        break;
    }
    case Act::Menu: {
        POINT p;
        GetCursorPos(&p);
        ShowContextMenu(p);
        break;
    }
    case Act::EditMode: ToggleEditMode(); break;
    default: ExtraAction(a); break;
    }
}

// ------------------------------------------------------------------------------------------------ menus

static void AddItem(HMENU m, UINT id, const std::wstring& text, bool checked = false, bool enabled = true) {
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text.c_str());
}

void App::BuildMenu(HMENU m) {
    auto item = [&](HMENU menu, Act a, bool checked = false, bool enabled = true) {
        const ActionInfo* info = FindAction(a);
        std::wstring t = info->label;
        std::wstring k = keys_.ShortcutText(a);
        if (!k.empty()) t += L"\t" + k;
        AddItem(menu, kCmdAction + (UINT)a, t, checked, enabled);
    };
    bool f = bar_.hasFile && mpv_;
    item(m, Act::Open);
    HMENU recent = CreatePopupMenu();
    auto rec = resume_.Recent(15);
    for (size_t i = 0; i < rec.size(); i++) AddItem(recent, kCmdRecent + (UINT)i, std::to_wstring(i + 1) + L"  " + rec[i]);
    if (rec.empty()) AddItem(recent, 0, L"(empty)", false, false);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)recent, L"Recent files");
    item(m, Act::Close, false, f);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    item(m, Act::PlayPause, false, f);
    item(m, Act::Stop, false, f);
    item(m, Act::FrameStep, false, f);
    item(m, Act::FrameBack, false, f);

    HMENU play = CreatePopupMenu();
    item(play, Act::SpeedUp, false, f);
    item(play, Act::SpeedDown, false, f);
    item(play, Act::SpeedReset, false, f);
    AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    static const double speeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
    for (int i = 0; i < (int)std::size(speeds); i++) {
        wchar_t b[32];
        swprintf(b, 32, L"%gx", speeds[i]);
        AddItem(play, kCmdSpeed + i, b, std::abs(bar_.speed - speeds[i]) < 1e-6, f);
    }
    AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    item(play, Act::AbStart, bar_.abA >= 0, f);
    item(play, Act::AbEnd, bar_.abB >= 0, f);
    item(play, Act::AbClear, false, f && bar_.abA >= 0);
    AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    item(play, Act::GoTo, false, f);
    item(play, Act::KeyframeBack, false, f);
    item(play, Act::KeyframeFwd, false, f);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)play, L"Play");

    HMENU nav = CreatePopupMenu();
    item(nav, Act::PrevFile, false, f);
    item(nav, Act::NextFile, false, f);
    if (!bar_.chapters.empty()) {
        AppendMenuW(nav, MF_SEPARATOR, 0, nullptr);
        int cur = (int)mpvx::GetInt(mpv_, "chapter", -1);
        for (size_t i = 0; i < bar_.chapters.size() && i < 200; i++) {
            std::wstring t = L"Chapter " + std::to_wstring(i + 1) + L"\t" + util::FormatTime(bar_.chapters[i]);
            AddItem(nav, kCmdChapter + (UINT)i, t, (int)i == cur);
        }
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)nav, L"Navigate");

    auto tracks = GetTracks(f ? mpv_ : nullptr);
    HMENU audio = CreatePopupMenu();
    for (auto& t : tracks)
        if (t.type == "audio") AddItem(audio, kCmdAudio + (UINT)t.id, TrackLabel(t), t.selected);
    AppendMenuW(audio, MF_SEPARATOR, 0, nullptr);
    item(audio, Act::Mute, bar_.mute);
    item(audio, Act::AudioDelayUp, false, f);
    item(audio, Act::AudioDelayDown, false, f);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)audio, L"Audio");

    HMENU subs = CreatePopupMenu();
    bool anySub = false;
    for (auto& t : tracks)
        if (t.type == "sub") {
            AddItem(subs, kCmdSub + (UINT)t.id, TrackLabel(t), t.selected);
            anySub = anySub || t.selected;
        }
    AddItem(subs, kCmdSub, L"Off", !anySub);
    AppendMenuW(subs, MF_SEPARATOR, 0, nullptr);
    AddItem(subs, kCmdLoadSub, L"Load subtitle...", false, f);
    item(subs, Act::SubToggle, false, f);
    item(subs, Act::SubDelayUp, false, f);
    item(subs, Act::SubDelayDown, false, f);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)subs, L"Subtitles");

    HMENU view = CreatePopupMenu();
    item(view, Act::Fullscreen, fullscreen_);
    item(view, Act::OnTop, onTop_);
    item(view, Act::ToggleCaption, !captionOn_);
    item(view, Act::ToggleControls, controlsOn_);
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    item(view, Act::Zoom50, false, vidW_ > 0);
    item(view, Act::Zoom100, false, vidW_ > 0);
    item(view, Act::Zoom200, false, vidW_ > 0);
    item(view, Act::ZoomAutofit, false, vidW_ > 0);
    item(view, Act::ZoomIn, false, f);
    item(view, Act::ZoomOut, false, f);
    item(view, Act::PanReset, false, f);
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    item(view, Act::Stats, statsOn_, f);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)view, L"View");
    BuildExtraMenus(m, play, view, audio, subs);  // MPC-HC extras: after playback, aspect, devices, tools, help
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    item(m, Act::ScreenshotSave, false, f);
    item(m, Act::ScreenshotAuto, false, f);
    AddItem(m, kCmdOpenFolder, L"Open containing folder", false, !path_.empty());
    item(m, Act::Properties, false, f);
    item(m, Act::EditSettings);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(m, Act::Exit);
}

void App::ShowContextMenu(POINT screen) {
    HMENU m = CreatePopupMenu();
    if (EditActive()) {
        editor_->AppendMenuItems(m);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    BuildMenu(m);
    thumbWnd_->Hide();
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd_, nullptr);
    DestroyMenu(m);  // destroys submenus too
    if (id) OnMenuCommand(id);
}

void App::OnMenuCommand(UINT id) {
    if (editor_ && editor_->RunMenuId(id)) return;
    if (ExtraMenuCommand(id)) return;
    if (id >= kCmdAction && id < kCmdAction + (UINT)Act::Count) {
        DoAction((Act)(id - kCmdAction));
    } else if (id >= kCmdAudio && id < kCmdSub) {
        std::string v = std::to_string(id - kCmdAudio);
        mpvx::SetStr(mpv_, "aid", v.c_str());
    } else if (id >= kCmdSub && id < kCmdRecent) {
        std::string v = id == kCmdSub ? "no" : std::to_string(id - kCmdSub);
        mpvx::SetStr(mpv_, "sid", v.c_str());
    } else if (id >= kCmdRecent && id < kCmdChapter) {
        auto rec = resume_.Recent(15);
        size_t i = id - kCmdRecent;
        if (i < rec.size()) Open(rec[i]);
    } else if (id >= kCmdChapter && id < kCmdTrayOpen) {
        std::string v = std::to_string(id - kCmdChapter);
        mpvx::SetStr(mpv_, "chapter", v.c_str());
    } else if (id >= kCmdSpeed && id < kCmdSpeed + 20) {
        static const double speeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
        if (id - kCmdSpeed < std::size(speeds)) mpvx::SetDouble(mpv_, "speed", speeds[id - kCmdSpeed]);
    } else if (id == kCmdTrayOpen) {
        ShowFromTray();
        DoAction(Act::Open);
    } else if (id == kCmdTrayShow) {
        ShowFromTray();
    } else if (id == kCmdTrayExit) {
        Quit();
    } else if (id == kCmdLoadSub) {
        std::wstring f = PickOpenFile(hwnd_, util::DirName(path_));
        if (!f.empty()) {
            std::string f8 = util::Utf8(f);
            mpvx::CmdAsync(mpv_, {"sub-add", f8.c_str(), "select"});
        }
    } else if (id == kCmdOpenFolder) {
        std::wstring args = L"/select,\"" + path_ + L"\"";
        ShellExecuteW(hwnd_, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

// ------------------------------------------------------------------------------------------------ tray, exit

void App::AddTray() {
    if (trayIcon_ || util::TestMode()) return;  // test instances stay out of the notification area
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = hwnd_;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_APP_TRAY;
    n.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(n.szTip, L"FastPlay");
    trayIcon_ = Shell_NotifyIconW(NIM_ADD, &n) != 0;
    if (n.hIcon) DestroyIcon(n.hIcon);
}

void App::RemoveTray() {
    if (!trayIcon_) return;
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = hwnd_;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
    trayIcon_ = false;
}

void App::HideToTray() {
    SetFullscreen(false);
    SaveResume();
    RetireThumbnails();
    if (mpv_) mpvx::Cmd(mpv_, {"stop"});  // releases the file; libmpv and the GPU output stay warm
    path_.clear();
    bar_ = BarState{false, true, bar_.mute, bar_.remaining, 0, 0, 1, -1, -1, bar_.volume, bar_.volumeMax, {}};
    UpdateTitle();
    ShowWindow(hwnd_, SW_HIDE);
    inTray_ = true;
    AddTray();
    for (auto& t : retiring_) t.join();
    retiring_.clear();
}

void App::ShowFromTray() {
    if (!inTray_) return;
    inTray_ = false;
    RemoveTray();
    ShowWindow(hwnd_, util::TestMode() ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (!util::TestMode()) SetForegroundWindow(hwnd_);
}

void App::ReloadSettingsIfChanged() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(iniPath_.c_str(), GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &iniTime_) == 0) return;
    iniTime_ = fa.ftLastWriteTime;
    ini_.Load(iniPath_);
    s_.Load(ini_);
    keys_.Load(ini_);
    ApplyLiveSettings();
    mpvx::ShowText(mpv_, L"Settings reloaded");
}

void App::ApplyLiveSettings() {
    if (!mpv_) return;
    std::string fmt = util::Utf8(s_.screenshotFormat);
    mpvx::SetStr(mpv_, "screenshot-format", fmt.c_str());
    if (!s_.screenshotDir.empty()) {
        std::string d = util::Utf8(s_.screenshotDir);
        mpvx::SetStr(mpv_, "screenshot-directory", d.c_str());
    }
    std::string hw = util::Utf8(s_.hwdec);
    mpvx::SetStr(mpv_, "hwdec", hw.c_str());
}

bool App::EditActive() const { return editor_ && editor_->Active(); }

void App::ToggleEditMode() {
    if (!editor_) editor_ = new edit::Editor(this);  // first use: nothing of Edit mode exists before this
    if (editor_->Active()) editor_->Leave();
    else {
        if (fullscreen_) SetFullscreen(false);
        editor_->Enter();
    }
    RefreshBar();
}

bool App::PreTranslateKey(MSG& msg) {
    // Keys typed while an edit-mode panel (timeline, lists, buttons) has focus run the actions, like FastCut's
    // KeyPreview. Text boxes and drop-downs keep their keys.
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    if (!EditActive() || msg.hwnd == hwnd_ || !IsChild(hwnd_, msg.hwnd)) return false;
    wchar_t cls[32];
    GetClassNameW(msg.hwnd, cls, 32);
    if (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"ComboBox")) return false;
    uint32_t vk = (uint32_t)msg.wParam;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) return false;
    if (editor_->HandleChord(CurrentMods() | vk, (msg.lParam & (1 << 30)) != 0)) return true;
    Act a = keys_.Lookup(CurrentMods() | vk);
    if (a == Act::None) return false;
    DoAction(a);
    return true;
}

void App::Quit() {
    if (quitting_) return;
    quitting_ = true;
    if (editor_) {
        if (editor_->Active()) editor_->Leave();
        editor_->Shutdown();
    }
    if (fullscreen_) SetFullscreen(false);
    SaveResume();
    ShowWindow(hwnd_, SW_HIDE);  // feels instant; libmpv teardown happens behind it
    KillTimer(hwnd_, kTimerUi);
    KillTimer(hwnd_, kTimerIdle);
    KillTimer(hwnd_, kTimerStats);
    if (!util::TestMode()) {
        s_.onTop = onTop_;
        s_.showCaption = captionOn_;
        s_.showControls = controlsOn_;
        s_.mute = bar_.mute;
        ini_.Load(iniPath_);  // keep edits made while we were running
        s_.Save(ini_);
        keys_.WriteDefaults(ini_);
        ini_.Save(iniPath_);
    }
    RetireThumbnails();
    for (auto& t : retiring_) t.join();
    retiring_.clear();
    if (initThread_.joinable()) initThread_.join();
    MSG ready;  // libmpv may have finished starting after we decided to quit
    if (!mpv_ && PeekMessageW(&ready, hwnd_, WM_APP_MPV_READY, WM_APP_MPV_READY, PM_REMOVE)) mpv_ = (mpv_handle*)ready.lParam;
    if (mpv_) {
        g_mpv.mpv_set_wakeup_callback(mpv_, nullptr, nullptr);
        g_mpv.mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
    if (clipProcess_) {
        // An unfinished clip export: stop ffmpeg and remove its partial output.
        TerminateProcess(clipProcess_, 1);
        if (clipThread_.joinable()) clipThread_.join();
        CloseHandle(clipProcess_);
        clipProcess_ = nullptr;
        DeleteFileW(clipOut_.c_str());
    }
    if (clipThread_.joinable()) clipThread_.join();
    if (taskbar_) {
        taskbar_->Release();
        taskbar_ = nullptr;
    }
    for (auto& ic : tbIcons_)
        if (ic) DestroyIcon(ic), ic = nullptr;
    if (comInit_) CoUninitialize();
    if (plWnd_) DestroyWindow(plWnd_);
    RemoveTray();
    DestroyWindow(hwnd_);
    delete editor_;
    editor_ = nullptr;
}
