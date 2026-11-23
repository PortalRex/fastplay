// MPC-HC parity extras: playlist, after-playback, video/audio/subtitle tweaks, taskbar progress and buttons, file
// associations, Open URL, help, FastCut hand-off and lossless clip export.
#include "app.h"
#include "controls.h"
#include "dialogs.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>

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
    for (const auto& f : files)
        if (!f.empty()) full.push_back(f.find(L"://") != std::wstring::npos ? f : util::FullPath(f));
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
    if (!taskbar_) return;
    DWORD now = GetTickCount();
    if (!force && now - lastTaskbarTick_ < 500) return;
    lastTaskbarTick_ = now;
    int state = !bar_.hasFile || bar_.dur <= 0 ? 0 : bar_.paused ? 2 : 1;
    if (state != lastTaskbarState_) {
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
    if (state) taskbar_->SetProgressValue(hwnd_, (ULONGLONG)(bar_.pos * 1000), (ULONGLONG)(bar_.dur * 1000));
}

// ------------------------------------------------------------------------------------------------ video / audio

void App::ToggleVideoFilter(const char* label, const char* filter, const wchar_t* name) {
    // hflip/vflip are software filters: zero-copy GPU frames (d3d11va) can't go through them, so while one is on
    // the decoder runs in copy-back mode, and goes back to zero-copy when both are off.
    std::string spec = std::string("@") + label + ":" + filter;
    std::string vf = mpvx::GetStr(mpv_, "vf");
    bool on = vf.find(std::string("@") + label + ":") == std::string::npos;  // toggling it on
    bool other = vf.find(std::string(label) == "fpmirror" ? "@fpflip:" : "@fpmirror:") != std::string::npos;
    if (on && !other && s_.hwdec.find(L"copy") == std::wstring::npos && s_.hwdec != L"no")
        mpvx::SetStr(mpv_, "hwdec", (util::Utf8(s_.hwdec) == "d3d11va" ? "d3d11va-copy" : "auto-copy"));
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

// ------------------------------------------------------------------------------------------------ FastCut, clips

std::wstring App::FindFastCut() {
    std::vector<std::wstring> c;
    if (!s_.fastcutPath.empty()) c.push_back(s_.fastcutPath);
    std::wstring exe = util::ExeDir();
    c.push_back(exe + L"\\FastCut.exe");
    c.push_back(exe + L"\\..\\FastCut\\FastCut.exe");
    c.push_back(exe + L"\\..\\..\\fastcut\\dist\\FastCut.exe");  // side-by-side source checkouts
    for (auto& p : c)
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return util::FullPath(p);
    return L"";
}

void App::OpenInFastCut() {
    if (path_.empty()) return;
    std::wstring fc = FindFastCut();
    if (fc.empty()) {
        MessageBoxW(hwnd_, L"FastCut.exe wasn't found. Put it next to fastplay.exe or set fastcut_path in fastplay.ini.",
                    L"Edit in FastCut", MB_ICONINFORMATION);
        return;
    }
    mpvx::SetFlag(mpv_, "pause", true);
    wchar_t b[96];
    swprintf(b, 96, L" --seek=%.3f", mpvx::GetDouble(mpv_, "time-pos", 0));
    std::wstring args = Quote(path_) + b;
    if (bar_.abA >= 0 && bar_.abB > bar_.abA) {
        swprintf(b, 96, L" --segment=%.3f,%.3f", bar_.abA, bar_.abB);
        args += b;
    }
    ShellExecuteW(hwnd_, L"open", fc.c_str(), args.c_str(), util::DirName(fc).c_str(), SW_SHOWNORMAL);
}

std::wstring App::FindFfmpeg() {
    std::wstring ffmpeg = s_.ffmpegPath;
    if (ffmpeg.empty()) {
        wchar_t found[MAX_PATH * 2];
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, (DWORD)std::size(found), found, nullptr)) ffmpeg = found;
    }
    return !ffmpeg.empty() && GetFileAttributesW(ffmpeg.c_str()) != INVALID_FILE_ATTRIBUTES ? ffmpeg : L"";
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
        MessageBoxW(hwnd_, L"ffmpeg.exe wasn't found on PATH. Set ffmpeg_path in fastplay.ini.", L"Save clip", MB_ICONINFORMATION);
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
    mpvx::ShowText(mpv_, *m, 4000);
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

bool App::ExtraAction(Act a) {
    switch (a) {
    case Act::Playlist: TogglePlaylistWindow(); return true;
    case Act::OpenUrl: OpenUrlDialog(); return true;
    case Act::Help: ShowHelp(); return true;
    default: break;
    }
    if (!mpv_) return true;
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
    case Act::OpenInFastCut: OpenInFastCut(); return true;
    case Act::SaveClip: SaveClip(); return true;
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

    AppendMenuW(subs, MF_SEPARATOR, 0, nullptr);
    item(subs, Act::SubSizeUp, false, f);
    item(subs, Act::SubSizeDown, false, f);
    item(subs, Act::SubPosUp, false, f);
    item(subs, Act::SubPosDown, false, f);

    // Top level: playlist, tools, help
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(m, Act::Playlist, plWnd_ && IsWindowVisible(plWnd_));
    HMENU tools = CreatePopupMenu();
    item(tools, Act::OpenInFastCut, false, f && path_.find(L"://") == std::wstring::npos);
    item(tools, Act::SaveClip, false, f && bar_.abA >= 0 && bar_.abB > bar_.abA);
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
