// Render + OK-handler test for src/edit/eddialogs.cpp. Never shows anything on a monitor: each dialog is created
// hidden and modeless, moved past the right edge of the virtual screen, shown without activation, captured with
// PrintWindow and destroyed. Focus is never moved (the test hook makes the dialogs leave it alone).
//   tools\edit-tests\build-dialogs.cmd   ->  images in bench\edit-tests\dialogs
#include "../../src/edit/eddialogs.h"
#include <cstdio>
#include <cwchar>
#include <cmath>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace edit::dialogs::detail {
extern int g_themeOverride;
extern bool g_testNoFocus;
HWND CreateForTest(int which, HWND owner, Segment* seg, EditSettings* s, double* speed, std::wstring* v1,
                   std::wstring* v2);
bool OkForTest(HWND dlg, std::wstring* error);
}  // namespace edit::dialogs::detail

// ---- stand-ins for common.cpp (not written yet when this test was made) ----
namespace edit {
std::wstring Num(double v) {
    wchar_t b[64];
    swprintf(b, 64, L"%.6f", v);
    std::wstring s = b;
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s == L"-0" ? L"0" : s;
}
std::wstring Fmt(double sec, bool millis) {
    int ms = (int)std::lround(sec * 1000);
    int h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, f = ms % 1000;
    wchar_t b[64];
    if (h) swprintf(b, 64, millis ? L"%d:%02d:%02d.%03d" : L"%d:%02d:%02d", h, m, s, f);
    else swprintf(b, 64, millis ? L"%02d:%02d.%03d" : L"%02d:%02d", m, s, f);
    return b;
}
bool TryParseTime(const std::wstring& text, double* seconds) {
    double total = 0;
    const wchar_t* p = text.c_str();
    for (;;) {
        wchar_t* end;
        double part = wcstod(p, &end);
        if (end == p) return false;
        total = total * 60 + part;
        if (*end == L':') {
            p = end + 1;
            continue;
        }
        if (*end) return false;
        *seconds = total;
        return true;
    }
}
}  // namespace edit

namespace dd = edit::dialogs::detail;

static int g_fail = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            printf("FAIL line %d: %s\n", __LINE__, #c);               \
            ++g_fail;                                                 \
        }                                                             \
    } while (0)

static void Pump(int ms) {
    DWORD until = GetTickCount() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        int left = (int)(until - GetTickCount());
        if (left <= 0) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, left, QS_ALLINPUT);
    }
}

static void CheckNotForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid == GetCurrentProcessId()) {
        printf("FAIL: a test window became the foreground window\n");
        ++g_fail;
    }
}

static bool SaveBmp(HWND h, const std::wstring& path, UINT flags) {
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, ht = r.bottom - r.top;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -ht, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL ok = PrintWindow(h, mem, flags);
    SelectObject(mem, old);
    bool saved = false;
    if (ok) {
        BITMAPFILEHEADER fh = {0x4D42};
        DWORD size = (DWORD)w * ht * 4;
        fh.bfOffBits = sizeof fh + sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + size;
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
            fwrite(bits, size, 1, f);
            fclose(f);
            saved = true;
        }
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return saved;
}

static const wchar_t* kNames[] = {L"segment", L"export", L"settings", L"batch", L"prompt", L"prompt2", L"viewer"};

struct Data {
    edit::Segment seg;
    edit::EditSettings s;
    double speed = 1;
    std::wstring v1, v2;
};

static void Render(HWND owner, const std::wstring& outDir, int offX) {
    for (int theme = 0; theme < 2; ++theme) {
        dd::g_themeOverride = theme;
        for (int which = 0; which < 7; ++which) {
            Data d;
            d.seg = {12.5, 74.25, L"Intro", L"scene=1, cam=a", true, 0};
            d.v1 = which == 6 ? L"ffmpeg version 7.1 Copyright (c) 2000-2024 the FFmpeg developers\n"
                                L"Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'C:\\Videos\\clip.mp4':\n"
                                L"  Duration: 00:01:14.25, start: 0.000000, bitrate: 8123 kb/s\n"
                                L"  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(tv), 1920x1080, "
                                L"8000 kb/s, 60 fps, 60 tbr, 15360 tbn (default) handler_name=VideoHandler\n"
                              : which == 5 ? L"-50" : L"1";
            d.v2 = which == 5 ? L"1" : L"";
            if (which == 6)
                for (int i = 0; i < 80; ++i) d.v1 += L"frame=" + std::to_wstring(i * 60) + L" fps=240 size=1024kB\n";
            HWND h = dd::CreateForTest(which, owner, &d.seg, &d.s, &d.speed, &d.v1, which == 6 ? nullptr : &d.v2);
            if (!h) {
                printf("FAIL: could not create dialog %d\n", which);
                ++g_fail;
                continue;
            }
            SetWindowPos(h, nullptr, offX, 50, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(h, SW_SHOWNOACTIVATE);
            UpdateWindow(h);
            Pump(800);
            CheckNotForeground();
            std::wstring path = outDir + L"\\" + kNames[which] + (theme ? L"-dark.bmp" : L"-light.bmp");
            // The DWM capture of an off-screen window is sometimes only partly painted; the WM_PRINT capture
            // ("-print") is always complete, so look at that one first.
            if (!SaveBmp(h, path, PW_RENDERFULLCONTENT) || !SaveBmp(h, path.substr(0, path.size() - 4) + L"-print.bmp", 0)) {
                printf("FAIL: capture %ls\n", path.c_str());
                ++g_fail;
            }
            RECT r;
            GetWindowRect(h, &r);
            printf("rendered %-8ls %-5s %ldx%ld px\n", kNames[which], theme ? "dark" : "light", r.right - r.left,
                   r.bottom - r.top);
            DestroyWindow(h);
        }
    }
    dd::g_themeOverride = -1;
}

static void Set(HWND d, int id, const wchar_t* t) { SetDlgItemTextW(d, id, t); }

static void UnitTests(HWND owner) {
    std::wstring err;
    // Segment
    {
        Data d;
        d.seg = {1, 2, L"a", L"", true, 3};
        HWND h = dd::CreateForTest(0, owner, &d.seg, &d.s, nullptr, nullptr, nullptr);
        Set(h, 101, L"10");
        Set(h, 102, L"5");
        EXPECT(!dd::OkForTest(h, &err) && err.find(L"end must be after start") != std::wstring::npos);
        EXPECT(d.seg.start == 1 && d.seg.end == 2);  // untouched on refusal
        Set(h, 101, L"abc");
        EXPECT(!dd::OkForTest(h, &err) && err.find(L"Start") == 0);
        Set(h, 101, L"1:02.5");
        Set(h, 102, L"1:10");
        Set(h, 103, L"  Label x ");
        Set(h, 104, L" k=v ");
        CheckDlgButton(h, 105, BST_UNCHECKED);
        EXPECT(dd::OkForTest(h, &err));
        EXPECT(d.seg.start == 62.5 && d.seg.end == 70 && d.seg.name == L"Label x" && d.seg.tags == L"k=v" &&
               !d.seg.enabled && d.seg.color == 3);
        DestroyWindow(h);
    }
    // Export
    {
        Data d;
        d.s.format = L"mkv";
        d.s.cutMode = L"smart";
        d.s.discordSizeMB = 10;
        HWND h = dd::CreateForTest(1, owner, nullptr, &d.s, &d.speed, nullptr, nullptr);
        EXPECT(SendDlgItemMessageW(h, 204, CB_GETCURSEL, 0, 0) == 2);
        EXPECT(SendDlgItemMessageW(h, 205, CB_GETCURSEL, 0, 0) == 2);
        wchar_t buf[64];
        SendDlgItemMessageW(h, 204, CB_GETLBTEXT, 0, (LPARAM)buf);
        EXPECT(wcscmp(buf, L"Same as input (.mp4)") == 0);
        Set(h, 208, L"0");
        EXPECT(!dd::OkForTest(h, &err) && err == L"Speed must be a positive number.");
        Set(h, 208, L"1.5x");
        EXPECT(!dd::OkForTest(h, &err));
        Set(h, 208, L"2,5");
        Set(h, 209, L"-1");
        EXPECT(!dd::OkForTest(h, &err) && err.find(L"Size limit") == 0);
        Set(h, 209, L"1.5");
        EXPECT(!dd::OkForTest(h, &err));
        EXPECT(d.speed == 1 && d.s.format == L"mkv");  // untouched on refusal
        Set(h, 209, L" 50 ");
        Set(h, 201, L"  D:\\out  ");
        Set(h, 206, L"   ");
        Set(h, 207, L"{name}-m");
        SendDlgItemMessageW(h, 203, CB_SETCURSEL, 1, 0);
        SendDlgItemMessageW(h, 204, CB_SETCURSEL, 13, 0);
        SendDlgItemMessageW(h, 205, CB_SETCURSEL, 1, 0);
        CheckDlgButton(h, 210, BST_UNCHECKED);
        CheckDlgButton(h, 216, BST_UNCHECKED);
        CheckDlgButton(h, 217, BST_UNCHECKED);
        EXPECT(dd::OkForTest(h, &err));
        EXPECT(d.speed == 2.5 && d.s.discordSizeMB == 50 && d.s.outDir == L"D:\\out" && d.s.merge &&
               d.s.format == L"avi" && d.s.cutMode == L"exact" && d.s.nameTemplate == L"{name}-{n}-{label}" &&
               d.s.mergeTemplate == L"{name}-m" && !d.s.preserveMetadata && !d.s.verifyOutputs &&
               !d.s.confirmExport && d.s.preserveChapters && d.s.discordReady);
        EXPECT(edit::dialogs::kFormatCount == 14 && wcscmp(edit::dialogs::kFormats[0], L"same") == 0);
        DestroyWindow(h);
    }
    // Settings
    {
        Data d;
        HWND h = dd::CreateForTest(2, owner, nullptr, &d.s, nullptr, nullptr, nullptr);
        wchar_t buf[64];
        GetDlgItemTextW(h, 306, buf, 64);
        EXPECT(wcscmp(buf, L"1") == 0);
        Set(h, 306, L"0.01");
        EXPECT(!dd::OkForTest(h, &err) && err.find(L"Small seek") == 0);
        Set(h, 306, L"0.25");
        Set(h, 307, L"5000");
        EXPECT(!dd::OkForTest(h, &err) && err.find(L"Large seek") == 0);
        Set(h, 307, L"30");
        Set(h, 308, L"2.5");
        EXPECT(!dd::OkForTest(h, &err) && err.find(L"Frame jump") == 0);
        Set(h, 308, L"25");
        Set(h, 309, L"0");
        EXPECT(!dd::OkForTest(h, &err) && err.find(L"Nudge") == 0);
        EXPECT(d.s.seekSmall == 1);  // untouched on refusal
        Set(h, 309, L"2");
        Set(h, 303, L"");
        Set(h, 304, L"{name}-all");
        SendDlgItemMessageW(h, 305, CB_SETCURSEL, 1, 0);
        CheckDlgButton(h, 311, BST_UNCHECKED);
        CheckDlgButton(h, 313, BST_UNCHECKED);
        EXPECT(dd::OkForTest(h, &err));
        EXPECT(d.s.seekSmall == 0.25 && d.s.seekLarge == 30 && d.s.frameJump == 25 && d.s.nudgeFrames == 2 &&
               d.s.nameTemplate == L"{name}-{n}-{label}" && d.s.mergeTemplate == L"{name}-all" &&
               d.s.snapshotFormat == L"png" && !d.s.showWave && d.s.showThumbs && !d.s.autoSaveProject);
        DestroyWindow(h);
    }
    // Batch
    {
        Data d;
        d.s.batchMode = L"saved";
        d.s.batchTrimStart = 1.5;
        HWND h = dd::CreateForTest(3, owner, nullptr, &d.s, nullptr, nullptr, nullptr);
        EXPECT(IsDlgButtonChecked(h, 402) == BST_CHECKED && IsDlgButtonChecked(h, 401) == BST_UNCHECKED);
        wchar_t buf[64];
        GetDlgItemTextW(h, 404, buf, 64);
        EXPECT(wcscmp(buf, L"1.5") == 0);
        Set(h, 405, L"-2");
        EXPECT(!dd::OkForTest(h, &err) && err.find(L"Trim amounts") == 0);
        Set(h, 405, L"1:30");
        CheckRadioButton(h, 401, 403, 403);
        EXPECT(dd::OkForTest(h, &err));
        EXPECT(d.s.batchMode == L"trim" && d.s.batchTrimStart == 1.5 && d.s.batchTrimEnd == 90);
        DestroyWindow(h);
    }
    // Prompt / Prompt2
    {
        Data d;
        d.v1 = L"10";
        HWND h = dd::CreateForTest(4, owner, nullptr, nullptr, nullptr, &d.v1, nullptr);
        Set(h, 501, L" 30s ");
        EXPECT(dd::OkForTest(h, &err) && d.v1 == L" 30s ");
        DestroyWindow(h);
        d.v1 = L"-50";
        d.v2 = L"1";
        h = dd::CreateForTest(5, owner, nullptr, nullptr, nullptr, &d.v1, &d.v2);
        Set(h, 501, L"-40");
        Set(h, 502, L"2.5");
        EXPECT(dd::OkForTest(h, &err) && d.v1 == L"-40" && d.v2 == L"2.5");
        DestroyWindow(h);
    }
    // Text viewer: CRLF conversion and live refresh.
    {
        Data d;
        d.v1 = L"line 1\nline 2";
        d.v2 = L"line 1\nline 2\nline 3";
        HWND h = dd::CreateForTest(6, owner, nullptr, nullptr, nullptr, &d.v1, &d.v2);
        wchar_t buf[256];
        GetDlgItemTextW(h, 601, buf, 256);
        EXPECT(wcscmp(buf, L"line 1\r\nline 2") == 0);
        Pump(1300);
        GetDlgItemTextW(h, 601, buf, 256);
        EXPECT(wcscmp(buf, L"line 1\r\nline 2\r\nline 3") == 0);
        EXPECT(dd::OkForTest(h, &err));
        DestroyWindow(h);
    }
    CheckNotForeground();
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    dd::g_testNoFocus = true;
    std::wstring outDir = argc > 1 ? argv[1] : L".";
    int offX = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200;
    // Hidden owner: owned dialogs get no taskbar button. Never shown.
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP, offX, 0, 10, 10,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    UnitTests(owner);
    Render(owner, outDir, offX);
    DestroyWindow(owner);
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
