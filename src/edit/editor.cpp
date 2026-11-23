// Edit mode core: windows and layout, media loading, the 30 Hz tick, jobs and status. Actions, segments and the
// export/tools commands are in editactions.cpp.
#include "editor.h"
#include "../app.h"
#include "../controls.h"
#include "eddialogs.h"
#include "exporter.h"
#include "ffrun.h"
#include "media.h"
#include "segio.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <cmath>

namespace edit {

namespace {
const UINT_PTR kTick = 20;
const wchar_t kNoTime[] = L"--:--:--.---";

std::wstring FormatLeft(double s) {
    wchar_t b[48];
    if (s < 60) swprintf(b, 48, L"%.0fs", std::ceil(s));
    else if (s < 3600) swprintf(b, 48, L"%dm %02ds", (int)s / 60, (int)s % 60);
    else swprintf(b, 48, L"%dh %02dm", (int)s / 3600, (int)s / 60 % 60);
    return b;
}

std::wstring FormatBytes(double b) {
    wchar_t s[32];
    if (b >= 1e9) swprintf(s, 32, L"%.1f GB", b / 1e9);
    else if (b >= 1e6) swprintf(s, 32, L"%.0f MB", b / 1e6);
    else swprintf(s, 32, L"%.0f KB", b / 1e3);
    return s;
}

bool FileExists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Enter applies, Escape reverts; both hand the keyboard back to the main window.
LRESULT CALLBACK BoxProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    auto* fn = (std::function<void(HWND, int)>*)ref;
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
        (*fn)(h, w == VK_RETURN ? 1 : 0);
        return 0;
    }
    if (m == WM_CHAR && (w == L'\r' || w == 27)) return 0;  // no beep
    if (m == WM_KILLFOCUS) (*fn)(h, 2);
    if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, BoxProc, id);
        delete fn;
    }
    return DefSubclassProc(h, m, w, l);
}
}  // namespace

struct Editor::LoadResult {
    std::shared_ptr<MediaInfo> media;
    std::wstring error;
};

struct Editor::JobOutcome {
    bool ok = false, cancelled = false;
    std::wstring msg, output, error;
};

Editor::Editor(App* app) : app_(app) {
    dpi_ = app->dpi_;
    s_.Load(app_->ini_);
    RegisterActions();
    LoadKeymap();
}

Editor::~Editor() { Shutdown(); }

// ------------------------------------------------------------------------------------------------ windows

void Editor::CreateWindows() {
    created_ = true;
    HINSTANCE inst = app_->inst_;
    HWND main = app_->hwnd_;
    INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_LISTVIEW_CLASSES | ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    panelBrush_ = CreateSolidBrush(theme::Panel);

    menuStrip_.Create(inst, main, theme::Bg);
    controlRow_.Create(inst, main, theme::Bg);
    exportBar_.Create(inst, main, theme::Panel);

    TimelineCallbacks cb;
    cb.seek = [this](double t) { Seek(t); };
    cb.hover = [this](double t, POINT screen) {
        if (!media_ || !media_->Video()) return;
        app_->thumbWnd_->Show(app_->thumbs_ ? app_->thumbs_->GetNear(t, std::max(2.0, tl_.ViewLength() / 60)) : nullptr, t,
                              screen, dpi_);
        if (app_->thumbs_) app_->thumbs_->Request(t);
    };
    cb.hoverEnd = [this] { app_->thumbWnd_->Hide(); };
    cb.selectSegment = [this](int i, uint32_t mods) { SelectIndex(i, mods, false); };
    cb.editBegin = [this] { PushUndo(); };
    cb.editEnd = [this] { SegmentsChanged(); };
    cb.newSegment = [this](double a, double b) { return NewSeg(a, b); };
    cb.timeText = [this](double t) { return FmtT(t); };
    tl_.Create(inst, main, cb);
    tl_.segments = &segs_;
    tl_.marked = &marked_;
    tl_.thumbs = this;

    BuildMenuStrip();
    BuildControlRow();
    BuildExportBar();
    BuildSidePanel();
    for (HWND h : {menuStrip_.Hwnd(), controlRow_.Hwnd(), exportBar_.Hwnd(), tl_.Hwnd(), side_}) DragAcceptFiles(h, TRUE);
    OnDpi(app_->dpi_);
}

void Editor::BuildMenuStrip() {
    auto& p = menuStrip_;
    for (const wchar_t* t : {L"File", L"Edit", L"Playback", L"View", L"Tools"}) {
        PanelItem it;
        it.text = t;
        std::wstring title = t;
        size_t idx = p.items.size();
        it.click = [this, title, idx] {
            RECT r = menuStrip_.ItemScreenRect(menuStrip_.items[idx]);
            ShowMenu(title, {r.left, r.bottom});
        };
        p.Add(it);
    }
    PanelItem back;
    back.glyph = glyph::Video;
    back.text = L"View mode";
    back.tip = L"Back to plain playback (E)";
    back.click = [this] { app_->DoAction(Act::EditMode); };
    p.Add(back);
    PanelItem title;
    title.kind = PanelItem::Label;
    title.alignRight = true;
    p.Add(title);
    p.layout = [](Panel& p, const RECT& rc) {
        int x = p.S(6);
        for (size_t i = 0; i < 5; i++) {
            int w = p.ButtonWidth(p.items[i]) - p.S(4);
            p.items[i].rc = {x, p.S(2), x + w, rc.bottom - p.S(2)};
            x += w;
        }
        int bw = p.ButtonWidth(p.items[5]);
        p.items[5].rc = {rc.right - bw - p.S(8), p.S(2), rc.right - p.S(8), rc.bottom - p.S(2)};
        p.items[6].rc = {x + p.S(20), 0, p.items[5].rc.left - p.S(16), rc.bottom};
    };
}

void Editor::BuildControlRow() {
    auto& p = controlRow_;
    auto btn = [&](const wchar_t* g, const wchar_t* text, const wchar_t* action, const wchar_t* tip) {
        PanelItem it;
        it.glyph = g ? g : L"";
        it.text = text ? text : L"";
        std::wstring id = action;
        it.tip = std::wstring(tip) + (KeyText(id).empty() ? L"" : L" (" + KeyText(id) + L")");
        it.click = [this, id] {
            Run(id);
            SetFocus(app_->hwnd_);
        };
        return p.Add(it);
    };
    // 0..3 left group
    PanelItem setStart;
    setStart.text = L"[";
    setStart.bold = true;
    setStart.fill = theme::Card;
    setStart.tip = L"Set cut start at playhead (" + KeyText(L"setstart") + L")";
    setStart.click = [this] { Run(L"setstart"); SetFocus(app_->hwnd_); };
    p.Add(setStart);
    btn(glyph::Cut, nullptr, L"split", L"Split segment at playhead");
    btn(glyph::Camera, nullptr, L"snapshot", L"Snapshot");
    // 3..10 transport
    btn(glyph::ToStart, nullptr, L"home", L"Go to start");
    btn(glyph::Prev, L"K", L"prevkey", L"Previous keyframe");
    btn(glyph::Prev, nullptr, L"prevframe", L"Previous frame");
    PanelItem play;
    play.glyph = glyph::Play;
    play.round = true;
    play.largeIcon = true;
    play.fill = theme::Accent;
    play.tip = L"Play / pause (" + KeyText(L"play") + L")";
    play.click = [this] { Run(L"play"); SetFocus(app_->hwnd_); };
    p.Add(play);
    btn(glyph::Next, nullptr, L"nextframe", L"Next frame");
    btn(glyph::Next, L"K", L"nextkey", L"Next keyframe");
    btn(glyph::ToEnd, nullptr, L"end", L"Go to end");
    PanelItem dur;  // 10: "/ 1:27:01.716"
    dur.kind = PanelItem::Label;
    dur.mono = true;
    p.Add(dur);
    // 11..14 right group
    btn(glyph::Volume, nullptr, L"mute", L"Mute");
    PanelItem vol;
    vol.kind = PanelItem::Slider;
    vol.max = 100;
    vol.tip = L"Volume";
    vol.changed = [this](double v) { app_->SetVolume((int)std::lround(v)); };
    p.Add(vol);
    PanelItem speed;  // 13
    speed.kind = PanelItem::Label;
    p.Add(speed);
    PanelItem setEnd;  // 14
    setEnd.text = L"]";
    setEnd.bold = true;
    setEnd.fill = theme::Card;
    setEnd.tip = L"Set cut end at playhead (" + KeyText(L"setend") + L")";
    setEnd.click = [this] { Run(L"setend"); SetFocus(app_->hwnd_); };
    p.Add(setEnd);

    auto makeBox = [&](bool large) {
        HWND h = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_CENTER | ES_AUTOHSCROLL, 0, 0, 10, 10, p.Hwnd(),
                                 nullptr, app_->inst_, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)theme::MonoFont(dpi_, large), TRUE);
        return h;
    };
    cutStartBox_ = makeBox(false);
    cutEndBox_ = makeBox(false);
    timeBox_ = makeBox(true);
    auto hook = [&](HWND box, std::function<void(HWND, int)> fn) {
        SetWindowSubclass(box, BoxProc, 1, (DWORD_PTR) new std::function<void(HWND, int)>(std::move(fn)));
    };
    hook(timeBox_, [this](HWND h, int how) {
        if (how == 1) {
            wchar_t b[64];
            GetWindowTextW(h, b, 64);
            double t;
            if (ParseT(b, &t)) Seek(t);
        }
        if (how != 2) SetFocus(app_->hwnd_);
        else SetWindowTextW(h, FmtT(tl_.position).c_str());
    });
    for (bool isStart : {true, false}) {
        hook(isStart ? cutStartBox_ : cutEndBox_, [this, isStart](HWND h, int how) {
            if (how == 1) {
                wchar_t b[64];
                GetWindowTextW(h, b, 64);
                SetCutTime(isStart, b);
            }
            if (how != 2) SetFocus(app_->hwnd_);
            else UpdateCutBoxes();
        });
    }

    p.layout = [this](Panel& p, const RECT& rc) {
        int h = rc.bottom, cy = h / 2, bh = p.S(32);
        auto place = [&](int i, int x, int w, int height = 0) {
            int hh = height ? height : bh;
            p.items[i].rc = {x, cy - hh / 2, x + w, cy - hh / 2 + hh};
            return x + w;
        };
        // Left: [ start-box split snapshot
        int x = p.S(8);
        x = place(0, x, p.S(34)) + p.S(4);
        MoveWindow(cutStartBox_, x, cy - p.S(11), p.S(104), p.S(22), TRUE);
        x += p.S(108);
        x = place(1, x, p.S(32)) + p.S(2);
        int leftEnd = place(2, x, p.S(32));
        // Right: mute volume speed end-box ]
        int r = rc.right - p.S(8);
        int rx = r - p.S(34);
        place(14, rx, p.S(34));
        rx -= p.S(108);
        MoveWindow(cutEndBox_, rx, cy - p.S(11), p.S(104), p.S(22), TRUE);
        int sw = p.items[13].text.empty() ? 0 : p.S(44);
        rx -= sw + p.S(6);
        place(13, rx, sw);
        rx -= p.S(90);
        place(12, rx, p.S(86), p.S(20));
        rx -= p.S(32);
        int rightStart = place(11, rx, p.S(30)) - p.S(30);
        // Center pill: transport, time box, duration
        int durW = (int)p.items[10].text.size() * p.S(8) + p.S(16);  // Consolas: fixed width
        int pillW = p.S(28) * 6 + p.S(40) + p.S(8) * 8 + p.S(126) + durW + p.S(20);
        int px = std::max(leftEnd + p.S(8), std::min((int)(rc.right - pillW) / 2, rightStart - pillW - p.S(8)));
        int cx = px + p.S(12);
        for (int i : {3, 4, 5}) cx = place(i, cx, p.S(i == 4 ? 40 : 30)) + p.S(2);
        cx = place(6, cx + p.S(4), p.S(40), p.S(40)) + p.S(6);
        for (int i : {7, 8, 9}) cx = place(i, cx, p.S(i == 8 ? 40 : 30)) + p.S(2);
        cx += p.S(10);
        MoveWindow(timeBox_, cx, cy - p.S(12), p.S(124), p.S(24), TRUE);
        cx += p.S(128);
        place(10, cx, durW);
    };
    p.paintExtra = [](Panel& p, HDC dc, const RECT&) {
        // The transport sits in a rounded pill (FastCut's RoundPanel).
        RECT a = p.items[3].rc, b = p.items[10].rc;
        RECT pill = {a.left - p.S(12), p.S(4), b.right + p.S(8), p.S(52) - p.S(4)};
        SetDCBrushColor(dc, theme::Panel);
        SetDCPenColor(dc, theme::Panel);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
        RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, p.S(44), p.S(44));
        SelectObject(dc, ob);
        SelectObject(dc, op);
    };
}

void Editor::BuildExportBar() {
    auto& p = exportBar_;
    PanelItem tracks;  // 0
    tracks.glyph = glyph::List;
    tracks.text = L"Tracks";
    tracks.tip = L"Choose which tracks to export";
    tracks.click = [this] { ShowTab(1); };
    p.Add(tracks);
    PanelItem chip;  // 1
    chip.kind = PanelItem::Label;
    p.Add(chip);
    PanelItem status;  // 2: icon + text; click shows the output in Explorer
    status.kind = PanelItem::Label;
    status.click = [this] {
        if (FileExists(lastOutput_))
            ShellExecuteW(app_->hwnd_, nullptr, L"explorer.exe", (L"/select,\"" + lastOutput_ + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    };
    p.Add(status);
    PanelItem prog;  // 3
    prog.kind = PanelItem::Progress;
    prog.visible = false;
    p.Add(prog);
    PanelItem progText;  // 4
    progText.kind = PanelItem::Label;
    progText.visible = false;
    p.Add(progText);
    PanelItem cancel;  // 5
    cancel.text = L"Cancel";
    cancel.fill = theme::Card;
    cancel.visible = false;
    cancel.click = [this] {
        if (jobCancel_) jobCancel_->flag = true;
    };
    p.Add(cancel);
    PanelItem show;  // 6
    show.glyph = glyph::Folder;
    show.text = L"Show in folder";
    show.tip = L"Show the exported file in Explorer";
    show.visible = false;
    show.click = p.items[2].click;
    p.Add(show);
    PanelItem discord;  // 7
    discord.text = L"Discord";
    discord.tip = L"Discord-ready: H.264 stays lossless; HEVC/AV1/10-bit video is re-encoded to H.264 on the GPU so it plays "
                  L"in Discord (click to turn off)";
    discord.click = [this] {
        s_.discordReady = !s_.discordReady;
        UpdateChecks();
        SaveSettings();
    };
    p.Add(discord);
    PanelItem size;  // 8
    size.dropdown = true;
    size.tip = L"Upload size limit: bigger exports are re-encoded to fit";
    size.click = [this] {
        HMENU m = CreatePopupMenu();
        const std::pair<int, const wchar_t*> opts[] = {{0, L"Any size"}, {10, L"10 MB (free)"}, {50, L"50 MB (Nitro Basic)"},
                                                       {500, L"500 MB (Nitro)"}};
        bool custom = true;
        for (auto& o : opts) {
            AppendMenuW(m, MF_STRING | (s_.discordSizeMB == o.first ? MF_CHECKED : 0), 100 + o.first, o.second);
            if (s_.discordSizeMB == o.first) custom = false;
        }
        AppendMenuW(m, MF_STRING | (custom ? MF_CHECKED : 0), 99, L"Custom...");
        RECT r = exportBar_.ItemScreenRect(exportBar_.items[8]);
        UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_BOTTOMALIGN, r.left, r.top, app_->hwnd_, nullptr);
        DestroyMenu(m);
        if (id >= 100) s_.discordSizeMB = id - 100;
        else if (id == 99) {
            std::wstring v = std::to_wstring(s_.discordSizeMB > 0 ? s_.discordSizeMB : 25);
            if (dialogs::Prompt(app_->hwnd_, L"Upload size limit", L"Megabytes", &v)) {
                int mb = _wtoi(v.c_str());
                if (mb >= 0) s_.discordSizeMB = mb;
            }
        }
        UpdateChecks();
        SaveSettings();
    };
    p.Add(size);
    PanelItem format;  // 9
    format.dropdown = true;
    format.tip = L"Output format (click to change)";
    format.click = [this] {
        HMENU m = CreatePopupMenu();
        for (int i = 0; i < dialogs::kFormatCount; i++) {
            std::wstring f = dialogs::kFormats[i];
            std::wstring label = f == L"same" ? L"Same (" + (file_.empty() ? std::wstring(L"input") : Ext(file_).substr(Ext(file_).empty() ? 0 : 1)) + L")"
                                              : util::Lower(f);
            for (auto& c : label) if (f != L"same") c = (wchar_t)towupper(c);
            AppendMenuW(m, MF_STRING | (s_.format == f ? MF_CHECKED : 0), 100 + i, label.c_str());
        }
        RECT r = exportBar_.ItemScreenRect(exportBar_.items[9]);
        UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_BOTTOMALIGN, r.left, r.top, app_->hwnd_, nullptr);
        DestroyMenu(m);
        if (id >= 100) s_.format = dialogs::kFormats[id - 100];
        UpdateChecks();
        SaveSettings();
    };
    p.Add(format);
    PanelItem cutMode;  // 10
    cutMode.click = [this] {
        s_.cutMode = s_.cutMode == L"keyframe" ? L"exact" : s_.cutMode == L"exact" ? L"smart" : L"keyframe";
        UpdateChecks();
        SaveSettings();
    };
    p.Add(cutMode);
    PanelItem merge;  // 11
    merge.tip = L"Export each segment to its own file, or merge them (click to change)";
    merge.click = [this] {
        s_.merge = !s_.merge;
        UpdateChecks();
        SaveSettings();
    };
    p.Add(merge);
    PanelItem exp;  // 12
    exp.glyph = glyph::Cut;
    exp.text = L"Export";
    exp.bold = true;
    exp.fill = theme::Export;
    exp.tip = L"Export the included segments (" + KeyText(L"export") + L")";
    exp.click = [this] { Run(L"export"); };
    p.Add(exp);

    p.layout = [](Panel& p, const RECT& rc) {
        int cy = rc.bottom / 2, bh = p.S(32);
        auto put = [&](int i, int x, int w, int h) {
            p.items[i].rc = {x, cy - h / 2, x + w, cy - h / 2 + h};
        };
        int x = p.S(10);
        int w0 = p.ButtonWidth(p.items[0]);
        put(0, x, w0, bh);
        x += w0 + p.S(8);
        int wc = p.items[1].text.empty() ? 0 : p.TextWidth(L"", p.items[1].text) + p.S(8);
        put(1, x, wc, bh);
        x += wc + (wc ? p.S(8) : 0);
        // Right side, from the edge inwards.
        int r = rc.right - p.S(10);
        for (int i : {12, 11, 10, 9, 8, 7, 6, 5}) {
            if (!p.items[i].visible) {
                p.items[i].rc = {};
                continue;
            }
            int w = p.ButtonWidth(p.items[i]) + (i == 12 ? p.S(8) : 0);
            put(i, r - w, w, i == 12 ? p.S(36) : bh);
            r -= w + p.S(6);
        }
        if (p.items[4].visible) {
            int w = p.TextWidth(L"", p.items[4].text) + p.S(8);
            put(4, r - w, w, bh);
            r -= w + p.S(4);
        }
        if (p.items[3].visible) {
            put(3, r - p.S(170), p.S(170), bh);
            r -= p.S(178);
        }
        put(2, x, std::max(0, r - x), bh);
    };
}

void Editor::BuildSidePanel() {
    HINSTANCE inst = app_->inst_;
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        auto* self = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            SetDCBrushColor(dc, theme::Panel);
            FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
            RECT grip = {0, 0, MulDiv(3, self->dpi_, 96), rc.bottom};
            SetDCBrushColor(dc, theme::Bg);
            FillRect(dc, &grip, (HBRUSH)GetStockObject(DC_BRUSH));
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, theme::UiFont(self->dpi_));
            SetTextColor(dc, theme::Faint);
            if (self->tab_ == 0 && self->segs_.empty()) {
                RECT t = rc;
                t.top += MulDiv(44, self->dpi_, 96);
                t.bottom -= MulDiv(44, self->dpi_, 96);
                DrawTextW(dc, L"No segments yet\n\nPress I and O to mark a start and end,\nor + to add a segment at the playhead.", -1,
                          &t, DT_CENTER | DT_NOPREFIX | DT_WORDBREAK | DT_VCENTER);
                // DT_VCENTER needs DT_SINGLELINE; nudge to the middle instead
            } else if (self->tab_ == 2) {
                RECT t = {MulDiv(14, self->dpi_, 96), MulDiv(48, self->dpi_, 96), rc.right, MulDiv(72, self->dpi_, 96)};
                DrawTextW(dc, L"Double-click to open. Merge joins the files in this order.", -1, &t,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE: {
            RECT rc;
            GetClientRect(h, &rc);
            self->LayoutSide(rc);
            return 0;
        }
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            if (p.x < MulDiv(5, self->dpi_, 96)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            if ((short)LOWORD(l) < MulDiv(5, self->dpi_, 96)) {
                self->splitterDrag_ = true;
                SetCapture(h);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (self->splitterDrag_) {
                POINT p;
                GetCursorPos(&p);
                RECT mr;
                GetClientRect(self->app_->hwnd_, &mr);
                ScreenToClient(self->app_->hwnd_, &p);
                int w = std::clamp((int)(mr.right - p.x), MulDiv(240, self->dpi_, 96), std::max(MulDiv(240, self->dpi_, 96), (int)mr.right - MulDiv(320, self->dpi_, 96)));
                self->s_.sidePanelWidth = MulDiv(w, 96, self->dpi_);
                self->app_->Layout();
            }
            return 0;
        case WM_LBUTTONUP:
            if (self->splitterDrag_) {
                self->splitterDrag_ = false;
                ReleaseCapture();
                self->SaveSettings();
            }
            return 0;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)w, theme::Text);
            SetBkColor((HDC)w, theme::Panel);
            return (LRESULT)self->panelBrush_;
        case WM_MEASUREITEM: {
            auto* mi = (MEASUREITEMSTRUCT*)l;
            mi->itemHeight = MulDiv(24, self->dpi_, 96);
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto* di = (DRAWITEMSTRUCT*)l;
            if (di->hwndItem != self->fileList_ || di->itemID == (UINT)-1) break;
            const auto& files = self->app_->plist_;
            std::wstring path = di->itemID < files.size() ? files[di->itemID] : self->app_->path_;
            bool current = util::Lower(path) == util::Lower(self->app_->path_);
            bool sel = (di->itemState & ODS_SELECTED) != 0;
            SetDCBrushColor(di->hDC, sel ? theme::CardSelected : theme::Panel);
            FillRect(di->hDC, &di->rcItem, (HBRUSH)GetStockObject(DC_BRUSH));
            SetBkMode(di->hDC, TRANSPARENT);
            SelectObject(di->hDC, theme::UiFont(self->dpi_, current));
            SetTextColor(di->hDC, current ? theme::Text : theme::Dim);
            RECT r = di->rcItem;
            r.left += MulDiv(8, self->dpi_, 96);
            std::wstring t = (current ? L"▶  " : L"     ") + util::FileName(path);
            DrawTextW(di->hDC, t.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            return TRUE;
        }
        case WM_COMMAND:
            if ((HWND)l == self->fileList_ && HIWORD(w) == LBN_DBLCLK) {
                int i = (int)SendMessageW(self->fileList_, LB_GETCURSEL, 0, 0);
                if (i >= 0 && i < (int)self->app_->plist_.size() && util::Lower(self->app_->plist_[i]) != util::Lower(self->app_->path_))
                    self->app_->PlaylistActivate(i);
            } else if ((HWND)l == self->rotationBox_ && HIWORD(w) == CBN_SELCHANGE) {
                SetFocus(self->app_->hwnd_);
            }
            return 0;
        case WM_NOTIFY: {
            auto* nm = (NMHDR*)l;
            if (nm->hwndFrom == self->trackList_) {
                if (nm->code == LVN_ITEMCHANGED) {
                    auto* lv = (NMLISTVIEW*)l;
                    if (!self->rebuildingTracks_ && (lv->uChanged & LVIF_STATE) &&
                        ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) && self->media_ &&
                        lv->iItem >= 0 && lv->iItem < (int)self->media_->streams.size()) {
                        self->media_->streams[lv->iItem].keep = ListView_GetCheckState(self->trackList_, lv->iItem) != 0;
                        self->UpdateSegHeader();
                    }
                } else if (nm->code == NM_DBLCLK) {
                    self->Run(L"streaminfo");
                } else if (nm->code == NM_CUSTOMDRAW) {
                    auto* cd = (NMLVCUSTOMDRAW*)l;
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        cd->clrText = theme::Text;
                        cd->clrTextBk = theme::Panel;
                        return CDRF_DODEFAULT;
                    }
                }
            }
            return 0;
        }
        case WM_DROPFILES:
            return SendMessageW(self->app_->hwnd_, m, w, l);
        }
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FastPlay.EditSide";
    RegisterClassExW(&wc);
    side_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, app_->hwnd_,
                            nullptr, inst, this);

    // Tab strip: Segments / Tracks / Files
    tabStrip_.Create(inst, side_, theme::Panel);
    for (int i = 0; i < 3; i++) {
        PanelItem it;
        it.text = i == 0 ? L"Segments" : i == 1 ? L"Tracks" : L"Files";
        it.click = [this, i] { ShowTab(i); };
        tabStrip_.Add(it);
    }
    tabStrip_.layout = [](Panel& p, const RECT& rc) {
        int x = p.S(6);
        for (auto& it : p.items) {
            int w = p.ButtonWidth(it);
            it.rc = {x, p.S(7), x + w, rc.bottom - p.S(5)};
            x += w + p.S(4);
        }
    };

    // Segments page
    SegmentCardsCallbacks cc;
    cc.itemClicked = [this](int i, uint32_t mods) { SelectIndex(i, mods, (mods & (MK_CONTROL | MK_SHIFT)) == 0); };
    cc.toggleRequested = [this](int i) {
        if (i < 0 || i >= (int)segs_.size()) return;
        PushUndo();
        segs_[i].enabled = !segs_[i].enabled;
        SegmentsChanged();
    };
    cc.editRequested = [this](int i) {
        SelectIndex(i, 0, false);
        EditSegment();
    };
    cc.reorder = [this](int a, int b) { ReorderSegment(a, b); };
    cc.contextMenu = [this](POINT p, int i) {
        if (i >= 0 && !marked_.count(i)) SelectIndex(i, 0, false);
        ShowMenu(L"Segment", p);
    };
    cards_.Create(inst, side_, cc);
    cards_.segments = &segs_;
    cards_.marked = &marked_;
    cards_.fmt = [this](double t) { return FmtT(t); };
    cards_.warning = [this](int i) { return CutWarning(i); };
    segSummary_.Create(inst, side_, theme::Panel);
    PanelItem sum;
    sum.kind = PanelItem::Label;
    segSummary_.Add(sum);
    segSummary_.layout = [](Panel& p, const RECT& rc) { p.items[0].rc = {p.S(14), 0, rc.right - p.S(8), rc.bottom}; };
    segBar_.Create(inst, side_, theme::Panel);
    const std::pair<const wchar_t*, const wchar_t*> segIcons[] = {
        {glyph::Plus, L"add"}, {glyph::Cut, L"split"}, {glyph::Trash, L"delete"}, {glyph::Up, L"segup"},
        {glyph::Down, L"segdown"}, {glyph::Tag, L"label"}, {glyph::Swap, L"invert"}, {glyph::Sort, L"sort"}};
    for (auto& [g, id] : segIcons) {
        PanelItem it;
        it.glyph = g;
        std::wstring action = id;
        auto ai = actionIndex_.find(action);
        it.tip = (ai != actionIndex_.end() ? actions_[ai->second].label : action) +
                 (KeyText(action).empty() ? L"" : L" (" + KeyText(action) + L")");
        it.click = [this, action] { Run(action); };
        segBar_.Add(it);
    }
    auto iconRow = [](Panel& p, const RECT& rc) {
        int x = p.S(6);
        for (auto& it : p.items) {
            if (!it.visible) continue;
            int w = it.kind == PanelItem::Button ? p.ButtonWidth(it) : 0;
            it.rc = {x, p.S(6), x + w, rc.bottom - p.S(6)};
            x += w + p.S(2);
        }
    };
    segBar_.layout = iconRow;

    // Tracks page
    trackList_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_SINGLESEL,
                                 0, 0, 10, 10, side_, nullptr, inst, nullptr);
    ListView_SetExtendedListViewStyle(trackList_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(trackList_, theme::Panel);
    ListView_SetTextBkColor(trackList_, theme::Panel);
    ListView_SetTextColor(trackList_, theme::Text);
    using SetTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static SetTheme setTheme = [] {
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return ux ? (SetTheme)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    }();
    if (setTheme) {
        setTheme(trackList_, L"DarkMode_Explorer", nullptr);
        setTheme(ListView_GetHeader(trackList_), L"DarkMode_ItemsView", nullptr);
    }
    const std::pair<const wchar_t*, int> cols[] = {{L"#", 40}, {L"Type", 64}, {L"Codec", 80}, {L"Lang", 44}, {L"Details", 240}};
    for (int i = 0; i < 5; i++) {
        LVCOLUMNW c = {LVCF_TEXT | LVCF_WIDTH};
        c.pszText = (LPWSTR)cols[i].first;
        c.cx = MulDiv(cols[i].second, dpi_, 96);
        ListView_InsertColumn(trackList_, i, &c);
    }
    trackBar_.Create(inst, side_, theme::Panel);
    rotationBox_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 200,
                                   trackBar_.Hwnd(), nullptr, inst, nullptr);
    if (setTheme) setTheme(rotationBox_, L"DarkMode_CFD", nullptr);
    for (const wchar_t* r : {L"Rotation: keep", L"Rotate 0°", L"Rotate 90°", L"Rotate 180°", L"Rotate 270°"})
        SendMessageW(rotationBox_, CB_ADDSTRING, 0, (LPARAM)r);
    SendMessageW(rotationBox_, CB_SETCURSEL, 0, 0);
    for (auto [text, id] : {std::pair<const wchar_t*, const wchar_t*>{L"All", L"tracksall"}, {L"None", L"tracksnone"},
                            {L"Details", L"streaminfo"}, {L"Extract all", L"extract"}}) {
        PanelItem it;
        it.text = text;
        it.fill = theme::Card;
        std::wstring action = id;
        it.click = [this, action] {
            if (action == L"tracksall") SetAllTracks(true);
            else if (action == L"tracksnone") SetAllTracks(false);
            else Run(action);
        };
        trackBar_.Add(it);
    }
    trackBar_.layout = [this](Panel& p, const RECT& rc) {
        int x = p.S(6);
        MoveWindow(rotationBox_, x, p.S(9), p.S(124), p.S(200), TRUE);
        x += p.S(130);
        for (auto& it : p.items) {
            int w = p.ButtonWidth(it);
            it.rc = {x, p.S(7), x + w, rc.bottom - p.S(7)};
            x += w + p.S(4);
        }
    };

    // Files page (FastPlay's playlist)
    fileList_ = CreateWindowExW(0, L"LISTBOX", L"",
                                WS_CHILD | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0,
                                0, 10, 10, side_, nullptr, inst, nullptr);
    if (setTheme) setTheme(fileList_, L"DarkMode_Explorer", nullptr);
    fileBar_.Create(inst, side_, theme::Panel);
    const std::tuple<const wchar_t*, const wchar_t*, int> fileBtns[] = {
        {glyph::Plus, L"Add", 0}, {glyph::Trash, L"", 1}, {glyph::Up, L"", 2}, {glyph::Down, L"", 3}, {nullptr, L"Merge files...", 4},
        {nullptr, L"Export all...", 5}};
    for (auto& [g, text, which] : fileBtns) {
        PanelItem it;
        it.glyph = g ? g : L"";
        it.text = text;
        it.fill = which >= 4 || which == 0 ? theme::Card : 0xFFFFFFFF;
        it.tip = which == 0 ? L"Add files" : which == 1 ? L"Remove from list" : which == 2 ? L"Move up" : which == 3 ? L"Move down" : L"";
        int w = which;
        it.click = [this, w] {
            auto& pl = app_->plist_;
            if (pl.empty() && !app_->path_.empty()) {
                pl = {app_->path_};
                app_->plPos_ = 0;
            }
            int i = (int)SendMessageW(fileList_, LB_GETCURSEL, 0, 0);
            switch (w) {
            case 0: Run(L"open"); break;
            case 1:
                if (i >= 0 && i < (int)pl.size()) {
                    bool cur = util::Lower(pl[i]) == util::Lower(app_->path_);
                    app_->PlaylistRemove(i);
                    if (cur) app_->CloseFile();
                }
                break;
            case 2:
            case 3: {
                int j = i + (w == 2 ? -1 : 1);
                if (i < 0 || j < 0 || j >= (int)pl.size()) break;
                std::swap(pl[i], pl[j]);
                if (app_->plPos_ == i) app_->plPos_ = j;
                else if (app_->plPos_ == j) app_->plPos_ = i;
                RefreshFileList();
                SendMessageW(fileList_, LB_SETCURSEL, j, 0);
                break;
            }
            case 4: Run(L"concat"); break;
            case 5: Run(L"batch"); break;
            }
            app_->RefreshPlaylistWindow();
        };
        fileBar_.Add(it);
    }
    fileBar_.layout = iconRow;
    ShowTab(0);
}

void Editor::ShowTab(int i) {
    tab_ = i;
    for (int j = 0; j < 3; j++) {
        tabStrip_.items[j].checked = j == i;
        tabStrip_.items[j].fill = j == i ? theme::Card : 0xFFFFFFFF;
    }
    tabStrip_.Invalidate();
    bool segsEmpty = segs_.empty();
    ShowWindow(cards_.Hwnd(), i == 0 && !segsEmpty ? SW_SHOW : SW_HIDE);
    ShowWindow(segSummary_.Hwnd(), i == 0 && !segsEmpty ? SW_SHOW : SW_HIDE);
    ShowWindow(segBar_.Hwnd(), i == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(trackList_, i == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(trackBar_.Hwnd(), i == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(fileList_, i == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(fileBar_.Hwnd(), i == 2 ? SW_SHOW : SW_HIDE);
    if (i == 2) RefreshFileList();
    InvalidateRect(side_, nullptr, TRUE);
}

void Editor::LayoutSide(const RECT& rc) {
    int g = MulDiv(4, dpi_, 96), tabH = MulDiv(44, dpi_, 96), barH = MulDiv(44, dpi_, 96), sumH = MulDiv(30, dpi_, 96);
    int w = rc.right - g;
    MoveWindow(tabStrip_.Hwnd(), g, 0, w, tabH, TRUE);
    ShowWindow(tabStrip_.Hwnd(), SW_SHOW);
    MoveWindow(segBar_.Hwnd(), g, rc.bottom - barH, w, barH, TRUE);
    MoveWindow(segSummary_.Hwnd(), g, rc.bottom - barH - sumH, w, sumH, TRUE);
    MoveWindow(cards_.Hwnd(), g, tabH, w, std::max(0, (int)rc.bottom - tabH - barH - sumH), TRUE);
    MoveWindow(trackBar_.Hwnd(), g, rc.bottom - barH, w, barH, TRUE);
    MoveWindow(trackList_, g, tabH, w, std::max(0, (int)rc.bottom - tabH - barH), TRUE);
    int hint = MulDiv(32, dpi_, 96);
    MoveWindow(fileBar_.Hwnd(), g, rc.bottom - barH, w, barH, TRUE);
    MoveWindow(fileList_, g, tabH + hint, w, std::max(0, (int)rc.bottom - tabH - hint - barH), TRUE);
}

void Editor::Layout(const RECT& rc) {
    if (!created_) return;
    HWND windows[] = {menuStrip_.Hwnd(), controlRow_.Hwnd(), exportBar_.Hwnd(), tl_.Hwnd(), side_};
    if (!active_ || app_->fullscreen_) {
        for (HWND h : windows) ShowWindow(h, SW_HIDE);
        if (active_) MoveWindow(app_->video_, 0, 0, rc.right, rc.bottom, TRUE);
        return;
    }
    int menuH = MulDiv(30, dpi_, 96), barH = MulDiv(52, dpi_, 96), rowH = MulDiv(56, dpi_, 96);
    int tlH = tl_.PreferredHeight();
    int bottom = barH + rowH + tlH;
    int sideW = sidebar_ ? std::clamp(MulDiv(s_.sidePanelWidth, dpi_, 96), MulDiv(240, dpi_, 96),
                                      std::max(MulDiv(240, dpi_, 96), (int)rc.right - MulDiv(320, dpi_, 96)))
                         : 0;
    int midH = std::max(0, (int)rc.bottom - menuH - bottom);
    MoveWindow(menuStrip_.Hwnd(), 0, 0, rc.right, menuH, TRUE);
    MoveWindow(app_->video_, 0, menuH, rc.right - sideW, midH, TRUE);
    MoveWindow(side_, rc.right - sideW, menuH, sideW, midH, TRUE);
    MoveWindow(tl_.Hwnd(), 0, menuH + midH, rc.right, tlH, TRUE);
    MoveWindow(controlRow_.Hwnd(), 0, menuH + midH + tlH, rc.right, rowH, TRUE);
    MoveWindow(exportBar_.Hwnd(), 0, rc.bottom - barH, rc.right, barH, TRUE);
    for (HWND h : windows) ShowWindow(h, SW_SHOWNA);
    if (!sidebar_) ShowWindow(side_, SW_HIDE);
}

void Editor::OnDpi(int dpi) {
    dpi_ = dpi;
    if (!created_) return;
    for (Panel* p : {&menuStrip_, &controlRow_, &exportBar_, &tabStrip_, &segBar_, &trackBar_, &fileBar_, &segSummary_}) p->SetDpi(dpi);
    tl_.SetDpi(dpi);
    cards_.SetDpi(dpi);
    for (HWND b : {cutStartBox_, cutEndBox_}) SendMessageW(b, WM_SETFONT, (WPARAM)theme::MonoFont(dpi, false), TRUE);
    SendMessageW(timeBox_, WM_SETFONT, (WPARAM)theme::MonoFont(dpi, true), TRUE);
    SendMessageW(fileList_, LB_SETITEMHEIGHT, 0, MulDiv(24, dpi, 96));
    SendMessageW(trackList_, WM_SETFONT, (WPARAM)theme::UiFont(dpi), TRUE);
    SendMessageW(fileList_, WM_SETFONT, (WPARAM)theme::UiFont(dpi), TRUE);
    SendMessageW(rotationBox_, WM_SETFONT, (WPARAM)theme::UiFont(dpi), TRUE);
    if (active_) app_->Layout();
}

// ------------------------------------------------------------------------------------------------ enter / leave

void Editor::Enter() {
    if (active_) return;
    if (!created_) CreateWindows();
    s_.Load(app_->ini_);
    LoadKeymap();
    active_ = true;
    EnableDarkMenus();
    DarkTitleBar(app_->hwnd_, true);
    // FastCut's 1360x860 working size when the window is smaller; View mode's placement comes back on leave.
    restoreViewPlacement_ = false;
    if (!app_->fullscreen_ && !IsZoomed(app_->hwnd_)) {
        viewPlacement_.length = sizeof viewPlacement_;
        GetWindowPlacement(app_->hwnd_, &viewPlacement_);
        RECT wr;
        GetWindowRect(app_->hwnd_, &wr);
        HMONITOR mon = MonitorFromWindow(app_->hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(mon, &mi);
        int w = std::min(MulDiv(1360, dpi_, 96), (int)(mi.rcWork.right - mi.rcWork.left));
        int h = std::min(MulDiv(860, dpi_, 96), (int)(mi.rcWork.bottom - mi.rcWork.top));
        if (wr.right - wr.left < w || wr.bottom - wr.top < h) {
            w = std::max(w, (int)(wr.right - wr.left));
            h = std::max(h, (int)(wr.bottom - wr.top));
            int x = std::clamp((int)(wr.left + wr.right) / 2 - w / 2, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
            int y = std::clamp((int)(wr.top + wr.bottom) / 2 - h / 2, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - h));
            if (util::TestMode()) {
                x = wr.left;
                y = wr.top;
            }
            SetWindowPos(app_->hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            restoreViewPlacement_ = true;
        }
    }
    SetTimer(app_->hwnd_, kTick, 33, nullptr);
    if (!app_->path_.empty() && (file_ != app_->path_ || !media_)) OnFileOpened(app_->path_);
    else UpdateUiForMedia();
    if (app_->mpv_ && !app_->path_.empty()) mpvx::SetFlag(app_->mpv_, "pause", true);
    app_->Layout();
    app_->UpdateTitle();
    UpdateChecks();
    SetFocus(app_->hwnd_);
}

void Editor::Leave() {
    if (!active_) return;
    SaveAutosave();
    SaveSettings();
    active_ = false;
    KillTimer(app_->hwnd_, kTick);
    app_->thumbWnd_->Hide();
    // The timeline filled the thumbnail cache and its decoder; View mode restarts it on the next seekbar hover.
    app_->RetireThumbnails();
    if (restoreViewPlacement_ && !app_->fullscreen_) SetWindowPlacement(app_->hwnd_, &viewPlacement_);
    restoreViewPlacement_ = false;
    DarkTitleBar(app_->hwnd_, false);
    app_->Layout();
    app_->UpdateTitle();
}

void Editor::SaveSettings() {
    if (util::TestMode()) return;
    app_->ini_.Load(app_->iniPath_);  // keep edits made by hand while running
    s_.Save(app_->ini_);
    for (const auto& a : actions_)
        if (!app_->ini_.Has(L"edit_keys", a.id)) app_->ini_.Set(L"edit_keys", a.id, a.defaults);
    app_->ini_.Save(app_->iniPath_);
}

// ------------------------------------------------------------------------------------------------ media loading

void Editor::OnFileOpened(const std::wstring& path) {
    if (!file_.empty() && util::Lower(file_) != util::Lower(path)) SaveAutosave();
    file_ = path;
    media_.reset();
    kf_ = KeyframeIndex{};
    segs_.clear();
    undo_.clear();
    redo_.clear();
    marked_.clear();
    looping_ = false;
    playUntil_ = -1;
    tl_.keyframes.clear();
    tl_.wave = nullptr;
    tl_.waveCount = 0;
    keyframeProgress_ = -1;
    waveProgress_ = -1;
    if (!created_) return;
    if (active_ && app_->mpv_) mpvx::SetFlag(app_->mpv_, "pause", true);
    SetStatus(L"Opening...");
    UpdateUiForMedia();
    StartLoad(path);
    RefreshFileList();
}

void Editor::OnFileClosed() {
    SaveAutosave();
    if (loadCancel_) loadCancel_->flag = true;
    file_.clear();
    media_.reset();
    segs_.clear();
    kf_ = KeyframeIndex{};
    tl_.keyframes.clear();
    tl_.wave = nullptr;
    tl_.waveCount = 0;
    if (!created_) return;
    UpdateUiForMedia();
    SetStatus(L"");
}

void Editor::StartLoad(const std::wstring& path) {
    if (loadCancel_) loadCancel_->flag = true;
    if (loadThread_.joinable()) loadThread_.join();
    if (waveThread_.joinable()) waveThread_.join();
    waveLoading_ = false;
    loadCancel_ = std::make_shared<Cancel>();
    unsigned serial = ++loadSerial_;
    auto cancel = loadCancel_;
    HWND hwnd = app_->hwnd_;
    keyframeProgress_ = -1;
    loadThread_ = std::thread([this, path, serial, cancel, hwnd] {
        auto* r = new LoadResult;
        try {
            r->media = std::make_shared<MediaInfo>(Probe(path, cancel.get()));
        } catch (const EditError& e) {
            r->error = e.msg;
        } catch (const Cancelled&) {
            delete r;
            return;
        } catch (const std::exception& e) {
            r->error = util::Wide(e.what());
        }
        auto media = r->media;
        if (!PostMessageW(hwnd, WM_APP_EDIT_LOADED, serial, (LPARAM)r)) delete r;
        if (!media || !media->Video()) return;
        // Keyframes: milliseconds from an MP4 header, else an ffprobe packet scan (shown in the timeline).
        keyframeProgress_ = 0;
        auto* k = new KeyframeIndex;
        try {
            *k = Keyframes(*media, media->Video()->index, cancel.get(), [this](double p) { keyframeProgress_ = p; });
        } catch (const Cancelled&) {
            delete k;
            return;
        } catch (const EditError& e) {
            ff::AddLog(L"Keyframe scan failed: " + e.msg);
        } catch (...) {
        }
        keyframeProgress_ = -1;
        if (!PostMessageW(hwnd, WM_APP_EDIT_KEYFRAMES, serial, (LPARAM)k)) delete k;
    });
}

void Editor::OnLoaded(LoadResult* raw, unsigned serial) {
    std::unique_ptr<LoadResult> r(raw);
    if (serial != loadSerial_) return;
    if (!r->media) {
        SetStatus(L"Could not read file: " + r->error);
        UpdateUiForMedia();
        return;
    }
    media_ = r->media;
    util::TimingLog("edit: media probed");
    segs_ = LoadAutosave(file_);
    nextColor_ = (int)segs_.size();
    if (!pendingProject_.empty()) {
        std::wstring llc;
        llc.swap(pendingProject_);
        try {
            std::ifstream f(llc, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            segs_ = Colorize(segio::ReadProject(text, media_->duration));
        } catch (...) {
        }
    }
    UpdateUiForMedia();
    std::wstring fmt = util::Wide(media_->formatName.substr(0, media_->formatName.find(',')));
    std::wstring msg = util::FileName(file_) + L" — " + fmt + L", " + Fmt(media_->duration) + L", " +
                       std::to_wstring(media_->streams.size()) + L" streams";
    if (!segs_.empty()) msg += L" — restored " + std::to_wstring(segs_.size()) + L" segments";
    SetStatus(msg);
    if (const StreamInfo* v = media_->Video()) {
        bool sideways = v->rotation == 90 || v->rotation == 270;
        tl_.thumbAspect = v->width > 0 && v->height > 0 ? (sideways ? (double)v->height / v->width : (double)v->width / v->height) : 16.0 / 9;
        app_->StartThumbnails();
    }
    LoadWaveIfNeeded(false);
}

void Editor::LoadWaveIfNeeded(bool force) {
    const StreamInfo* a = media_ ? media_->Audio() : nullptr;
    if (!s_.showWave || !a || tl_.wave || waveLoading_) return;
    // The waveform decodes the whole audio track, which means reading the whole file. Automatic for files on SSDs or
    // under 2 GB; for bigger files on a hard disk only when asked for (View > Show waveform), so opening stays instant.
    uint64_t size = 0, mtime = 0;
    ResumeStore::Stat(file_, &size, &mtime);
    if (!force && size > (2ull << 30) && util::IsOnSpinningDisk(file_)) {
        tl_.waveProgress = -1;
        SetStatus(exportBar_.items[2].text + L"  ·  waveform not loaded for this big file on a hard disk (View > Show waveform)");
        return;
    }
    if (waveThread_.joinable()) waveThread_.join();
    size_t count = (size_t)(media_->duration * kWaveRate) + 1;
    wave_.assign(count, 0.0f);
    waveFilled_ = 0;
    tl_.wave = wave_.data();
    tl_.waveCount = count;
    waveLoading_ = true;
    waveProgress_ = 0;
    auto m = media_;
    auto cancel = loadCancel_;
    int stream = a->index;
    float* data = wave_.data();
    waveThread_ = std::thread([this, m, cancel, stream, data, count] {
        try {
            Waveform(*m, stream, data, count, &waveFilled_, cancel.get());
        } catch (const EditError& e) {
            ff::AddLog(L"Waveform failed: " + e.msg);
        } catch (...) {
        }
        waveProgress_ = -1;
    });
}

void Editor::UpdateUiForMedia() {
    bool has = media_ != nullptr;
    tl_.duration = has ? media_->duration : 1;
    tl_.hasVideo = has && media_->Video();
    tl_.hasMedia = has;
    tl_.selected = segs_.empty() ? -1 : 0;
    tl_.position = 0;
    lastPos_ = -1;
    ApplyTimelineHeight();
    tl_.ResetView();
    controlRow_.items[10].text = has ? L"/ " + Fmt(media_->duration) : L"";
    SetWindowTextW(timeBox_, FmtT(0).c_str());
    SendMessageW(rotationBox_, CB_SETCURSEL, 0, 0);
    app_->thumbWnd_->Hide();
    RebuildTrackList();
    RebuildSegList();
    UpdateChecks();
    controlRow_.Relayout();
}

void Editor::ApplyTimelineHeight() {
    tl_.showThumbs = s_.showThumbs;
    tl_.showWave = s_.showWave;
    tl_.showKeyframes = s_.showKeyframes;
    tl_.Invalidate();
    if (active_) app_->Layout();
}

void Editor::RebuildTrackList() {
    if (!trackList_) return;
    rebuildingTracks_ = true;
    SendMessageW(trackList_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(trackList_);
    if (media_) {
        for (size_t i = 0; i < media_->streams.size(); i++) {
            const auto& s = media_->streams[i];
            std::wstring cells[] = {std::to_wstring(s.index), util::Wide(s.type), util::Wide(s.codec), s.language, s.Details()};
            LVITEMW it = {LVIF_TEXT};
            it.iItem = (int)i;
            it.pszText = cells[0].data();
            ListView_InsertItem(trackList_, &it);
            for (int c = 1; c < 5; c++) ListView_SetItemText(trackList_, (int)i, c, cells[c].data());
        }
        for (size_t i = 0; i < media_->streams.size(); i++) ListView_SetCheckState(trackList_, (int)i, media_->streams[i].keep);
    }
    rebuildingTracks_ = false;
    SendMessageW(trackList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(trackList_, nullptr, TRUE);
}

void Editor::RefreshFileList() {
    if (!fileList_) return;
    SendMessageW(fileList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(fileList_, LB_RESETCONTENT, 0, 0);
    const auto& pl = app_->plist_;
    size_t n = pl.empty() ? (app_->path_.empty() ? 0 : 1) : pl.size();
    for (size_t i = 0; i < n; i++) SendMessageW(fileList_, LB_ADDSTRING, 0, (LPARAM)L"");
    SendMessageW(fileList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(fileList_, nullptr, TRUE);
}

// ------------------------------------------------------------------------------------------------ tick, status

void Editor::OnTimer() {
    if (!active_) return;
    UpdateLoadIndicators();
    // Job progress: bar, text, Export button percentage and the taskbar.
    if (jobRunning_) {
        double frac;
        std::wstring msg;
        {
            std::lock_guard<std::mutex> l(progMu_);
            frac = progFrac_;
            msg = progMsg_;
        }
        auto& bar = exportBar_.items;
        bar[3].value = frac < 0 ? -1 : std::clamp(frac, 0.0, 1.0);
        double el = (GetTickCount() - jobStart_) / 1000.0;
        std::wstring text;
        if (frac < 0) text = Fmt(el, false);
        else {
            wchar_t pct[16];
            swprintf(pct, 16, L"%.0f%%", std::clamp(frac, 0.0, 1.0) * 100);
            double left = frac > 0.03 && el > 1 ? el * (1 - frac) / frac : -1;
            text = left >= 0 ? std::wstring(pct) + L" · " + FormatLeft(left) + L" left" : pct;
            if (jobOnExport_) bar[12].text = std::wstring(L"Exporting ") + pct;
        }
        bool relayout = bar[4].text.size() != text.size();
        bar[4].text = text;
        if (!msg.empty()) bar[2].text = msg;
        if (relayout) exportBar_.Relayout();
        else exportBar_.Invalidate();
        if (app_->taskbar_) {
            app_->taskbar_->SetProgressState(app_->hwnd_, frac < 0 ? TBPF_INDETERMINATE : TBPF_NORMAL);
            if (frac >= 0) app_->taskbar_->SetProgressValue(app_->hwnd_, (ULONGLONG)(frac * 1000), 1000);
        }
    }
    if (!media_ || !app_->mpv_) return;
    bool paused = app_->bar_.paused;
    if (paused != lastPaused_) {
        lastPaused_ = paused;
        controlRow_.items[6].glyph = paused ? glyph::Play : glyph::Pause;
        controlRow_.Invalidate();
    }
    double pos = GetTickCount() < holdPosUntil_ ? holdPos_ : mpvx::GetDouble(app_->mpv_, "time-pos", tl_.position);
    if (playUntil_ >= 0 && !paused && pos >= playUntil_) {
        mpvx::SetFlag(app_->mpv_, "pause", true);
        playUntil_ = -1;
    }
    if (std::abs(pos - lastPos_) < 0.0005) return;
    lastPos_ = pos;
    tl_.position = pos;
    if (!paused) tl_.Follow(pos);
    tl_.Invalidate();
    if (GetFocus() != timeBox_) SetWindowTextW(timeBox_, FmtT(pos).c_str());
}

void Editor::UpdateLoadIndicators() {
    double kp = keyframeProgress_, wp = waveProgress_;
    if (waveLoading_ && wp >= 0 && tl_.waveCount) wp = std::min(1.0, (double)waveFilled_ / tl_.waveCount);
    if (waveLoading_ && waveProgress_ < 0) waveLoading_ = false;  // thread finished
    size_t thumbsLeft = media_ && s_.showThumbs && tl_.hasVideo && app_->thumbs_ ? app_->thumbs_->Pending() : 0;
    if (std::abs(kp - tl_.keyframeProgress) >= 0.01 || (kp < 0) != (tl_.keyframeProgress < 0) ||
        std::abs(wp - tl_.waveProgress) >= 0.01 || (wp < 0) != (tl_.waveProgress < 0)) {
        tl_.keyframeProgress = kp;
        tl_.waveProgress = wp;
        if (wp >= 0 || tl_.wave) tl_.WaveUpdated();
        else tl_.Invalidate();
    } else if (waveLoading_) {
        tl_.WaveUpdated();
    }
    std::vector<std::wstring> parts;
    wchar_t b[64];
    if (kp >= 0) { swprintf(b, 64, L"keyframes %.0f%%", kp * 100); parts.push_back(b); }
    if (wp >= 0) { swprintf(b, 64, L"waveform %.0f%%", wp * 100); parts.push_back(b); }
    if (thumbsLeft > 0) { swprintf(b, 64, L"%zu thumbnail%s", thumbsLeft, thumbsLeft == 1 ? L"" : L"s"); parts.push_back(b); }
    std::wstring text;
    for (size_t i = 0; i < parts.size(); i++) text += (i ? L" · " : L"Loading ") + parts[i];
    if (text == loadText_) return;
    loadText_ = text;
    exportBar_.items[1].text = text;
    exportBar_.Relayout();
}

void Editor::SetStatus(const std::wstring& text, const std::wstring& output, int kind) {
    // 0 info, 1 success, 2 warning, 3 error (guessed from the wording when not given, like FastCut).
    if (kind < 0) {
        std::wstring l = util::Lower(text);
        kind = l.find(L"failed") != std::wstring::npos || text.rfind(L"Error", 0) == 0 || text.rfind(L"Could not", 0) == 0 ? 3
               : l.find(L"warning") != std::wstring::npos || l.find(L"problem") != std::wstring::npos               ? 2
                                                                                                                    : 0;
    }
    auto& st = exportBar_.items[2];
    st.text = text;
    st.glyph = kind == 1 ? glyph::Check : kind == 2 ? glyph::Warning : kind == 3 ? glyph::ErrorBadge : text.empty() ? L"" : glyph::Info;
    st.color = kind == 1 ? theme::Success : kind == 2 ? theme::Warn : kind == 3 ? theme::Error : 0xFFFFFFFF;
    lastOutput_ = output;
    exportBar_.items[6].visible = !output.empty() && !jobRunning_ && FileExists(output);
    exportBar_.Relayout();
}

int Editor::Box(const std::wstring& text, const std::wstring& title, UINT flags) {
    if (util::TestMode()) {  // test runs never put windows on screen
        util::TimingLog(("box: " + util::Utf8(title) + ": " + util::Utf8(text)).c_str());
        return (flags & 0xF) == MB_YESNO ? IDNO : IDOK;
    }
    return Box(text.c_str(), title.c_str(), flags);
}

void Editor::SetOption(const std::wstring& key, const std::wstring& value) {
    if (key == L"cut_mode") s_.cutMode = value;
    else if (key == L"merge") s_.merge = value == L"1";
    else if (key == L"size_mb") s_.discordSizeMB = _wtoi(value.c_str());
    else if (key == L"discord") s_.discordReady = value == L"1";
    else if (key == L"format") s_.format = value;
    UpdateChecks();
    UpdateSegHeader();
}

bool Editor::RunAction(const std::wstring& id) {
    if (!actionIndex_.count(id)) return false;
    Run(id);
    return true;
}

std::wstring Editor::DebugState() const {
    std::wstring s = L"edit: active " + std::to_wstring(active_) + L", media " + std::to_wstring(media_ != nullptr) +
                     L", keyframes " + std::to_wstring(kf_.pts.size()) + L", segments " + std::to_wstring(segs_.size());
    for (const auto& g : segs_) s += L" [" + Num(g.start) + L"-" + Num(g.end) + (g.enabled ? L"" : L" off") + L"]";
    s += L", selected " + std::to_wstring(tl_.selected) + L", job " + std::to_wstring(jobRunning_) + L", status: " +
         (created_ ? exportBar_.items[2].text : L"");
    return s;
}

EditSettings Editor::JobSettings() const {
    EditSettings s = s_;
    wchar_t env[MAX_PATH];
    if (util::TestMode()) {
        // Test runs must never write next to the user's media: without FASTPLAY_EDIT_OUT they go to FastPlay's cache.
        s.outDir = GetEnvironmentVariableW(L"FASTPLAY_EDIT_OUT", env, MAX_PATH) ? env : util::LocalCacheDir();
    }
    return s;
}

std::wstring Editor::OutDir() const {
    if (util::TestMode()) return JobSettings().outDir;
    if (!s_.outDir.empty() && GetFileAttributesW(s_.outDir.c_str()) != INVALID_FILE_ATTRIBUTES) return s_.outDir;
    std::wstring p = media_ ? media_->path : file_;
    return p.empty() ? util::ExeDir() : util::DirName(p);
}

void Editor::Post(std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
    if (!PostMessageW(app_->hwnd_, WM_APP_EDIT_UI, 0, (LPARAM)f)) delete f;
}

void Editor::RunJob(const std::wstring& name, std::function<std::pair<std::wstring, std::wstring>(ProgressFn, Cancel*)> work,
                    bool onExportButton, std::function<void(bool)> after) {
    if (jobRunning_) {
        SetStatus(L"Another job is running.");
        return;
    }
    if (jobThread_.joinable()) jobThread_.join();
    jobRunning_ = true;
    jobOnExport_ = onExportButton;
    jobName_ = name;
    jobAfter_ = std::move(after);
    jobCancel_ = std::make_shared<Cancel>();
    jobStart_ = GetTickCount();
    {
        std::lock_guard<std::mutex> l(progMu_);
        progFrac_ = -1;
        progMsg_.clear();
    }
    auto& bar = exportBar_.items;
    bar[3].visible = bar[4].visible = bar[5].visible = true;
    bar[3].value = -1;
    bar[4].text.clear();
    bar[6].visible = false;
    bar[12].enabled = false;
    if (onExportButton) bar[12].text = L"Exporting...";
    SetStatus(name + L"...");
    auto cancel = jobCancel_;
    HWND hwnd = app_->hwnd_;
    ProgressFn progress = [this](double f, const std::wstring& m) {
        std::lock_guard<std::mutex> l(progMu_);
        progFrac_ = f;
        progMsg_ = m;
    };
    jobThread_ = std::thread([work = std::move(work), progress, cancel, hwnd] {
        auto* o = new JobOutcome;
        try {
            auto r = work(progress, cancel.get());
            o->ok = true;
            o->msg = r.first;
            o->output = r.second;
        } catch (const Cancelled&) {
            o->cancelled = true;
        } catch (const EditError& e) {
            o->error = e.msg;
        } catch (const std::exception& e) {
            o->error = util::Wide(e.what());
        }
        if (!PostMessageW(hwnd, WM_APP_EDIT_JOBDONE, 0, (LPARAM)o)) delete o;
    });
}

void Editor::OnJobDone(JobOutcome* raw) {
    std::unique_ptr<JobOutcome> o(raw);
    if (jobThread_.joinable()) jobThread_.join();
    double secs = (GetTickCount() - jobStart_) / 1000.0;
    jobRunning_ = false;
    jobCancel_.reset();
    auto& bar = exportBar_.items;
    bar[3].visible = bar[4].visible = bar[5].visible = false;
    bar[12].enabled = media_ != nullptr;
    if (app_->taskbar_) {
        app_->taskbar_->SetProgressState(app_->hwnd_, TBPF_NOPROGRESS);
        app_->lastTaskbarState_ = -1;
    }
    if (o->ok) {
        std::wstring l = util::Lower(o->msg);
        bool warned = l.find(L"warning") != std::wstring::npos || l.find(L"problem") != std::wstring::npos;
        wchar_t t[32];
        swprintf(t, 32, L"  (%.1fs)", secs);
        SetStatus(o->msg + t, o->output, warned ? 2 : 1);
    } else if (o->cancelled) {
        SetStatus(jobName_ + L" cancelled.", L"", 0);
    } else {
        std::wstring last = o->error;
        size_t nl = last.find_last_of(L'\n');
        if (nl != std::wstring::npos) last = last.substr(nl + 1);
        SetStatus(jobName_ + L" failed: " + last, L"", 3);
        std::wstring box = o->error + L"\n\nSee Tools > FFmpeg log (F12) for the full command.";
        Box(box.c_str(), (jobName_ + L" failed").c_str(), MB_OK | MB_ICONWARNING);
    }
    UpdateSegHeader();  // restores the Export button text
    auto after = std::move(jobAfter_);
    jobAfter_ = nullptr;
    if (after) after(o->ok);
}

void Editor::ShowIssues(const std::vector<std::wstring>& issues) {
    std::wstring text;
    for (size_t i = 0; i < issues.size() && i < 15; i++) text += (i ? L"\n\n" : L"") + issues[i];
    if (issues.size() > 15) text += L"\n\n...and " + std::to_wstring(issues.size() - 15) + L" more";
    Post([this, text] { Box(text.c_str(), L"Export finished with warnings", MB_OK | MB_ICONWARNING); });
}

bool Editor::OnAppMessage(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_APP_EDIT_LOADED:
        OnLoaded((LoadResult*)l, (unsigned)w);
        return true;
    case WM_APP_EDIT_KEYFRAMES: {
        std::unique_ptr<KeyframeIndex> k((KeyframeIndex*)l);
        if ((unsigned)w != loadSerial_) return true;
        kf_ = std::move(*k);
        util::TimingLog("edit: keyframes ready");
        tl_.keyframes = kf_.pts;
        tl_.Invalidate();
        cards_.Refresh();
        return true;
    }
    case WM_APP_EDIT_JOBDONE:
        OnJobDone((JobOutcome*)l);
        return true;
    case WM_APP_EDIT_UI: {
        std::unique_ptr<std::function<void()>> fn((std::function<void()>*)l);
        (*fn)();
        return true;
    }
    case WM_TIMER:
        if (w != kTick) return false;
        OnTimer();
        return true;
    }
    return false;
}

bool Editor::ConfirmQuit() {
    if (!jobRunning_) return true;
    return Box(L"A job is still running. Cancel it and quit?", L"FastPlay", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void Editor::Shutdown() {
    if (jobCancel_) jobCancel_->flag = true;
    if (loadCancel_) loadCancel_->flag = true;
    if (jobThread_.joinable()) jobThread_.join();
    if (loadThread_.joinable()) loadThread_.join();
    if (waveThread_.joinable()) waveThread_.join();
    jobRunning_ = false;
    if (created_ && app_->hwnd_) KillTimer(app_->hwnd_, kTick);
    SaveAutosave();
    if (panelBrush_) DeleteObject(panelBrush_);
    panelBrush_ = nullptr;
}

// ------------------------------------------------------------------------------------------------ thumbnails

std::shared_ptr<const Thumb> Editor::Get(double t, double tol) {
    return app_->thumbs_ ? app_->thumbs_->GetNear(t, tol) : nullptr;
}

void Editor::Want(const std::vector<double>& times, double, double center) {
    if (app_->thumbs_) app_->thumbs_->Want(times, center);
}

}  // namespace edit
