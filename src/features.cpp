// MPC-HC parity extras: playlist, after-playback, video/audio/subtitle tweaks, taskbar progress and buttons, file
// associations, Open URL, help and clip export.
#include "app.h"
#include "controls.h"
#include "dialogs.h"
#include "edit/editor.h"
#include "edit/ffrun.h"
#include "edit/exporter.h"
#include "edit/media.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <thread>

namespace {

const wchar_t* kProgId = L"FastPlay.Media";
const wchar_t* kAssocExts[] = {L"mp4", L"m4v", L"mkv", L"webm", L"mov", L"avi", L"wmv", L"flv", L"ts", L"m2ts",
                               L"mts", L"mpg", L"mpeg", L"vob", L"ogv", L"3gp", L"mp3", L"flac", L"wav", L"m4a",
                               L"aac", L"ogg", L"opus", L"wma", L"mka"};

struct Aspect { const wchar_t* label; const char* value; };
const Aspect kAspects[] = {{L"Default", "no"}, {L"4:3", "4:3"}, {L"16:9", "16:9"}, {L"1.85:1", "1.85"},
                           {L"2.35:1", "2.35"}, {L"21:9", "21:9"}, {L"1:1", "1:1"}};

const wchar_t* kAfterNames[] = {L"Do nothing", L"Play next file", L"Repeat file", L"Close file", L"Exit"};
const int kVolMax[] = {100, 150, 200};

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring TimeForName(double t) {
    std::wstring s = util::FormatTime(t, true);
    for (auto& c : s)
        if (c == L':') c = L'.';
    return s;
}

// Small white glyphs for the taskbar thumbnail buttons (alpha from a GDI drawing in white on black).
HICON MakeGlyph(int kind, int size) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, color);
    RECT all = {0, 0, size, size};
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    SelectObject(dc, GetStockObject(WHITE_PEN));
    int u = size / 4, c = size / 2;
    switch (kind) {
    case 0: {  // previous: bar + left triangle
        RECT bar = {u, u, u + 2, size - u};
        FillRect(dc, &bar, (HBRUSH)GetStockObject(WHITE_BRUSH));
        POINT t[] = {{size - u, u}, {size - u, size - u}, {u + 2, c}};
        Polygon(dc, t, 3);
        break;
    }
    case 1: {  // play
        POINT t[] = {{u + 1, u - 1}, {u + 1, size - u + 1}, {size - u + 1, c}};
        Polygon(dc, t, 3);
        break;
    }
    case 2: {  // pause
        RECT a = {u, u, c - 1, size - u}, b = {c + 1, u, size - u, size - u};
        FillRect(dc, &a, (HBRUSH)GetStockObject(WHITE_BRUSH));
        FillRect(dc, &b, (HBRUSH)GetStockObject(WHITE_BRUSH));
        break;
    }
    case 3: {  // next: right triangle + bar
        POINT t[] = {{u, u}, {u, size - u}, {size - u - 2, c}};
        Polygon(dc, t, 3);
        RECT bar = {size - u - 2, u, size - u, size - u};
        FillRect(dc, &bar, (HBRUSH)GetStockObject(WHITE_BRUSH));
        break;
    }
    }
    GdiFlush();
    auto* px = (uint32_t*)bits;
    for (int i = 0; i < size * size; i++) {
        uint32_t a = px[i] & 0xFF;  // white drawing: any channel is the coverage
        px[i] = (a << 24) | (a << 16) | (a << 8) | a;  // premultiplied white
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ playlist

void App::OpenFiles(const std::vector<std::wstring>& files, bool append) {
    std::vector<std::wstring> full;
    for (const auto& f : files) {
        if (f.empty()) continue;
        std::wstring p = f.find(L"://") != std::wstring::npos ? f : util::FullPath(f);
        if (util::Lower(p).size() > 4 && util::Lower(p).compare(p.size() - 4, 4, L".llc") == 0) {
            // A LosslessCut / FastCut project: Edit mode opens its media and applies the segments.
            if (!editor_) editor_ = new edit::Editor(this);
            editor_->LoadProjectFile(p);
            continue;
        }
        full.push_back(p);
    }
    if (full.empty()) return;
    if (append && !plist_.empty()) {
        plist_.insert(plist_.end(), full.begin(), full.end());
        RefreshPlaylistWindow();
        mpvx::ShowText(mpv_, L"Added " + std::to_wstring(full.size()) + L" to the playlist");
        if (path_.empty()) PlaylistActivate(plPos_ < 0 ? 0 : plPos_);
        return;
    }
    plist_ = full;
    plPos_ = 0;
    Open(plist_[0]);  // keeps plist_ because the path is in it
    RefreshPlaylistWindow();
}

bool App::StepPlaylist(int dir) {
    if (plist_.size() < 2) return false;
    int next = plPos_ + dir;
    if (next < 0 || next >= (int)plist_.size()) return false;
    PlaylistActivate(next);
    return true;
}

void App::PlaylistActivate(int index) {
    if (index < 0 || index >= (int)plist_.size()) return;
    plPos_ = index;
    Open(plist_[index]);
    RefreshPlaylistWindow();
}

void App::PlaylistRemove(int index) {
    if (index < 0 || index >= (int)plist_.size()) return;
    plist_.erase(plist_.begin() + index);
    if (index < plPos_) plPos_--;
    else if (index == plPos_) plPos_ = std::min(plPos_, (int)plist_.size() - 1);
    RefreshPlaylistWindow();
}

void App::OnEndOfFile() {
    // Playlists play through first (as in MPC-HC); "after playback" applies at the end of the last entry.
    if (StepPlaylist(1)) return;
    switch (s_.afterPlayback) {
    case 1: StepFile(1); break;
    case 3: CloseFile(); break;
    case 4: if (!util::TestMode()) Quit(); break;
    default: break;  // 0: stay on the last frame; 2: loop-file never reaches the end
    }
}

void App::SetAfterPlayback(int mode) {
    s_.afterPlayback = mode;
    mpvx::SetStr(mpv_, "loop-file", mode == 2 ? "inf" : "no");
    mpvx::ShowText(mpv_, std::wstring(L"After playback: ") + kAfterNames[mode]);
}

void App::TogglePlaylistWindow() {
    if (!plWnd_) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = PlaylistProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 24));
        wc.lpszClassName = L"FastPlay.Playlist";
        RegisterClassExW(&wc);
        RECT mr;
        GetWindowRect(hwnd_, &mr);
        int w = MulDiv(340, dpi_, 96);
        plWnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES, wc.lpszClassName, L"Playlist",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN, mr.right, mr.top, w,
                                 std::max<int>(MulDiv(300, dpi_, 96), mr.bottom - mr.top), hwnd_, nullptr, inst_, this);
        plList_ = CreateWindowExW(0, L"LISTBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                                      LBS_WANTKEYBOARDINPUT | LBS_EXTENDEDSEL,
                                  0, 0, 10, 10, plWnd_, (HMENU)1, inst_, nullptr);
        plFont_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Segoe UI");
        plBrush_ = CreateSolidBrush(RGB(24, 24, 24));
        SendMessageW(plList_, WM_SETFONT, (WPARAM)plFont_, TRUE);
        RECT rc;
        GetClientRect(plWnd_, &rc);
        MoveWindow(plList_, 0, 0, rc.right, rc.bottom, TRUE);
        RefreshPlaylistWindow();
    }
    if (IsWindowVisible(plWnd_)) ShowWindow(plWnd_, SW_HIDE);
    else ShowWindow(plWnd_, util::TestMode() ? SW_SHOWNOACTIVATE : SW_SHOW);
}

void App::RefreshPlaylistWindow() {
    if (!plList_) return;
    SendMessageW(plList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(plList_, LB_RESETCONTENT, 0, 0);
    std::vector<std::wstring> show = plist_;
    if (show.empty() && !path_.empty()) show.push_back(path_);
    for (size_t i = 0; i < show.size(); i++) {
        bool cur = (int)i == plPos_ || (plist_.empty() && i == 0);
        std::wstring t = (cur ? L"▶  " : L"     ") + util::FileName(show[i]);
        SendMessageW(plList_, LB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    SendMessageW(plList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(plList_, nullptr, TRUE);
    std::wstring title = L"Playlist (" + std::to_wstring(show.size()) + L")";
    SetWindowTextW(plWnd_, title.c_str());
}

LRESULT CALLBACK App::PlaylistProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto* self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_SIZE:
        if (self->plList_) MoveWindow(self->plList_, 0, 0, LOWORD(l), HIWORD(l), TRUE);
        return 0;
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)w, RGB(222, 222, 222));
        SetBkColor((HDC)w, RGB(24, 24, 24));
        return (LRESULT)self->plBrush_;
    case WM_COMMAND:
        if (LOWORD(w) == 1 && HIWORD(w) == LBN_DBLCLK) {
            int i = (int)SendMessageW(self->plList_, LB_GETCARETINDEX, 0, 0);
            if (self->plist_.empty() && !self->path_.empty()) self->plist_ = {self->path_};
            self->PlaylistActivate(i);
        }
        return 0;
    case WM_VKEYTOITEM: {
        int i = (int)SendMessageW(self->plList_, LB_GETCARETINDEX, 0, 0);
        if (LOWORD(w) == VK_DELETE) {
            self->PlaylistRemove(i);
            return -2;
        }
        if (LOWORD(w) == VK_RETURN) {
            self->PlaylistActivate(i);
            return -2;
        }
        return -1;
    }
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kCmdAddFiles, L"Add files...");
        AppendMenuW(menu, MF_STRING, kCmdClearPlaylist, L"Clear");
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (p.x == -1) GetCursorPos(&p);
        UINT id = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, h, nullptr);
        DestroyMenu(menu);
        if (id) self->ExtraMenuCommand(id);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        std::vector<std::wstring> files;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t p[MAX_PATH * 4];
            if (DragQueryFileW(drop, i, p, (UINT)std::size(p))) files.push_back(p);
        }
        DragFinish(drop);
        if (self->plist_.empty() && !self->path_.empty()) {
            self->plist_ = {self->path_};
            self->plPos_ = 0;
        }
        self->OpenFiles(files, true);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (self->plFont_) DeleteObject(self->plFont_);
        if (self->plBrush_) DeleteObject(self->plBrush_);
        self->plFont_ = nullptr;
        self->plBrush_ = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------------------------------------ taskbar

void App::OnTaskbarButtonCreated() {
    // The button exists; set up COM/ITaskbarList3 later (after the first frame) so it never delays start-up.
    taskbarWanted_ = s_.taskbarProgress;
    thumbButtons_ = false;
    if (taskbar_ && taskbarWanted_) InitTaskbar();
}

void App::InitTaskbar() {
    if (!taskbarWanted_ || util::TestMode()) return;
    if (!taskbar_) {
        if (!comInit_) comInit_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        ITaskbarList3* t = nullptr;
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t))) || !t) return;
        t->HrInit();
        taskbar_ = t;
    }
    if (!thumbButtons_) {
        int sz = GetSystemMetrics(SM_CXSMICON);
        for (int i = 0; i < 4; i++)
            if (!tbIcons_[i]) tbIcons_[i] = MakeGlyph(i, sz);
        THUMBBUTTON b[3] = {};
        UINT ids[3] = {kCmdThumbPrev, kCmdThumbPlay, kCmdThumbNext};
        const wchar_t* tips[3] = {L"Previous", L"Play/Pause", L"Next"};
        HICON icons[3] = {tbIcons_[0], tbIcons_[bar_.paused ? 1 : 2], tbIcons_[3]};
        for (int i = 0; i < 3; i++) {
            b[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
            b[i].iId = ids[i];
            b[i].hIcon = icons[i];
            b[i].dwFlags = THBF_ENABLED;
            wcscpy_s(b[i].szTip, tips[i]);
        }
        thumbButtons_ = SUCCEEDED(taskbar_->ThumbBarAddButtons(hwnd_, 3, b));
    }
    lastTaskbarState_ = -1;
    UpdateTaskbar(true);
}

void App::UpdateTaskbar(bool force) {
    if (!taskbar_ || (editor_ && editor_->JobRunning())) return;  // an edit job owns the taskbar progress
    DWORD now = GetTickCount();
    if (!force && now - lastTaskbarTick_ < 500) return;
    lastTaskbarTick_ = now;
    int state = !bar_.hasFile || bar_.dur <= 0 ? 0 : bar_.paused ? 2 : 1;
    bool stateChanged = state != lastTaskbarState_;
    if (stateChanged) {
        taskbar_->SetProgressState(hwnd_, state == 0 ? TBPF_NOPROGRESS : state == 2 ? TBPF_PAUSED : TBPF_NORMAL);
        if (thumbButtons_) {
            THUMBBUTTON b = {};
            b.dwMask = THB_ICON;
            b.iId = kCmdThumbPlay;
            b.hIcon = tbIcons_[bar_.paused ? 1 : 2];
            taskbar_->ThumbBarUpdateButtons(hwnd_, 1, &b);
        }
        lastTaskbarState_ = state;
    }
    // The taskbar button is a few dozen pixels wide: a cross-process call only when the bar moves by 1/256.
    int step = state && bar_.dur > 0 ? (int)(std::clamp(bar_.pos / bar_.dur, 0.0, 1.0) * 256) : -1;
    if (state && (force || stateChanged || step != lastTaskbarStep_))
        taskbar_->SetProgressValue(hwnd_, (ULONGLONG)(bar_.pos * 1000), (ULONGLONG)(bar_.dur * 1000));
    lastTaskbarStep_ = step;
}

// ------------------------------------------------------------------------------------------------ video / audio

void App::ToggleVideoFilter(const char* label, const char* filter, const wchar_t* name) {
    // hflip/vflip are software filters: zero-copy GPU frames (d3d11va) can't go through them, so while one is on
    // the decoder runs in copy-back mode, and goes back to zero-copy when both are off.
    std::string spec = std::string("@") + label + ":" + filter;
    std::string vf = mpvx::GetStr(mpv_, "vf");
    bool on = vf.find(std::string("@") + label + ":") == std::string::npos;  // toggling it on
    bool other = vf.find(std::string(label) == "fpmirror" ? "@fpflip:" : "@fpmirror:") != std::string::npos;
    // Only the first entry of the hwdec list decides whether frames stay on the GPU ("d3d11va,auto-copy").
    std::wstring firstHw = s_.hwdec.substr(0, s_.hwdec.find(L','));
    if (on && !other && firstHw.find(L"copy") == std::wstring::npos && firstHw != L"no")
        mpvx::SetStr(mpv_, "hwdec", firstHw == L"d3d11va" ? "d3d11va-copy,auto-copy" : "auto-copy");
    mpvx::Cmd(mpv_, {"vf", "toggle", spec.c_str()});
    if (!on && !other) {
        std::string hw = util::Utf8(s_.hwdec);
        mpvx::SetStr(mpv_, "hwdec", hw.c_str());
    }
    mpvx::ShowText(mpv_, std::wstring(name) + (on ? L": on" : L": off"));
}

void App::AdjustColor(const char* prop, int delta) {
    int v = (int)mpvx::GetInt(mpv_, prop, 0);
    v = delta == 0 ? 0 : std::clamp(v + delta, -100, 100);
    mpvx::SetStr(mpv_, prop, std::to_string(v).c_str());  // integer properties
    if (delta == 0) {
        for (const char* p : {"brightness", "contrast", "saturation", "gamma", "hue"}) mpvx::SetStr(mpv_, p, "0");
        mpvx::ShowText(mpv_, L"Colors reset");
        return;
    }
    mpvx::ShowText(mpv_, util::Wide(prop) + L": " + std::to_wstring(v));
}

std::vector<std::pair<std::string, std::wstring>> App::AudioDevices() {
    std::vector<std::pair<std::string, std::wstring>> r;
    mpv_node n;
    if (!mpv_ || g_mpv.mpv_get_property(mpv_, "audio-device-list", MPV_FORMAT_NODE, &n) < 0) return r;
    if (n.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < n.u.list->num; i++) {
            mpv_node& d = n.u.list->values[i];
            if (d.format != MPV_FORMAT_NODE_MAP) continue;
            std::string name, desc;
            for (int k = 0; k < d.u.list->num; k++) {
                if (d.u.list->values[k].format != MPV_FORMAT_STRING) continue;
                if (!strcmp(d.u.list->keys[k], "name")) name = d.u.list->values[k].u.string;
                else if (!strcmp(d.u.list->keys[k], "description")) desc = d.u.list->values[k].u.string;
            }
            if (!name.empty()) r.push_back({name, util::Wide(desc.empty() ? name : desc)});
        }
    }
    g_mpv.mpv_free_node_contents(&n);
    return r;
}

// ------------------------------------------------------------------------------------------------ file types

void App::SetAssociations(bool on) {
    // Per-user only (HKCU): registers FastPlay as a handler and lists it under "Open with" and Default apps.
    // Windows doesn't let programs make themselves the default; the Default apps page opens so you can pick it.
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    auto setStr = [](const std::wstring& key, const wchar_t* name, const std::wstring& val) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)) return;
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * 2));
        RegCloseKey(k);
    };
    const std::wstring classes = L"Software\\Classes\\";
    if (on) {
        std::wstring cmd = Quote(exe) + L" \"%1\"";
        setStr(classes + kProgId, nullptr, L"FastPlay media file");
        setStr(classes + kProgId + L"\\DefaultIcon", nullptr, std::wstring(exe) + L",0");
        setStr(classes + kProgId + L"\\shell\\open\\command", nullptr, cmd);
        setStr(classes + L"Applications\\fastplay.exe\\shell\\open\\command", nullptr, cmd);
        setStr(classes + L"Applications\\fastplay.exe", L"FriendlyAppName", L"FastPlay");
        setStr(L"Software\\FastPlay\\Capabilities", L"ApplicationName", L"FastPlay");
        setStr(L"Software\\FastPlay\\Capabilities", L"ApplicationDescription", L"Fast video player");
        for (auto e : kAssocExts) {
            std::wstring ext = std::wstring(L".") + e;
            setStr(classes + ext + L"\\OpenWithProgids", kProgId, L"");
            setStr(classes + L"Applications\\fastplay.exe\\SupportedTypes", ext.c_str(), L"");
            setStr(L"Software\\FastPlay\\Capabilities\\FileAssociations", ext.c_str(), kProgId);
        }
        setStr(L"Software\\RegisteredApplications", L"FastPlay", L"Software\\FastPlay\\Capabilities");
    } else {
        RegDeleteTreeW(HKEY_CURRENT_USER, (classes + kProgId).c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, (classes + L"Applications\\fastplay.exe").c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FastPlay");
        HKEY k;
        if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &k)) {
            RegDeleteValueW(k, L"FastPlay");
            RegCloseKey(k);
        }
        for (auto e : kAssocExts) {
            std::wstring key = classes + L"." + e + L"\\OpenWithProgids";
            if (!RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &k)) {
                RegDeleteValueW(k, kProgId);
                RegCloseKey(k);
            }
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (on) {
        ShellExecuteW(hwnd_, L"open", L"ms-settings:defaultapps?registeredAppUser=FastPlay", nullptr, nullptr, SW_SHOWNORMAL);
        MessageBoxW(hwnd_,
                    L"FastPlay is now listed under \"Open with\" for video and audio files.\n\n"
                    L"To make it the default, pick FastPlay in the Default apps page that just opened "
                    L"(Windows only lets you set defaults yourself).",
                    L"File types", MB_ICONINFORMATION);
    } else {
        mpvx::ShowText(mpv_, L"File type registration removed");
    }
}

// ------------------------------------------------------------------------------------------------ clips

std::wstring App::FindFfmpeg() {
    // Same lookup as Edit mode: ffmpeg_path, next to fastplay.exe, then PATH.
    const std::wstring& ffmpeg = edit::ff::Ffmpeg();
    return ffmpeg.find(L'\\') != std::wstring::npos ? ffmpeg : L"";
}

void App::SaveClip() {
    if (path_.empty() || path_.find(L"://") != std::wstring::npos) return;
    if (clipBusy_) {
        mpvx::ShowText(mpv_, L"A clip is still being saved");
        return;
    }
    if (bar_.abA < 0 || bar_.abB <= bar_.abA) {
        mpvx::ShowText(mpv_, L"Set the clip with the A-B loop first ([ = start, ] = end)", 3000);
        return;
    }
    std::wstring ffmpeg = FindFfmpeg();
    if (ffmpeg.empty()) {
        MessageBoxW(hwnd_, edit::ff::MissingToolsMessage().c_str(), L"Save clip", MB_ICONINFORMATION);
        return;
    }
    std::wstring name = util::FileName(path_);
    size_t dot = name.find_last_of(L'.');
    std::wstring base = name.substr(0, dot), ext = dot == std::wstring::npos ? L"mp4" : name.substr(dot + 1);
    std::wstring suggested = base + L" " + TimeForName(bar_.abA) + L"-" + TimeForName(bar_.abB) + L"." + ext;
    std::wstring filter = util::Lower(ext) + L" file";
    filter.push_back(L'\0');
    filter += L"*." + ext;
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    std::wstring out = PickSaveFile(hwnd_, util::DirName(path_), suggested, filter.c_str(), ext.c_str());
    if (out.empty()) return;
    StartClip(out, ffmpeg);
}

void App::StartClip(const std::wstring& out, const std::wstring& ffmpeg) {
    std::wstring name = util::FileName(path_);
    size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"mp4" : name.substr(dot + 1);
    if (util::Lower(util::FullPath(out)) == util::Lower(path_)) {
        MessageBoxW(hwnd_, L"Pick a different file name than the source.", L"Save clip", MB_ICONWARNING);
        return;
    }
    // Stream copy: the clip starts on the keyframe at or before A (FastCut does frame-exact smart cuts).
    std::wstring le = util::Lower(ext);
    std::wstring cmd = Quote(ffmpeg) + L" -hide_banner -nostdin -loglevel error -y";
    wchar_t num[3][32];
    if (indexReady_ && !index_.keyTimes.empty()) {
        // Keyframe K from our header index. The input seek (mfra fast path for fragmented MP4: no fragment scan)
        // may land a fragment early, so with -copyts the output -ss drops everything before K. It sits 0.1 s before
        // K because stream copy compares decode timestamps (the keyframe's is ~2 frames before its pts); leftover
        // non-keyframes are dropped, so the clip's first video frame is K.
        auto it = std::upper_bound(index_.keyTimes.begin(), index_.keyTimes.end(), bar_.abA + 0.0005);
        double k = it == index_.keyTimes.begin() ? 0.0 : *(it - 1);
        swprintf(num[0], 32, L"%.3f", std::max(0.0, k - 0.1));
        swprintf(num[1], 32, L"%.3f", bar_.abB);
        if (index_.fragmented) cmd += L" -use_mfra_for pts";
        cmd += std::wstring(L" -ss ") + num[0] + L" -copyts -i " + Quote(path_) + L" -ss " + num[0] + L" -to " + num[1];
    } else {
        swprintf(num[0], 32, L"%.3f", bar_.abA);
        swprintf(num[1], 32, L"%.3f", bar_.abB - bar_.abA);
        cmd += std::wstring(L" -ss ") + num[0] + L" -i " + Quote(path_) + L" -t " + num[1];
    }
    cmd += L" -map 0 -c copy -avoid_negative_ts make_zero " + Quote(out);
    util::TimingLog(("clip: " + util::Utf8(cmd)).c_str());
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi)) {
        mpvx::ShowText(mpv_, L"Could not start ffmpeg", 3000);
        return;
    }
    CloseHandle(pi.hThread);
    if (clipThread_.joinable()) clipThread_.join();
    clipProcess_ = pi.hProcess;
    clipOut_ = out;
    clipBusy_ = true;
    mpvx::ShowText(mpv_, L"Saving clip...", 2000);
    HWND notify = hwnd_;
    HANDLE proc = pi.hProcess;
    clipThread_ = std::thread([notify, proc, out] {
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        auto* msg = new std::wstring(code == 0 ? L"Clip saved: " + util::FileName(out) : L"Saving the clip failed (ffmpeg error)");
        if (!PostMessageW(notify, WM_APP_CLIP_DONE, code == 0, (LPARAM)msg)) delete msg;
    });
}

void App::OnClipDone(bool ok, std::wstring* msg) {
    std::unique_ptr<std::wstring> m(msg);
    if (clipThread_.joinable()) clipThread_.join();
    if (clipProcess_) CloseHandle(clipProcess_);
    clipProcess_ = nullptr;
    clipBusy_ = false;
    if (!ok) DeleteFileW(clipOut_.c_str());  // our own partial output
    edit::EditSettings es;
    es.Load(ini_);
    if (ok && es.copyToClipboard && util::CopyFilesToClipboard(hwnd_, {clipOut_})) *m += L" (copied: Ctrl+V to paste it)";
    mpvx::ShowText(mpv_, *m, 4000);
}

struct DiscordClipResult {
    bool ok = false, cancelled = false;
    std::wstring msg;
    std::vector<std::wstring> files;
};

// One key from View mode: the A-B range as a Discord-ready MP4 under discord_clip_mb, made by Edit mode's exporter
// (smart cut: H.264 stays lossless except the first GOP; HEVC/AV1/10-bit re-encoded to fit), then put on the
// clipboard. Pressing the key again while it runs cancels it.
void App::DiscordClip() {
    if (path_.empty() || path_.find(L"://") != std::wstring::npos) return;
    if (dclipBusy_) {
        if (dclipCancel_) dclipCancel_->flag = true;
        mpvx::ShowText(mpv_, L"Cancelling the Discord clip...", 2000);
        return;
    }
    if (bar_.abA < 0 || bar_.abB <= bar_.abA) {
        mpvx::ShowText(mpv_, L"Set the clip with the A-B loop first ([ = start, ] = end)", 3000);
        return;
    }
    if (!edit::ff::Available()) {
        MessageBoxW(hwnd_, edit::ff::MissingToolsMessage().c_str(), L"Discord clip", MB_ICONINFORMATION);
        return;
    }
    edit::EditSettings es;
    es.Load(ini_);  // output folder, metadata and fast-start choices from Edit mode's settings
    es.discordReady = true;
    es.discordSizeMB = s_.discordClipMB;
    es.cutMode = L"smart";
    es.merge = false;
    es.format = L"same";  // Discord-ready turns any video into .mp4
    es.nameTemplate = L"{name}-{start}-{end}";
    es.mixAudio = s_.mixAudioTracks;
    wchar_t env[MAX_PATH];
    if (util::TestMode())  // never write next to the user's media from a test copy
        es.outDir = GetEnvironmentVariableW(L"FASTPLAY_EDIT_OUT", env, MAX_PATH) ? env : util::LocalCacheDir();
    edit::Segment seg;
    seg.start = bar_.abA;
    seg.end = bar_.abB;

    if (dclipThread_.joinable()) dclipThread_.join();
    dclipBusy_ = true;
    dclipCancel_ = std::make_shared<edit::Cancel>();
    mpvx::ShowText(mpv_, L"Making a Discord clip (" + edit::Fmt(seg.Duration()) + L")...", 2000);
    HWND notify = hwnd_;
    auto cancel = dclipCancel_;
    std::wstring path = path_;
    dclipThread_ = std::thread([notify, cancel, path, es, seg] {
        auto* r = new DiscordClipResult;
        try {
            edit::MediaInfo m = edit::Probe(path, cancel.get());
            std::vector<int> streams;
            int audio = 0;
            for (const auto& st : m.streams) {
                if (!st.keep) continue;
                if (st.encrypted) throw edit::EditError(L"This file is DRM-protected (copy-protected tracks), so it can't be cut.");
                streams.push_back(st.index);
                audio += st.type == "audio";
            }
            edit::KeyframeIndex kfi = m.Video() ? edit::Keyframes(m, m.Video()->index, cancel.get()) : edit::KeyframeIndex{};
            std::atomic<int> lastPct{-2};
            edit::ProgressFn progress = [&lastPct, notify](double f, const std::wstring& msg) {
                int pct = f < 0 ? -1 : (int)(std::clamp(f, 0.0, 1.0) * 100);
                if (lastPct.exchange(pct) == pct) return;
                auto* text = new std::wstring(L"Discord clip: " + (pct >= 0 ? std::to_wstring(pct) + L"% " : L"") + msg);
                if (!PostMessageW(notify, WM_APP_DCLIP_PROGRESS, 0, (LPARAM)text)) delete text;
            };
            edit::Exporter ex(m, es, streams, -1, 1, kfi, progress);
            r->files = ex.Run({seg}, cancel.get());
            if (!r->files.empty()) {
                WIN32_FILE_ATTRIBUTE_DATA fa = {};
                GetFileAttributesExW(r->files[0].c_str(), GetFileExInfoStandard, &fa);
                uint64_t size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                if (size < 1024) {  // ffmpeg can succeed without writing frames (a range past the end of a recording in progress)
                    DeleteFileW(r->files[0].c_str());
                    throw edit::EditError(L"the clip came out empty (is the A-B range inside the recording?)");
                }
                wchar_t mb[32];
                swprintf(mb, 32, L"%.1f MB", size / 1e6);
                r->msg = L"Discord clip: " + util::FileName(r->files[0]) + L" (" + mb;
                if (es.mixAudio && audio >= 2) r->msg += L", " + std::to_wstring(audio) + L" audio tracks mixed";
                r->msg += L")";
                r->ok = true;
            }
        } catch (const edit::Cancelled&) {
            r->cancelled = true;
        } catch (const edit::EditError& e) {
            r->msg = e.msg;
        } catch (const std::exception& e) {
            r->msg = util::Wide(e.what());
        }
        if (!PostMessageW(notify, WM_APP_DCLIP_DONE, 0, (LPARAM)r)) delete r;
    });
}

void App::OnDiscordClipDone(DiscordClipResult* raw) {
    std::unique_ptr<DiscordClipResult> r(raw);
    if (dclipThread_.joinable()) dclipThread_.join();
    dclipBusy_ = false;
    dclipCancel_.reset();
    util::TimingLog(("discord clip: " + std::string(r->ok ? "ok " : r->cancelled ? "cancelled " : "failed ") +
                     util::Utf8(r->msg)).c_str());
    if (r->cancelled) {
        mpvx::ShowText(mpv_, L"Discord clip cancelled", 2000);
    } else if (!r->ok) {
        std::wstring last = r->msg;
        size_t nl = last.find_last_of(L'\n');
        if (nl != std::wstring::npos) last = last.substr(nl + 1);
        mpvx::ShowText(mpv_, L"Discord clip failed: " + last, 6000);
    } else {
        bool copied = util::CopyFilesToClipboard(hwnd_, r->files);
        mpvx::ShowText(mpv_, r->msg + (copied ? L" copied: Ctrl+V to paste it" : L""), 5000);
    }
}

// ------------------------------------------------------------------------------------------------ help, URL

void App::ShowHelp() {
    std::wstring t = L"Keyboard and mouse shortcuts (change them in fastplay.ini, [keys]; Options opens it)\n\n";
    int n;
    const ActionInfo* acts = Actions(&n);
    for (int i = 0; i < n; i++) {
        std::wstring k = keys_.AllShortcuts(acts[i].act);
        if (k.empty()) continue;
        std::wstring label = acts[i].label;
        label.resize(std::max<size_t>(label.size(), 34), L' ');
        t += label + L"  " + k + L"\n";
    }
    t += L"\nUnbound (menu only or set your own key):\n";
    for (int i = 0; i < n; i++)
        if (keys_.AllShortcuts(acts[i].act).empty()) t += std::wstring(L"  ") + acts[i].label + L"   [" + acts[i].id + L"]\n";
    t += L"\nClick the video: play/pause. Drag it: move the window. Right-click: menu.\n";
    ShowTextBox(hwnd_, L"FastPlay shortcuts", t);
}

void App::OpenUrlDialog() {
    std::wstring url;
    if (!InputBox(hwnd_, L"Open URL", L"Direct media or stream URL (http, https, rtmp, ...):", &url)) return;
    url = util::Trim(url);
    if (!url.empty()) OpenFiles({url}, false);
}

// ------------------------------------------------------------------------------------------------ actions, menus

void App::ZoomAutofitLarger() {
    // MPC-HC "Auto fit (larger only)": shrink to fit when the video is bigger than the auto-fit area, else 100 %.
    if (vidW_ <= 0 || vidH_ <= 0) return;
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    double maxW = (mi.rcWork.right - mi.rcWork.left) * s_.autofitPercent / 100.0;
    double maxH = (mi.rcWork.bottom - mi.rcWork.top) * s_.autofitPercent / 100.0;
    if (vidW_ > maxW || vidH_ > maxH) ResizeToVideo(0, true);
    else ResizeToVideo(1, false);
}

void App::RecycleCurrent() {
    // MPC-HC's Delete: ask (Windows itself no longer does by default), move to the next file, then recycle.
    if (path_.empty() || path_.find(L"://") != std::wstring::npos) return;
    std::wstring victim = path_;
    mpvx::SetFlag(mpv_, "pause", true);
    std::wstring msg = L"Move file to the Recycle Bin?\n\n" + victim;
    if (util::TestMode()) {  // test copies never delete anything and never show dialogs
        util::TimingLog(("recycle: would ask: " + util::Utf8(victim)).c_str());
        return;
    }
    if (MessageBoxW(hwnd_, msg.c_str(), L"FastPlay", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    int idx = plPos_;
    if (idx >= 0 && idx < (int)plist_.size() && plist_.size() > 1) {
        PlaylistRemove(idx);
        PlaylistActivate(std::min(idx, (int)plist_.size() - 1));
    } else {
        StepFile(1);
        if (path_ == victim) StepFile(-1);
        if (path_ == victim) CloseFile();
    }
    HWND hwnd = hwnd_;
    std::thread([hwnd, victim] {
        // libmpv, the thumbnailer and the header reader let go of the file within moments of the switch.
        std::wstring from = victim + L'\0';  // double-NUL terminated list
        bool ok = false;
        for (int i = 0; i < 30 && !ok; i++) {
            SHFILEOPSTRUCTW op = {};
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted &&
                 GetFileAttributesW(victim.c_str()) == INVALID_FILE_ATTRIBUTES;
            if (!ok) Sleep(200);
        }
        auto* name = new std::wstring(util::FileName(victim));
        if (!PostMessageW(hwnd, WM_APP_RECYCLED, ok, (LPARAM)name)) delete name;
    }).detach();
}

bool App::ExtraAction(Act a) {
    switch (a) {
    case Act::Playlist: TogglePlaylistWindow(); return true;
    case Act::OpenUrl: OpenUrlDialog(); return true;
    case Act::Help: ShowHelp(); return true;
    default: break;
    }
    if (!mpv_) return true;
    switch (a) {
    case Act::Reopen: {
        if (path_.empty()) return true;
        double pos = mpvx::GetDouble(mpv_, "time-pos", 0);
        startOverride_ = pos;  // opens fast on the keyframe before...
        Open(path_);
        exactSeekAfterOpen_ = pos;  // ...then lands on the exact frame, like MPC-HC
        return true;
    }
    case Act::Recycle: RecycleCurrent(); return true;
    case Act::LoadSubtitles: if (!path_.empty()) OnMenuCommand(kCmdLoadSub); return true;
    case Act::OpenLocation: if (!path_.empty() && path_.find(L"://") == std::wstring::npos) OnMenuCommand(kCmdOpenFolder); return true;
    case Act::BossKey:
        mpvx::SetFlag(mpv_, "pause", true);
        if (fullscreen_) DoAction(Act::Fullscreen);
        ShowWindow(hwnd_, SW_MINIMIZE);
        return true;
    case Act::OsdTime:
        // MPC-HC keeps the time on screen until toggled off; mpv's OSD level 3 does the same.
        osdTime_ = !osdTime_;
        mpvx::SetStr(mpv_, "osd-level", osdTime_ ? "3" : "1");
        return true;
    case Act::OsdFilename: if (!path_.empty()) mpvx::ShowText(mpv_, util::FileName(path_), 2000); return true;
    case Act::SubLinePrev: mpvx::CmdAsync(mpv_, {"sub-seek", "-1"}); return true;
    case Act::SubLineNext: mpvx::CmdAsync(mpv_, {"sub-seek", "1"}); return true;
    case Act::SubShiftUp: mpvx::CmdAsync(mpv_, {"osd-msg", "sub-step", "-1"}); return true;
    case Act::SubShiftDown: mpvx::CmdAsync(mpv_, {"osd-msg", "sub-step", "1"}); return true;
    case Act::Zoom25: ResizeToVideo(0.25, false); return true;
    case Act::ZoomAutofitLarger: ZoomAutofitLarger(); return true;
    case Act::WidthUp: case Act::WidthDown: case Act::HeightUp: case Act::HeightDown: {
        // MPC-HC: 2 % per press, 0.2x-5x.
        double& v = a == Act::WidthUp || a == Act::WidthDown ? scaleX_ : scaleY_;
        v = std::clamp(a == Act::WidthUp || a == Act::HeightUp ? v * 1.02 : v / 1.02, 0.2, 5.0);
        mpvx::SetDouble(mpv_, "video-scale-x", scaleX_);
        mpvx::SetDouble(mpv_, "video-scale-y", scaleY_);
        wchar_t b[64];
        swprintf(b, 64, L"Stretch %.0f%% x %.0f%%", scaleX_ * 100, scaleY_ * 100);
        mpvx::ShowText(mpv_, b);
        return true;
    }
    case Act::PanUpLeft: PanZoom(-0.02, -0.02, 0); return true;
    case Act::PanUpRight: PanZoom(0.02, -0.02, 0); return true;
    case Act::PanDownLeft: PanZoom(-0.02, 0.02, 0); return true;
    case Act::PanDownRight: PanZoom(0.02, 0.02, 0); return true;
    case Act::PanCenter: if (panX_ || panY_) PanZoom(-panX_, -panY_, 0); return true;
    default: break;
    }
    switch (a) {
    case Act::AspectNext:
        aspectIdx_ = (aspectIdx_ + 1) % (int)std::size(kAspects);
        mpvx::SetStr(mpv_, "video-aspect-override", kAspects[aspectIdx_].value);
        mpvx::ShowText(mpv_, std::wstring(L"Aspect ratio: ") + kAspects[aspectIdx_].label);
        return true;
    case Act::RotateCW:
    case Act::RotateCCW:
        rotate_ = (rotate_ + (a == Act::RotateCW ? 90 : 270)) % 360;
        mpvx::SetStr(mpv_, "video-rotate", std::to_string(rotate_).c_str());  // integer property
        mpvx::ShowText(mpv_, L"Rotation: " + std::to_wstring(rotate_) + L"°");
        return true;
    case Act::FlipH: ToggleVideoFilter("fpmirror", "hflip", L"Mirror"); return true;
    case Act::FlipV: ToggleVideoFilter("fpflip", "vflip", L"Flip"); return true;
    case Act::Deinterlace:
        mpvx::CmdAsync(mpv_, {"osd-msg", "cycle", "deinterlace"});
        return true;
    case Act::BrightnessUp: AdjustColor("brightness", 5); return true;
    case Act::BrightnessDown: AdjustColor("brightness", -5); return true;
    case Act::ContrastUp: AdjustColor("contrast", 5); return true;
    case Act::ContrastDown: AdjustColor("contrast", -5); return true;
    case Act::SaturationUp: AdjustColor("saturation", 5); return true;
    case Act::SaturationDown: AdjustColor("saturation", -5); return true;
    case Act::ColorReset: AdjustColor("brightness", 0); return true;
    case Act::Normalize:
        mpvx::CmdAsync(mpv_, {"af", "toggle", "@fpnorm:lavfi=[dynaudnorm=f=250:g=15]"});
        mpvx::ShowText(mpv_, L"Normalize volume (toggled)");
        return true;
    case Act::SubSizeUp: mpvx::CmdAsync(mpv_, {"osd-msg", "add", "sub-scale", "0.1"}); return true;
    case Act::SubSizeDown: mpvx::CmdAsync(mpv_, {"osd-msg", "add", "sub-scale", "-0.1"}); return true;
    case Act::SubPosUp: mpvx::CmdAsync(mpv_, {"osd-msg", "add", "sub-pos", "-2"}); return true;
    case Act::SubPosDown: mpvx::CmdAsync(mpv_, {"osd-msg", "add", "sub-pos", "2"}); return true;
    case Act::RepeatFile: SetAfterPlayback(s_.afterPlayback == 2 ? 0 : 2); return true;
    case Act::SaveClip: SaveClip(); return true;
    case Act::DiscordClip: DiscordClip(); return true;
    case Act::MixAudio:
        s_.mixAudioTracks = !s_.mixAudioTracks;
        ApplyAudioMix(true);
        if (!audioMixed_) mpvx::ShowText(mpv_, s_.mixAudioTracks ? L"Mix all audio tracks: on" : L"Mix all audio tracks: off");
        return true;
    default: return false;
    }
}

void App::BuildExtraMenus(HMENU m, HMENU play, HMENU video, HMENU audio, HMENU subs) {
    auto item = [&](HMENU menu, Act a, bool checked = false, bool enabled = true) {
        const ActionInfo* info = FindAction(a);
        std::wstring t = info->label;
        std::wstring k = keys_.ShortcutText(a);
        if (!k.empty()) t += L"\t" + k;
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), kCmdAction + (UINT)a, t.c_str());
    };
    bool f = bar_.hasFile && mpv_;

    HMENU after = CreatePopupMenu();
    for (int i = 0; i < (int)std::size(kAfterNames); i++)
        AppendMenuW(after, MF_STRING | (s_.afterPlayback == i ? MF_CHECKED : 0), kCmdAfter + i, kAfterNames[i]);
    AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(play, MF_POPUP, (UINT_PTR)after, L"After playback");

    HMENU aspect = CreatePopupMenu();
    for (int i = 0; i < (int)std::size(kAspects); i++)
        AppendMenuW(aspect, MF_STRING | (aspectIdx_ == i ? MF_CHECKED : 0) | (f ? 0 : MF_GRAYED), kCmdAspect + i, kAspects[i].label);
    AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(video, MF_POPUP, (UINT_PTR)aspect, L"Aspect ratio");
    item(video, Act::RotateCW, false, f);
    item(video, Act::RotateCCW, false, f);
    item(video, Act::FlipH, false, f);
    item(video, Act::FlipV, false, f);
    item(video, Act::Deinterlace, f && mpvx::GetFlag(mpv_, "deinterlace"), f);
    HMENU color = CreatePopupMenu();
    item(color, Act::BrightnessUp, false, f);
    item(color, Act::BrightnessDown, false, f);
    item(color, Act::ContrastUp, false, f);
    item(color, Act::ContrastDown, false, f);
    item(color, Act::SaturationUp, false, f);
    item(color, Act::SaturationDown, false, f);
    item(color, Act::ColorReset, false, f);
    AppendMenuW(video, MF_POPUP, (UINT_PTR)color, L"Color");

    AppendMenuW(audio, MF_SEPARATOR, 0, nullptr);
    HMENU dev = CreatePopupMenu();
    auto devs = AudioDevices();
    std::string cur = mpvx::GetStr(mpv_, "audio-device");
    audioDevNames_.clear();
    for (size_t i = 0; i < devs.size() && i < 90; i++) {
        audioDevNames_.push_back(devs[i].first);
        std::wstring label = devs[i].first == "auto" ? L"Default device" : devs[i].second;
        AppendMenuW(dev, MF_STRING | (devs[i].first == cur ? MF_CHECKED : 0), kCmdAudioDev + (UINT)i, label.c_str());
    }
    if (devs.empty()) AppendMenuW(dev, MF_STRING | MF_GRAYED, 0, L"(not available yet)");
    AppendMenuW(audio, MF_POPUP, (UINT_PTR)dev, L"Output device");
    HMENU boost = CreatePopupMenu();
    for (int i = 0; i < (int)std::size(kVolMax); i++) {
        std::wstring t = kVolMax[i] == 100 ? L"Off (max 100%)" : L"Up to " + std::to_wstring(kVolMax[i]) + L"%";
        AppendMenuW(boost, MF_STRING | (s_.volumeMax == kVolMax[i] ? MF_CHECKED : 0), kCmdVolMax + i, t.c_str());
    }
    AppendMenuW(audio, MF_POPUP, (UINT_PTR)boost, L"Volume boost");
    item(audio, Act::Normalize, false, f);
    item(audio, Act::MixAudio, s_.mixAudioTracks);

    AppendMenuW(subs, MF_SEPARATOR, 0, nullptr);
    item(subs, Act::SubSizeUp, false, f);
    item(subs, Act::SubSizeDown, false, f);
    item(subs, Act::SubPosUp, false, f);
    item(subs, Act::SubPosDown, false, f);

    // Top level: playlist, tools, help
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(m, Act::Playlist, plWnd_ && IsWindowVisible(plWnd_));
    HMENU tools = CreatePopupMenu();
    item(tools, Act::SaveClip, false, f && bar_.abA >= 0 && bar_.abB > bar_.abA);
    item(tools, Act::DiscordClip, false, f && bar_.abA >= 0 && bar_.abB > bar_.abA);
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, kCmdAssocOn, L"Register as video player (Open with / Default apps)...");
    AppendMenuW(tools, MF_STRING, kCmdAssocOff, L"Remove file type registration");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tools, L"Tools");
    HMENU help = CreatePopupMenu();
    item(help, Act::Help);
    AppendMenuW(help, MF_STRING, kCmdAbout, L"About FastPlay");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)help, L"Help");
}

bool App::ExtraMenuCommand(UINT id) {
    if (id >= kCmdAudioDev && id < kCmdAudioDev + 100) {
        size_t i = id - kCmdAudioDev;
        if (i < audioDevNames_.size()) mpvx::SetStr(mpv_, "audio-device", audioDevNames_[i].c_str());
        return true;
    }
    if (id >= kCmdAspect && id < kCmdAspect + std::size(kAspects)) {
        aspectIdx_ = id - kCmdAspect;
        mpvx::SetStr(mpv_, "video-aspect-override", kAspects[aspectIdx_].value);
        return true;
    }
    if (id >= kCmdAfter && id < kCmdAfter + std::size(kAfterNames)) {
        SetAfterPlayback(id - kCmdAfter);
        return true;
    }
    if (id >= kCmdVolMax && id < kCmdVolMax + std::size(kVolMax)) {
        s_.volumeMax = kVolMax[id - kCmdVolMax];
        mpvx::SetDouble(mpv_, "volume-max", s_.volumeMax);
        bar_.volumeMax = s_.volumeMax;
        if (bar_.volume > s_.volumeMax) SetVolume(s_.volumeMax);
        RefreshBar();
        return true;
    }
    switch (id) {
    case kCmdThumbPrev: DoAction(Act::PrevChapter); return true;
    case kCmdThumbPlay: DoAction(Act::PlayPause); return true;
    case kCmdThumbNext: DoAction(Act::NextChapter); return true;
    case kCmdAssocOn: SetAssociations(true); return true;
    case kCmdAssocOff: SetAssociations(false); return true;
    case kCmdAddFiles: {
        auto files = PickOpenFiles(plWnd_ ? plWnd_ : hwnd_, path_.empty() ? L"" : util::DirName(path_));
        if (plist_.empty() && !path_.empty()) {
            plist_ = {path_};
            plPos_ = 0;
        }
        OpenFiles(files, true);
        return true;
    }
    case kCmdClearPlaylist:
        plist_.clear();
        plPos_ = -1;
        if (!path_.empty()) {
            plist_ = {path_};
            plPos_ = 0;
        }
        RefreshPlaylistWindow();
        return true;
    case kCmdAbout: {
        std::wstring t = L"FastPlay 0.2\nA fast libmpv video player for Windows.\n\nlibmpv: " +
                         util::Wide(mpvx::GetStr(mpv_, "mpv-version")) + L"\nSettings: " + iniPath_;
        MessageBoxW(hwnd_, t.c_str(), L"About FastPlay", MB_OK);
        return true;
    }
    }
    return false;
}
