// Edit-mode dialogs (FastCut Dialogs.cs), built as in-memory dialog templates: no resources, nothing loaded until
// a dialog opens. Dark styling follows the Windows app theme (title bar, colours, themed controls, custom-drawn
// check boxes and radio buttons whose text the themes would otherwise draw black).
#include "eddialogs.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cstdlib>
#include <locale.h>

namespace edit::dialogs {

const wchar_t* const kFormats[] = {L"same", L"mp4", L"mkv", L"mov",  L"webm", L"ts",  L"m4a",
                                   L"mka",  L"mp3", L"flac", L"wav", L"ogg",  L"opus", L"avi"};
const int kFormatCount = (int)(sizeof kFormats / sizeof kFormats[0]);

namespace detail {
// Test hooks (tools/edit-tests/test_dialogs.cpp): -1 = follow Windows, 0 = light, 1 = dark; quiet = validation
// errors go to g_lastError instead of a message box.
int g_themeOverride = -1;
bool g_testQuiet = false;
bool g_testNoFocus = false;  // never move the focus (moving it can activate the window)
std::wstring g_lastError;
}  // namespace detail

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Control ids

enum : WORD {
    kHint = 900,  // grey helper text
    kSegStart = 101, kSegEnd, kSegLabel, kSegTags, kSegInclude,
    kExpDir = 201, kExpBrowse, kExpMode, kExpFormat, kExpCut, kExpName, kExpMergeName, kExpSpeed, kExpSizeMB,
    kExpMeta, kExpChapters, kExpSegChapters, kExpNegTs, kExpFastStart, kExpDiscord, kExpVerify, kExpConfirm,
    kSetDir = 301, kSetBrowse, kSetName, kSetMergeName, kSetSnap, kSetSeekSmall, kSetSeekLarge, kSetFrameJump,
    kSetNudge, kSetThumbs, kSetWave, kSetKeyframes, kSetAutoSave, kSetConfirm, kSetVerify, kSetMeta, kSetChapters,
    kSetSegChapters, kSetNegTs, kSetFastStart, kSetDiscord,
    kBatSame = 401, kBatSaved, kBatTrim, kBatTrimStart, kBatTrimEnd,
    kPrompt1 = 501, kPrompt2,
    kViewEdit = 601, kViewCopy,
};

enum : WORD { kClsButton = 0x80, kClsEdit = 0x81, kClsStatic = 0x82, kClsCombo = 0x85 };
constexpr WORD kNoId = 0xFFFF;  // IDC_STATIC
constexpr int kMargin = 7;      // dialog units, like FastPlay's own dialogs

// ---------------------------------------------------------------------------------------------------------------
// Text helpers

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

std::wstring GetText(HWND d, int id) {
    HWND c = GetDlgItem(d, id);
    std::wstring s((size_t)GetWindowTextLengthW(c), L'\0');
    if (!s.empty()) s.resize((size_t)GetWindowTextW(c, &s[0], (int)s.size() + 1));
    return s;
}

void SetText(HWND d, int id, const std::wstring& s) { SetDlgItemTextW(d, id, s.c_str()); }
bool Checked(HWND d, int id) { return IsDlgButtonChecked(d, id) == BST_CHECKED; }
void Check(HWND d, int id, bool on) { CheckDlgButton(d, id, on ? BST_CHECKED : BST_UNCHECKED); }

// Focus a control (and select an edit box's text).
void Focus(HWND d, int id) {
    if (!detail::g_testNoFocus) SendMessageW(d, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(d, id), TRUE);
}

// Invariant number: '.' decimal (',' accepted as one too, like FastCut's speed box), whole text, finite.
bool ParseNumber(const std::wstring& text, double* out) {
    std::wstring t = Trim(text);
    std::replace(t.begin(), t.end(), L',', L'.');
    if (t.empty()) return false;
    static const _locale_t c = _create_locale(LC_NUMERIC, "C");
    wchar_t* end = nullptr;
    double v = _wcstod_l(t.c_str(), &end, c);
    if (end != t.c_str() + t.size() || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

bool ParseInt(const std::wstring& text, int* out) {
    std::wstring t = Trim(text);
    if (t.empty() || t.size() > 9) return false;
    size_t i = (t[0] == L'-' || t[0] == L'+') ? 1 : 0;
    if (i == t.size()) return false;
    for (size_t k = i; k < t.size(); ++k)
        if (t[k] < L'0' || t[k] > L'9') return false;
    *out = _wtoi(t.c_str());
    return true;
}

// Edit controls want CRLF line breaks.
std::wstring ToCrlf(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) r += L'\r';
        r += s[i];
    }
    return r;
}

std::wstring Plural(int n, const wchar_t* word) {
    return std::to_wstring(n) + L" " + word + (n == 1 ? L"" : L"s");
}

// ---------------------------------------------------------------------------------------------------------------
// Dialog template building

// Measures text in dialog units for the dialog font (Segoe UI 9), the same way the dialog manager derives them,
// so labels and check boxes get exactly the room they need.
class Measure {
public:
    Measure() {
        dc_ = CreateCompatibleDC(nullptr);
        font_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        old_ = SelectObject(dc_, font_);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc_, &tm);
        SIZE sz = {};
        GetTextExtentPoint32W(dc_, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &sz);
        baseX_ = std::max(1, (int)((sz.cx / 26 + 1) / 2));
        baseY_ = std::max(1, (int)tm.tmHeight);
    }
    ~Measure() {
        SelectObject(dc_, old_);
        DeleteObject(font_);
        DeleteDC(dc_);
    }
    Measure(const Measure&) = delete;
    Measure& operator=(const Measure&) = delete;

    // Width of one line, rounded up, with a little slack for hinting differences at other DPIs.
    int Width(const wchar_t* s) const {
        SIZE sz = {};
        GetTextExtentPoint32W(dc_, s, (int)wcslen(s), &sz);
        return (sz.cx * 4 + baseX_ - 1) / baseX_ + 2;
    }
    // Height of word-wrapped text in a box widthDlu wide.
    int Height(const wchar_t* s, int widthDlu) const {
        RECT r = {0, 0, MulDiv(widthDlu - 2, baseX_, 4), 0};
        DrawTextW(dc_, s, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        return (r.bottom * 8 + baseY_ - 1) / baseY_;
    }

private:
    HDC dc_;
    HFONT font_;
    HGDIOBJ old_;
    int baseX_, baseY_;
};

class Template {
public:
    void Add(DWORD style, int x, int y, int w, int h, WORD id, WORD cls, const std::wstring& text = L"") {
        items_.push_back({style, x, y, w, h, id, cls, text});
    }

    // DLGTEMPLATE + items (DWORD-aligned items, WORD-aligned strings). Not WS_VISIBLE: DialogBox shows it itself
    // and the test creates it hidden.
    std::vector<uint8_t> Build(const std::wstring& title, int w, int h, DWORD extraStyle) const {
        std::vector<uint8_t> b;
        auto put = [&](const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
        auto word = [&](WORD v) { put(&v, 2); };
        auto str = [&](const std::wstring& s) { put(s.c_str(), (s.size() + 1) * 2); };
        auto align = [&] { while (b.size() % 4) b.push_back(0); };
        DLGTEMPLATE t = {};
        t.style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | extraStyle;
        t.cdit = (WORD)items_.size();
        t.cx = (short)w;
        t.cy = (short)h;
        put(&t, sizeof t);
        word(0);  // menu
        word(0);  // class
        str(title);
        word(9);  // font size
        str(L"Segoe UI");
        for (const Item& it : items_) {
            align();
            DLGITEMTEMPLATE d = {};
            d.style = it.style | WS_CHILD | WS_VISIBLE;
            d.x = (short)it.x;
            d.y = (short)it.y;
            d.cx = (short)it.w;
            d.cy = (short)it.h;
            d.id = it.id;
            put(&d, sizeof d);
            word(0xFFFF);
            word(it.cls);
            str(it.text);
            word(0);  // no creation data
        }
        return b;
    }

private:
    struct Item {
        DWORD style;
        int x, y, w, h;
        WORD id, cls;
        std::wstring text;
    };
    std::vector<Item> items_;
};

// Row layout like FastCut's DialogBase: a label column sized to the widest label, controls to its right, OK and
// Cancel at the bottom right. Every control starts its own WS_GROUP except radio buttons after the first.
class Form {
public:
    Form(int width, std::initializer_list<const wchar_t*> labels) : width_(width) {
        for (const wchar_t* l : labels) labelW_ = std::max(labelW_, m_.Width(l));
    }

    int y = kMargin;
    int Width() const { return width_; }
    int CtrlX() const { return kMargin + (labelW_ ? labelW_ + 4 : 0); }
    int CtrlW() const { return width_ - kMargin - CtrlX(); }
    const Measure& M() const { return m_; }

    void Label(const wchar_t* text, int atY) {
        if (text && *text) t_.Add(SS_LEFT | SS_NOPREFIX | WS_GROUP, kMargin, atY, labelW_, 9, kNoId, kClsStatic, text);
    }

    // Edit box; w = 0 fills the row; a hint goes right of a narrow box.
    void Edit(WORD id, const wchar_t* label, int w = 0, const wchar_t* hint = nullptr) {
        Label(label, y + 3);
        int ew = w ? w : CtrlW();
        t_.Add(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, CtrlX(), y, ew, 14, id, kClsEdit);
        if (hint) {
            int hx = CtrlX() + ew + 5;
            t_.Add(SS_LEFT | SS_NOPREFIX | WS_GROUP, hx, y + 3, width_ - kMargin - hx, 9, kHint, kClsStatic, hint);
        }
        y += 18;
    }

    // Two narrow boxes on one row, the second with its own label (label2W = room for the widest such label).
    void Pair(WORD id1, const wchar_t* label1, WORD id2, const wchar_t* label2, int editW, int label2W) {
        Label(label1, y + 3);
        t_.Add(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, CtrlX(), y, editW, 14, id1, kClsEdit);
        int lx = CtrlX() + editW + 12;
        t_.Add(SS_LEFT | SS_NOPREFIX | WS_GROUP, lx, y + 3, label2W, 9, kNoId, kClsStatic, label2);
        t_.Add(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, lx + label2W + 4, y, editW, 14, id2, kClsEdit);
        y += 18;
    }

    // Folder box plus a "…" browse button.
    void Folder(WORD editId, WORD buttonId, const wchar_t* label) {
        Label(label, y + 3);
        t_.Add(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, CtrlX(), y, CtrlW() - 20, 14, editId, kClsEdit);
        t_.Add(BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, CtrlX() + CtrlW() - 16, y, 16, 14, buttonId, kClsButton,
               L"…");
        y += 18;
    }

    void Combo(WORD id, const wchar_t* label) {
        Label(label, y + 3);
        t_.Add(CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, CtrlX(), y, CtrlW(), 160, id, kClsCombo);
        y += 18;
    }

    // Check box in the control column (wraps to more lines when the text is wider), optional label on its left.
    void Check(WORD id, const wchar_t* text, const wchar_t* label = nullptr) {
        Label(label, y + 1);
        bool wrap = m_.Width(text) + 14 > CtrlW();
        int h = wrap ? m_.Height(text, CtrlW() - 14) + 1 : 10;
        t_.Add(BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP | (wrap ? BS_MULTILINE : 0), CtrlX(), y, CtrlW(), h, id,
               kClsButton, text);
        y += h + 3;
    }

    // Several short check boxes side by side (the caller keeps the row within the control column).
    void CheckRow(std::initializer_list<std::pair<WORD, const wchar_t*>> items, const wchar_t* label = nullptr) {
        Label(label, y + 1);
        int x = CtrlX();
        for (const auto& it : items) {
            int w = m_.Width(it.second) + 14;
            t_.Add(BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, x, y, w, 10, it.first, kClsButton, it.second);
            x += w + 10;
        }
        y += 13;
    }

    // Full-width radio button; the first of a group starts it and takes the tab stop.
    void Radio(WORD id, const wchar_t* text, bool first) {
        t_.Add(BS_AUTORADIOBUTTON | (first ? WS_GROUP | WS_TABSTOP : 0), kMargin, y, width_ - 2 * kMargin, 10, id,
               kClsButton, text);
        y += 13;
    }

    // Grey helper text in the control column (or the full width), word-wrapped.
    void Hint(const wchar_t* text, bool fullWidth = false) {
        int x = fullWidth ? kMargin : CtrlX();
        int w = width_ - kMargin - x;
        int h = m_.Height(text, w);
        t_.Add(SS_LEFT | SS_NOPREFIX | WS_GROUP, x, y, w, h, kHint, kClsStatic, text);
        y += h + 4;
    }

    // Static text across the full width (prompt labels), word-wrapped.
    void Text(const wchar_t* text) {
        int w = width_ - 2 * kMargin;
        int h = m_.Height(text, w);
        t_.Add(SS_LEFT | SS_NOPREFIX | WS_GROUP, kMargin, y, w, h, kNoId, kClsStatic, text);
        y += h + 2;
    }

    // Full-width edit box under a Text().
    void WideEdit(WORD id) {
        t_.Add(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, kMargin, y, width_ - 2 * kMargin, 14, id, kClsEdit);
        y += 18;
    }

    void Gap(int dy) { y += dy; }

    std::vector<uint8_t> Finish(const std::wstring& title) {
        y += 4;
        t_.Add(BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, width_ - kMargin - 104, y, 50, 14, IDOK, kClsButton, L"OK");
        t_.Add(BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, width_ - kMargin - 50, y, 50, 14, IDCANCEL, kClsButton, L"Cancel");
        y += 14 + kMargin;
        return t_.Build(title, width_, y, 0);
    }

    Template& Raw() { return t_; }

private:
    Measure m_;
    Template t_;
    int width_;
    int labelW_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Dark styling (all optional at runtime: uxtheme and dwmapi are looked up, never linked)

struct UxTheme {
    HRESULT(WINAPI* setWindowTheme)(HWND, LPCWSTR, LPCWSTR) = nullptr;
    HTHEME(WINAPI* openThemeData)(HWND, LPCWSTR) = nullptr;
    HRESULT(WINAPI* closeThemeData)(HTHEME) = nullptr;
    HRESULT(WINAPI* drawThemeBackground)(HTHEME, HDC, int, int, LPCRECT, LPCRECT) = nullptr;
    HRESULT(WINAPI* getThemePartSize)(HTHEME, HDC, int, int, LPCRECT, THEMESIZE, SIZE*) = nullptr;
    bool Ok() const { return setWindowTheme && openThemeData && closeThemeData && drawThemeBackground && getThemePartSize; }
};

HMODULE SystemDll(const wchar_t* name) {
    HMODULE m = GetModuleHandleW(name);
    return m ? m : LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

const UxTheme& Ux() {
    static const UxTheme ux = [] {
        UxTheme u;
        if (HMODULE m = SystemDll(L"uxtheme.dll")) {
            u.setWindowTheme = (decltype(u.setWindowTheme))GetProcAddress(m, "SetWindowTheme");
            u.openThemeData = (decltype(u.openThemeData))GetProcAddress(m, "OpenThemeData");
            u.closeThemeData = (decltype(u.closeThemeData))GetProcAddress(m, "CloseThemeData");
            u.drawThemeBackground = (decltype(u.drawThemeBackground))GetProcAddress(m, "DrawThemeBackground");
            u.getThemePartSize = (decltype(u.getThemePartSize))GetProcAddress(m, "GetThemePartSize");
        }
        return u;
    }();
    return ux;
}

bool UseDark() {
    if (detail::g_themeOverride >= 0) return detail::g_themeOverride == 1 && Ux().Ok();
    HIGHCONTRASTW hc = {sizeof hc};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON)) return false;
    DWORD v = 1, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS)
        return false;
    return v == 0 && Ux().Ok();
}

constexpr COLORREF kDarkBg = RGB(32, 32, 32), kDarkField = RGB(43, 43, 43), kDarkText = RGB(230, 230, 230),
                   kDarkHint = RGB(150, 150, 150);

HBRUSH DarkBgBrush() {
    static HBRUSH b = CreateSolidBrush(kDarkBg);
    return b;
}
HBRUSH DarkFieldBrush() {
    static HBRUSH b = CreateSolidBrush(kDarkField);
    return b;
}

bool IsClass(HWND h, const wchar_t* cls) {
    wchar_t name[32];
    return GetClassNameW(h, name, 32) && _wcsicmp(name, cls) == 0;
}

bool IsCheckOrRadio(HWND h) {
    if (!IsClass(h, L"Button")) return false;
    LONG t = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    return t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON;
}

void ApplyDark(HWND d) {
    if (HMODULE dwm = SystemDll(L"dwmapi.dll")) {
        using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto set = (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL on = TRUE;
            if (FAILED(set(d, 20, &on, sizeof on))) set(d, 19, &on, sizeof on);  // DWMWA_USE_IMMERSIVE_DARK_MODE
        }
    }
    EnumChildWindows(
        d,
        [](HWND c, LPARAM) -> BOOL {
            const UxTheme& ux = Ux();
            if (IsClass(c, L"ComboBox")) {
                ux.setWindowTheme(c, L"DarkMode_CFD", nullptr);
                COMBOBOXINFO ci = {sizeof ci};
                if (GetComboBoxInfo(c, &ci) && ci.hwndList) ux.setWindowTheme(ci.hwndList, L"DarkMode_Explorer", nullptr);
            } else if (IsClass(c, L"Edit")) {
                LONG style = GetWindowLongW(c, GWL_STYLE);
                bool multi = style & ES_MULTILINE;
                ux.setWindowTheme(c, multi ? L"DarkMode_Explorer" : L"DarkMode_CFD", nullptr);
                if (multi) {  // the dark theme draws its border white; the field colour is enough
                    // (the dialog manager turns an edit's WS_BORDER into WS_EX_CLIENTEDGE)
                    SetWindowLongW(c, GWL_STYLE, style & ~WS_BORDER);
                    SetWindowLongW(c, GWL_EXSTYLE, GetWindowLongW(c, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
                    SetWindowPos(c, nullptr, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                }
            } else if (IsClass(c, L"Button")) {
                ux.setWindowTheme(c, L"DarkMode_Explorer", nullptr);
            }
            return TRUE;
        },
        0);
}

// Check boxes and radio buttons: the dark theme draws the glyph, we draw the text (the theme would draw it black).
LRESULT DrawDarkCheck(HWND d, const NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const UxTheme& ux = Ux();
    HWND h = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT rc = cd->rc;
    FillRect(dc, &rc, DarkBgBrush());

    LONG style = GetWindowLongW(h, GWL_STYLE);
    LONG type = style & BS_TYPEMASK;
    bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool disabled = (cd->uItemState & CDIS_DISABLED) || !IsWindowEnabled(h);
    int state = disabled ? 4 : (cd->uItemState & CDIS_SELECTED) ? 3 : (cd->uItemState & CDIS_HOT) ? 2 : 1;
    if (checked) state += 4;  // *_CHECKEDNORMAL.. follow *_UNCHECKED* for both parts
    int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;

    UINT dpi = GetDpiForWindow(h);
    SIZE glyph = {MulDiv(13, dpi, 96), MulDiv(13, dpi, 96)};
    if (HTHEME th = ux.openThemeData(h, L"Button")) {
        ux.getThemePartSize(th, dc, part, state, nullptr, TS_DRAW, &glyph);
        RECT g = {rc.left, rc.top + (rc.bottom - rc.top - glyph.cy) / 2, rc.left + glyph.cx, 0};
        g.bottom = g.top + glyph.cy;
        ux.drawThemeBackground(th, dc, part, state, &g, nullptr);
        ux.closeThemeData(th);
    }

    wchar_t text[512];
    GetWindowTextW(h, text, 512);
    HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? kDarkHint : kDarkText);
    RECT tr = {rc.left + glyph.cx + MulDiv(4, dpi, 96), rc.top, rc.right, rc.bottom};
    UINT fmt = DT_LEFT | DT_NOPREFIX;
    RECT calc = tr;
    if (style & BS_MULTILINE) {
        DrawTextW(dc, text, -1, &calc, fmt | DT_WORDBREAK | DT_CALCRECT);
        int dy = ((tr.bottom - tr.top) - (calc.bottom - calc.top)) / 2;
        calc = {tr.left, tr.top + dy, tr.right, tr.top + dy + (calc.bottom - calc.top)};
        DrawTextW(dc, text, -1, &calc, fmt | DT_WORDBREAK);
    } else {
        DrawTextW(dc, text, -1, &calc, fmt | DT_SINGLELINE | DT_CALCRECT);
        int dy = ((tr.bottom - tr.top) - (calc.bottom - calc.top)) / 2;
        calc = {tr.left, tr.top + dy, std::min(calc.right, tr.right), tr.top + dy + (calc.bottom - calc.top)};
        DrawTextW(dc, text, -1, &calc, fmt | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if ((cd->uItemState & CDIS_FOCUS) && !(SendMessageW(d, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        InflateRect(&calc, 1, 1);
        SetTextColor(dc, kDarkText);
        DrawFocusRect(dc, &calc);
    }
    SelectObject(dc, oldFont);
    return CDRF_SKIPDEFAULT;
}

INT_PTR Colors(bool dark, UINT m, HDC dc, HWND c) {
    bool hint = GetDlgCtrlID(c) == kHint;
    if (!dark) {
        if (m != WM_CTLCOLORSTATIC || !hint) return FALSE;
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
    }
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
        SetBkColor(dc, kDarkBg);
        return (INT_PTR)DarkBgBrush();
    case WM_CTLCOLORSTATIC:
        if (IsClass(c, L"Edit")) break;  // read-only / disabled edit boxes: field colours
        SetTextColor(dc, hint ? kDarkHint : kDarkText);
        SetBkColor(dc, kDarkBg);
        return (INT_PTR)DarkBgBrush();
    }
    SetTextColor(dc, IsWindowEnabled(c) ? kDarkText : kDarkHint);
    SetBkColor(dc, kDarkField);
    return (INT_PTR)DarkFieldBrush();
}

// ---------------------------------------------------------------------------------------------------------------
// Shared dialog procedure

struct Dialog {
    virtual ~Dialog() = default;
    // Fills the controls. Return true to let the dialog manager focus the first tab stop.
    virtual bool Init(HWND d) = 0;
    // OK pressed: validate everything first, then write the outputs. On failure set *error and the control to fix.
    virtual bool Accept(HWND, std::wstring*, int*) { return true; }
    virtual bool Command(HWND, int /*id*/, int /*code*/) { return false; }
    // Any other message; set *handled to consume it.
    virtual INT_PTR Message(HWND, UINT, WPARAM, LPARAM, bool* /*handled*/) { return 0; }
    virtual void Destroyed() {}

    bool dark = false;
    bool modal = true;      // false: created by the test hook (modeless, deleted with its window)
    int result = 0;         // test hook: IDOK once accepted
};

void Close(HWND d, Dialog* dlg, int id) {
    dlg->result = id;
    if (dlg->modal) EndDialog(d, id);
}

INT_PTR CALLBACK Proc(HWND d, UINT m, WPARAM w, LPARAM l) {
    auto* dlg = (Dialog*)GetWindowLongPtrW(d, DWLP_USER);
    if (m == WM_INITDIALOG) {
        dlg = (Dialog*)l;
        SetWindowLongPtrW(d, DWLP_USER, l);
        dlg->dark = UseDark();
        if (dlg->dark) ApplyDark(d);
        bool defaultFocus = dlg->Init(d);
        return defaultFocus && !detail::g_testNoFocus ? TRUE : FALSE;
    }
    if (!dlg) return FALSE;
    bool handled = false;
    INT_PTR r = dlg->Message(d, m, w, l, &handled);
    if (handled) return r;
    switch (m) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return Colors(dlg->dark, m, (HDC)w, (HWND)l);
    case WM_NOTIFY: {
        auto* nm = (const NMHDR*)l;
        if (dlg->dark && nm->code == NM_CUSTOMDRAW && IsCheckOrRadio(nm->hwndFrom)) {
            SetWindowLongPtrW(d, DWLP_MSGRESULT, DrawDarkCheck(d, (const NMCUSTOMDRAW*)l));
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDOK) {
            std::wstring error;
            int bad = 0;
            if (dlg->Accept(d, &error, &bad)) {
                Close(d, dlg, IDOK);
            } else {
                if (detail::g_testQuiet) {
                    detail::g_lastError = error;
                } else {
                    wchar_t caption[256];
                    GetWindowTextW(d, caption, 256);
                    MessageBoxW(d, error.c_str(), caption, MB_OK | MB_ICONWARNING);
                }
                if (bad) Focus(d, bad);
            }
            return TRUE;
        }
        if (id == IDCANCEL) {
            Close(d, dlg, IDCANCEL);
            return TRUE;
        }
        return dlg->Command(d, id, HIWORD(w)) ? TRUE : FALSE;
    }
    case WM_NCDESTROY:
        dlg->Destroyed();
        SetWindowLongPtrW(d, DWLP_USER, 0);
        if (!dlg->modal) delete dlg;
        return FALSE;
    }
    return FALSE;
}

bool RunModal(HWND owner, const std::vector<uint8_t>& tmpl, Dialog& dlg) {
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (const DLGTEMPLATE*)tmpl.data(), owner, Proc,
                                   (LPARAM)&dlg) == IDOK;
}

void FillCombo(HWND d, int id, const std::vector<std::wstring>& items, int sel) {
    HWND c = GetDlgItem(d, id);
    for (const auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageW(c, CB_SETCURSEL, (WPARAM)std::max(0, sel), 0);
}

int ComboIndex(HWND d, int id) { return std::max(0, (int)SendDlgItemMessageW(d, id, CB_GETCURSEL, 0, 0)); }

void Browse(HWND d, int editId) {
    std::wstring dir = PickFolder(d, Trim(GetText(d, editId)));
    if (!dir.empty()) SetText(d, editId, dir);
}

void Cue(HWND d, int id, const wchar_t* text) { SendDlgItemMessageW(d, id, EM_SETCUEBANNER, TRUE, (LPARAM)text); }

// ---------------------------------------------------------------------------------------------------------------
// SegmentDialog

struct SegmentDlg : Dialog {
    Segment* seg;
    std::function<std::wstring(double)> fmt;
    std::function<bool(const std::wstring&, double*)> parse;

    static std::vector<uint8_t> Layout() {
        Form f(250, {L"Start", L"End", L"Label", L"Tags (k=v, ...)"});
        f.Edit(kSegStart, L"Start");
        f.Edit(kSegEnd, L"End");
        f.Edit(kSegLabel, L"Label");
        f.Edit(kSegTags, L"Tags (k=v, ...)");
        f.Check(kSegInclude, L"Include in export");
        return f.Finish(L"Edit segment");
    }

    bool Init(HWND d) override {
        SetText(d, kSegStart, fmt(seg->start));
        SetText(d, kSegEnd, fmt(seg->end));
        SetText(d, kSegLabel, seg->name);
        SetText(d, kSegTags, seg->tags);
        Check(d, kSegInclude, seg->enabled);
        return true;
    }

    bool Accept(HWND d, std::wstring* error, int* bad) override {
        double a = 0, b = 0;
        if (!parse(Trim(GetText(d, kSegStart)), &a)) {
            *error = L"Start is not a valid time (e.g. 1:02.5 or 62.5).";
            *bad = kSegStart;
            return false;
        }
        if (!parse(Trim(GetText(d, kSegEnd)), &b)) {
            *error = L"End is not a valid time (e.g. 1:02.5 or 62.5).";
            *bad = kSegEnd;
            return false;
        }
        if (b <= a) {
            *error = L"Invalid times: end must be after start.";
            *bad = kSegEnd;
            return false;
        }
        seg->start = a;
        seg->end = b;
        seg->name = Trim(GetText(d, kSegLabel));
        seg->tags = Trim(GetText(d, kSegTags));
        seg->enabled = Checked(d, kSegInclude);
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// ExportDialog

const wchar_t* const kCutModes[] = {L"keyframe", L"exact", L"smart"};

struct ExportDlg : Dialog {
    EditSettings* s;
    double* speed;
    std::wstring inputExt;

    static std::vector<uint8_t> Layout(int segCount) {
        Form f(340, {L"Output folder", L"Mode", L"Format", L"Cut mode", L"File name", L"Merged name", L"Speed ×",
                     L"Size limit (MB)"});
        f.Folder(kExpDir, kExpBrowse, L"Output folder");
        f.Combo(kExpMode, L"Mode");
        f.Combo(kExpFormat, L"Format");
        f.Combo(kExpCut, L"Cut mode");
        f.Edit(kExpName, L"File name");
        f.Edit(kExpMergeName, L"Merged name");
        f.Gap(-3);
        f.Hint(L"Tokens: {name} {n} {label} {start} {end}");
        f.Edit(kExpSpeed, L"Speed ×", 40, L"1 = unchanged; other speeds retime the video and drop audio");
        f.Gap(2);
        f.CheckRow({{kExpMeta, L"Preserve metadata"}, {kExpChapters, L"Preserve chapters"}});
        f.Check(kExpSegChapters, L"Create chapters from segments (merge)");
        f.Check(kExpNegTs, L"Shift timestamps to start at zero (avoid_negative_ts)");
        f.Check(kExpFastStart, L"MP4/MOV: move index to front (faststart)");
        f.Check(kExpDiscord, L"Discord-ready: re-encode HEVC/AV1/10-bit video to H.264 (H.264 stays lossless)");
        f.Gap(2);
        f.Edit(kExpSizeMB, L"Size limit (MB)", 40, L"Discord upload limit, 0 = none; bigger exports are re-encoded");
        f.Check(kExpVerify, L"Check outputs after export (duration, tracks)");
        f.Check(kExpConfirm, L"Show this dialog before every export");
        return f.Finish(L"Export " + Plural(segCount, L"segment"));
    }

    bool Init(HWND d) override {
        SetText(d, kExpDir, s->outDir);
        Cue(d, kExpDir, L"(same folder as input)");
        FillCombo(d, kExpMode, {L"Separate files", L"Merge into one file"}, s->merge ? 1 : 0);
        std::vector<std::wstring> formats;
        int fmtSel = 0;
        for (int i = 0; i < kFormatCount; ++i) {
            if (i == 0)
                formats.push_back(inputExt.empty() ? L"Same as input" : L"Same as input (" + inputExt + L")");
            else
                formats.push_back(kFormats[i]);
            if (s->format == kFormats[i]) fmtSel = i;
        }
        FillCombo(d, kExpFormat, formats, fmtSel);
        FillCombo(d, kExpCut,
                  {L"Keyframe cut (fast, starts at previous keyframe)",
                   L"Normal cut (audio exact, video from next keyframe)",
                   L"Smart cut (re-encode to first keyframe, H.264/HEVC)"},
                  s->cutMode == L"exact" ? 1 : s->cutMode == L"smart" ? 2 : 0);
        SetText(d, kExpName, s->nameTemplate);
        SetText(d, kExpMergeName, s->mergeTemplate);
        SetText(d, kExpSpeed, speed && *speed > 0 ? Num(*speed) : L"1");
        Check(d, kExpMeta, s->preserveMetadata);
        Check(d, kExpChapters, s->preserveChapters);
        Check(d, kExpSegChapters, s->chaptersFromSegments);
        Check(d, kExpNegTs, s->avoidNegativeTs);
        Check(d, kExpFastStart, s->fastStart);
        Check(d, kExpDiscord, s->discordReady);
        SetText(d, kExpSizeMB, std::to_wstring(std::max(0, s->discordSizeMB)));
        Check(d, kExpVerify, s->verifyOutputs);
        Check(d, kExpConfirm, s->confirmExport);
        EnableWindow(GetDlgItem(d, kExpSizeMB), s->discordReady);
        return true;
    }

    bool Command(HWND d, int id, int code) override {
        if (id == kExpBrowse && code == BN_CLICKED) {
            Browse(d, kExpDir);
            return true;
        }
        if (id == kExpDiscord && code == BN_CLICKED) {
            EnableWindow(GetDlgItem(d, kExpSizeMB), Checked(d, kExpDiscord));  // the limit only applies then
            return true;
        }
        return false;
    }

    bool Accept(HWND d, std::wstring* error, int* bad) override {
        double sp = 0;
        if (!ParseNumber(GetText(d, kExpSpeed), &sp) || sp <= 0) {
            *error = L"Speed must be a positive number.";
            *bad = kExpSpeed;
            return false;
        }
        int mb = 0;
        if (!ParseInt(GetText(d, kExpSizeMB), &mb) || mb < 0) {
            *error = L"Size limit must be a whole number of megabytes (0 = no limit).";
            *bad = kExpSizeMB;
            return false;
        }
        if (speed) *speed = sp;
        s->outDir = Trim(GetText(d, kExpDir));
        s->merge = ComboIndex(d, kExpMode) == 1;
        s->format = kFormats[std::min(ComboIndex(d, kExpFormat), kFormatCount - 1)];
        s->cutMode = kCutModes[std::min(ComboIndex(d, kExpCut), 2)];
        std::wstring name = Trim(GetText(d, kExpName)), merged = Trim(GetText(d, kExpMergeName));
        s->nameTemplate = name.empty() ? L"{name}-{n}-{label}" : name;
        s->mergeTemplate = merged.empty() ? L"{name}-cut" : merged;
        s->preserveMetadata = Checked(d, kExpMeta);
        s->preserveChapters = Checked(d, kExpChapters);
        s->chaptersFromSegments = Checked(d, kExpSegChapters);
        s->avoidNegativeTs = Checked(d, kExpNegTs);
        s->fastStart = Checked(d, kExpFastStart);
        s->discordReady = Checked(d, kExpDiscord);
        s->discordSizeMB = mb;
        s->verifyOutputs = Checked(d, kExpVerify);
        s->confirmExport = Checked(d, kExpConfirm);
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// SettingsDialog

// Step ranges from FastCut's shortcut editor (its NumericUpDown limits).
struct StepField {
    int id;
    const wchar_t* name;
    double lo, hi;
    const wchar_t* unit;
};
const StepField kSteps[] = {
    {kSetSeekSmall, L"Small seek", 0.05, 600, L"seconds"},
    {kSetSeekLarge, L"Large seek", 0.1, 3600, L"seconds"},
    {kSetFrameJump, L"Frame jump", 1, 1000, L"frames"},
    {kSetNudge, L"Nudge", 1, 1000, L"frames"},
};

struct SettingsDlg : Dialog {
    EditSettings* s;

    static std::vector<uint8_t> Layout() {
        Form f(340, {L"Output folder", L"File name", L"Merged name", L"Snapshots", L"Small seek (s)",
                     L"Frame jump (frames)", L"Timeline", L"Segments", L"Export"});
        f.Folder(kSetDir, kSetBrowse, L"Output folder");
        f.Edit(kSetName, L"File name");
        f.Edit(kSetMergeName, L"Merged name");
        f.Gap(-3);
        f.Hint(L"Tokens: {name} {n} {label} {start} {end}");
        f.Combo(kSetSnap, L"Snapshots");
        int label2W = std::max(f.M().Width(L"Large seek (s)"), f.M().Width(L"Nudge (frames)"));
        f.Pair(kSetSeekSmall, L"Small seek (s)", kSetSeekLarge, L"Large seek (s)", 40, label2W);
        f.Pair(kSetFrameJump, L"Frame jump (frames)", kSetNudge, L"Nudge (frames)", 40, label2W);
        f.Gap(3);
        f.CheckRow({{kSetThumbs, L"Thumbnails"}, {kSetWave, L"Waveform"}, {kSetKeyframes, L"Keyframe marks"}},
                   L"Timeline");
        f.Gap(3);
        f.Check(kSetAutoSave, L"Remember segments per file", L"Segments");
        f.Gap(3);
        f.Check(kSetConfirm, L"Show export options before exporting", L"Export");
        f.Check(kSetVerify, L"Check outputs after export (duration, tracks)");
        f.CheckRow({{kSetMeta, L"Preserve metadata"}, {kSetChapters, L"Preserve chapters"}});
        f.Check(kSetSegChapters, L"Create chapters from segments when merging");
        f.Check(kSetNegTs, L"Shift timestamps to start at zero");
        f.Check(kSetFastStart, L"MP4/MOV: move index to front (faststart)");
        f.Check(kSetDiscord, L"Discord-ready: re-encode HEVC/AV1/10-bit video to H.264 (H.264 stays lossless)");
        f.Gap(4);
        f.Hint(L"Keyboard and mouse shortcuts are set in fastplay.ini, section [keys] (Options opens it).", true);
        return f.Finish(L"Settings");
    }

    bool Init(HWND d) override {
        SetText(d, kSetDir, s->outDir);
        Cue(d, kSetDir, L"(same folder as input)");
        SetText(d, kSetName, s->nameTemplate);
        SetText(d, kSetMergeName, s->mergeTemplate);
        FillCombo(d, kSetSnap, {L"JPEG", L"PNG"}, s->snapshotFormat == L"png" ? 1 : 0);
        SetText(d, kSetSeekSmall, Num(s->seekSmall));
        SetText(d, kSetSeekLarge, Num(s->seekLarge));
        SetText(d, kSetFrameJump, std::to_wstring(s->frameJump));
        SetText(d, kSetNudge, std::to_wstring(s->nudgeFrames));
        Check(d, kSetThumbs, s->showThumbs);
        Check(d, kSetWave, s->showWave);
        Check(d, kSetKeyframes, s->showKeyframes);
        Check(d, kSetAutoSave, s->autoSaveProject);
        Check(d, kSetConfirm, s->confirmExport);
        Check(d, kSetVerify, s->verifyOutputs);
        Check(d, kSetMeta, s->preserveMetadata);
        Check(d, kSetChapters, s->preserveChapters);
        Check(d, kSetSegChapters, s->chaptersFromSegments);
        Check(d, kSetNegTs, s->avoidNegativeTs);
        Check(d, kSetFastStart, s->fastStart);
        Check(d, kSetDiscord, s->discordReady);
        return true;
    }

    bool Command(HWND d, int id, int code) override {
        if (id == kSetBrowse && code == BN_CLICKED) {
            Browse(d, kSetDir);
            return true;
        }
        return false;
    }

    bool Accept(HWND d, std::wstring* error, int* bad) override {
        double v[4] = {};
        for (int i = 0; i < 4; ++i) {
            const StepField& f = kSteps[i];
            std::wstring t = GetText(d, f.id);
            bool whole = f.id == kSetFrameJump || f.id == kSetNudge;
            int n = 0;
            bool ok = whole ? ParseInt(t, &n) : ParseNumber(t, &v[i]);
            if (whole) v[i] = n;
            if (!ok || v[i] < f.lo || v[i] > f.hi) {
                *error = std::wstring(f.name) + L" must be " + (whole ? L"a whole number" : L"a number") + L" from " +
                         Num(f.lo) + L" to " + Num(f.hi) + L" " + f.unit + L".";
                *bad = f.id;
                return false;
            }
        }
        s->outDir = Trim(GetText(d, kSetDir));
        std::wstring name = Trim(GetText(d, kSetName)), merged = Trim(GetText(d, kSetMergeName));
        if (!name.empty()) s->nameTemplate = name;
        if (!merged.empty()) s->mergeTemplate = merged;
        s->snapshotFormat = ComboIndex(d, kSetSnap) == 1 ? L"png" : L"jpg";
        s->seekSmall = v[0];
        s->seekLarge = v[1];
        s->frameJump = (int)v[2];
        s->nudgeFrames = (int)v[3];
        s->showThumbs = Checked(d, kSetThumbs);
        s->showWave = Checked(d, kSetWave);
        s->showKeyframes = Checked(d, kSetKeyframes);
        s->autoSaveProject = Checked(d, kSetAutoSave);
        s->confirmExport = Checked(d, kSetConfirm);
        s->verifyOutputs = Checked(d, kSetVerify);
        s->preserveMetadata = Checked(d, kSetMeta);
        s->preserveChapters = Checked(d, kSetChapters);
        s->chaptersFromSegments = Checked(d, kSetSegChapters);
        s->avoidNegativeTs = Checked(d, kSetNegTs);
        s->fastStart = Checked(d, kSetFastStart);
        s->discordReady = Checked(d, kSetDiscord);
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// BatchDialog

struct BatchDlg : Dialog {
    EditSettings* s;

    static std::vector<uint8_t> Layout(int files, int currentSegments) {
        Form f(310, {L"Remove from start (s)", L"Remove from end (s)"});
        std::wstring same = L"Use the current file's " + Plural(currentSegments, L"included segment") + L" on every file";
        f.Radio(kBatSame, same.c_str(), true);
        f.Radio(kBatSaved, L"Use each file's own saved segments (whole file if it has none)", false);
        f.Radio(kBatTrim, L"Trim every file:", false);
        f.Gap(3);
        f.Edit(kBatTrimStart, L"Remove from start (s)", 60);
        f.Edit(kBatTrimEnd, L"Remove from end (s)", 60);
        f.Hint(L"Format, cut mode and merge follow the export bar.");
        return f.Finish(L"Export " + Plural(files, L"file"));
    }

    bool Init(HWND d) override {
        int sel = s->batchMode == L"trim" ? kBatTrim : s->batchMode == L"saved" ? kBatSaved : kBatSame;
        CheckRadioButton(d, kBatSame, kBatTrim, sel);
        SetText(d, kBatTrimStart, Num(s->batchTrimStart));
        SetText(d, kBatTrimEnd, Num(s->batchTrimEnd));
        return true;
    }

    bool Accept(HWND d, std::wstring* error, int* bad) override {
        double a = 0, b = 0;
        bool okA = TryParseTime(Trim(GetText(d, kBatTrimStart)), &a) && a >= 0;
        bool okB = TryParseTime(Trim(GetText(d, kBatTrimEnd)), &b) && b >= 0;
        if (!okA || !okB) {
            *error = L"Trim amounts must be times like 5, 1:30 or 0.5.";
            *bad = okA ? kBatTrimEnd : kBatTrimStart;
            return false;
        }
        s->batchMode = Checked(d, kBatTrim) ? L"trim" : Checked(d, kBatSaved) ? L"saved" : L"same";
        s->batchTrimStart = a;
        s->batchTrimEnd = b;
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// PromptDialog (one or two fields)

struct PromptDlg : Dialog {
    std::wstring* v1;
    std::wstring* v2 = nullptr;

    static std::vector<uint8_t> Layout(const wchar_t* title, const wchar_t* label1, const wchar_t* label2) {
        Form f(270, {});
        f.Text(label1 ? label1 : L"");
        f.WideEdit(kPrompt1);
        if (label2) {
            f.Gap(2);
            f.Text(label2);
            f.WideEdit(kPrompt2);
        }
        return f.Finish(title ? title : L"");
    }

    bool Init(HWND d) override {
        SetText(d, kPrompt1, *v1);
        if (v2) SetText(d, kPrompt2, *v2);
        Focus(d, kPrompt1);
        return false;
    }

    bool Accept(HWND d, std::wstring*, int*) override {
        *v1 = GetText(d, kPrompt1);
        if (v2) *v2 = GetText(d, kPrompt2);
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// TextViewer

constexpr UINT_PTR kTimerLive = 1, kTimerCopied = 2;
constexpr UINT kMsgRefont = WM_APP + 1;

struct ViewerDlg : Dialog {
    std::wstring text;  // what the box shows (as given, LF or CRLF)
    std::function<std::wstring()> live;
    HFONT mono = nullptr;
    SIZE minTrack = {};

    static std::vector<uint8_t> Layout(const wchar_t* title) {
        Template t;
        t.Add(ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_BORDER |
                  WS_TABSTOP | WS_GROUP,
              kMargin, kMargin, 486, 265, kViewEdit, kClsEdit);
        t.Add(BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 389, 279, 50, 14, kViewCopy, kClsButton, L"Copy all");
        t.Add(BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 443, 279, 50, 14, IDOK, kClsButton, L"Close");
        return t.Build(title ? title : L"", 500, 300, WS_THICKFRAME | WS_MAXIMIZEBOX);
    }

    void SetFont(HWND d) {
        HFONT old = mono;
        mono = CreateFontW(-MulDiv(9, (int)GetDpiForWindow(d), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendDlgItemMessageW(d, kViewEdit, WM_SETFONT, (WPARAM)mono, TRUE);
        if (old) DeleteObject(old);
    }

    void Arrange(HWND d) {
        RECT u = {kMargin, 4, 50, 14};  // margin, gap, button width, button height in dialog units
        MapDialogRect(d, &u);
        int m = u.left, gap = u.top, bw = u.right, bh = u.bottom;
        RECT c;
        GetClientRect(d, &c);
        int by = c.bottom - m - bh;
        MoveWindow(GetDlgItem(d, kViewEdit), m, m, std::max(0, (int)c.right - 2 * m), std::max(0, by - gap - m), TRUE);
        MoveWindow(GetDlgItem(d, IDOK), c.right - m - bw, by, bw, bh, TRUE);
        MoveWindow(GetDlgItem(d, kViewCopy), c.right - m - 2 * bw - gap, by, bw, bh, TRUE);
    }

    void CaretToEnd(HWND e) {
        int n = GetWindowTextLengthW(e);
        SendMessageW(e, EM_SETSEL, n, n);
        SendMessageW(e, EM_SCROLLCARET, 0, 0);
        SendMessageW(e, WM_VSCROLL, SB_BOTTOM, 0);  // EM_SCROLLCARET alone does nothing while the box is hidden
    }

    bool Init(HWND d) override {
        HWND e = GetDlgItem(d, kViewEdit);
        SendMessageW(e, EM_SETLIMITTEXT, 0, 0);
        SetFont(d);
        SetWindowTextW(e, ToCrlf(text).c_str());

        // Minimum size: a quarter of the designed size; and never larger than the monitor's work area.
        RECT w, mn = {0, 0, 200, 100};
        GetWindowRect(d, &w);
        RECT cl;
        GetClientRect(d, &cl);
        MapDialogRect(d, &mn);
        minTrack = {mn.right + (w.right - w.left - cl.right), mn.bottom + (w.bottom - w.top - cl.bottom)};
        MONITORINFO mi = {sizeof mi};
        if (GetMonitorInfoW(MonitorFromWindow(d, MONITOR_DEFAULTTONEAREST), &mi)) {
            const RECT& wa = mi.rcWork;
            int ww = std::min(w.right - w.left, wa.right - wa.left), wh = std::min(w.bottom - w.top, wa.bottom - wa.top);
            if (ww != w.right - w.left || wh != w.bottom - w.top)
                SetWindowPos(d, nullptr, wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2,
                             ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Arrange(d);
        if (live) SetTimer(d, kTimerLive, 1000, nullptr);
        if (!detail::g_testNoFocus) SetFocus(e);
        CaretToEnd(e);
        return false;
    }

    // Live log: only touch the box when the text changed; follow the end unless the user moved away from it.
    void Refresh(HWND d) {
        std::wstring t = live();
        if (t == text) return;
        text = std::move(t);
        HWND e = GetDlgItem(d, kViewEdit);
        DWORD a = 0, b = 0;
        SendMessageW(e, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        bool follow = b >= (DWORD)GetWindowTextLengthW(e);
        int first = (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(e, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(e, ToCrlf(text).c_str());
        if (follow) {
            CaretToEnd(e);
        } else {
            SendMessageW(e, EM_SETSEL, a, b);
            SendMessageW(e, EM_LINESCROLL, 0, first - (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0));
        }
        SendMessageW(e, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(e, nullptr, TRUE);
    }

    void CopyAll(HWND d) {
        std::wstring t = GetText(d, kViewEdit);
        if (!OpenClipboard(d)) return;
        EmptyClipboard();
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
            memcpy(GlobalLock(g), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        CloseClipboard();
        SetDlgItemTextW(d, kViewCopy, L"Copied");
        SetTimer(d, kTimerCopied, 1200, nullptr);
    }

    bool Command(HWND d, int id, int code) override {
        if (id == kViewCopy && code == BN_CLICKED) {
            CopyAll(d);
            return true;
        }
        return false;
    }

    INT_PTR Message(HWND d, UINT m, WPARAM w, LPARAM l, bool* handled) override {
        switch (m) {
        case WM_SIZE:
            Arrange(d);
            break;
        case WM_GETMINMAXINFO:
            if (minTrack.cx) {
                auto* mm = (MINMAXINFO*)l;
                mm->ptMinTrackSize = {minTrack.cx, minTrack.cy};
                *handled = true;
            }
            break;
        case WM_TIMER:
            if (w == kTimerLive) Refresh(d);
            if (w == kTimerCopied) {
                KillTimer(d, kTimerCopied);
                SetDlgItemTextW(d, kViewCopy, L"Copy all");
            }
            *handled = true;
            break;
        case WM_DPICHANGED:
            PostMessageW(d, kMsgRefont, 0, 0);  // after the dialog manager has rescaled and re-set fonts
            break;
        case kMsgRefont:
            SetFont(d);
            Arrange(d);
            *handled = true;
            break;
        case WM_DESTROY:
            KillTimer(d, kTimerLive);
            KillTimer(d, kTimerCopied);
            break;
        }
        return 0;
    }

    void Destroyed() override {
        if (mono) DeleteObject(mono);
        mono = nullptr;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Public API

bool EditSegment(HWND owner, Segment& seg, const std::function<std::wstring(double)>& fmt,
                 const std::function<bool(const std::wstring&, double*)>& parse) {
    SegmentDlg dlg;
    dlg.seg = &seg;
    dlg.fmt = fmt ? fmt : [](double v) { return Fmt(v); };
    dlg.parse = parse ? parse : [](const std::wstring& t, double* v) { return TryParseTime(t, v); };
    return RunModal(owner, SegmentDlg::Layout(), dlg);
}

bool ExportOptions(HWND owner, EditSettings& s, int segCount, const std::wstring& inputExt, double* speed) {
    ExportDlg dlg;
    dlg.s = &s;
    dlg.speed = speed;
    dlg.inputExt = inputExt;
    return RunModal(owner, ExportDlg::Layout(segCount), dlg);
}

bool Settings(HWND owner, EditSettings& s) {
    SettingsDlg dlg;
    dlg.s = &s;
    return RunModal(owner, SettingsDlg::Layout(), dlg);
}

bool Batch(HWND owner, EditSettings& s, int files, int currentSegments) {
    BatchDlg dlg;
    dlg.s = &s;
    return RunModal(owner, BatchDlg::Layout(files, currentSegments), dlg);
}

bool Prompt(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring* value) {
    PromptDlg dlg;
    dlg.v1 = value;
    return RunModal(owner, PromptDlg::Layout(title, label, nullptr), dlg);
}

bool Prompt2(HWND owner, const wchar_t* title, const wchar_t* label1, std::wstring* value1, const wchar_t* label2,
             std::wstring* value2) {
    PromptDlg dlg;
    dlg.v1 = value1;
    dlg.v2 = value2;
    return RunModal(owner, PromptDlg::Layout(title, label1, label2 ? label2 : L""), dlg);
}

void TextViewer(HWND owner, const wchar_t* title, const std::wstring& text, std::function<std::wstring()> liveSource) {
    ViewerDlg dlg;
    dlg.text = text;
    dlg.live = std::move(liveSource);
    RunModal(owner, ViewerDlg::Layout(title), dlg);
}

std::wstring PickFolder(HWND owner, const std::wstring& initial) {
    // COM may not be initialised on this thread yet (FastPlay only needs it here); balance our own init.
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring out;
    IFileOpenDialog* fd = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) {
        DWORD opts = 0;
        fd->GetOptions(&opts);
        fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (!initial.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                fd->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(fd->Show(owner))) {
            IShellItem* result = nullptr;
            if (SUCCEEDED(fd->GetResult(&result))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = path;
                    CoTaskMemFree(path);
                }
                result->Release();
            }
        }
        fd->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Test hooks: declared only in tools/edit-tests/test_dialogs.cpp.

namespace detail {

// Creates dialog `which` (0 segment, 1 export, 2 settings, 3 batch, 4 prompt, 5 prompt2, 6 text viewer) modeless
// and hidden, bound to the given objects. Destroy it with DestroyWindow.
HWND CreateForTest(int which, HWND owner, Segment* seg, EditSettings* s, double* speed, std::wstring* v1,
                   std::wstring* v2) {
    Dialog* dlg = nullptr;
    std::vector<uint8_t> t;
    switch (which) {
    case 0: {
        auto* x = new SegmentDlg;
        x->seg = seg;
        x->fmt = [](double v) { return Fmt(v); };
        x->parse = [](const std::wstring& txt, double* v) { return TryParseTime(txt, v); };
        dlg = x;
        t = SegmentDlg::Layout();
        break;
    }
    case 1: {
        auto* x = new ExportDlg;
        x->s = s;
        x->speed = speed;
        x->inputExt = L".mp4";
        dlg = x;
        t = ExportDlg::Layout(3);
        break;
    }
    case 2: {
        auto* x = new SettingsDlg;
        x->s = s;
        dlg = x;
        t = SettingsDlg::Layout();
        break;
    }
    case 3: {
        auto* x = new BatchDlg;
        x->s = s;
        dlg = x;
        t = BatchDlg::Layout(5, 2);
        break;
    }
    case 4: {
        auto* x = new PromptDlg;
        x->v1 = v1;
        dlg = x;
        t = PromptDlg::Layout(L"Snapshots", L"Interval in seconds (within the selected segment, or the whole file)",
                              nullptr);
        break;
    }
    case 5: {
        auto* x = new PromptDlg;
        x->v1 = v1;
        x->v2 = v2;
        dlg = x;
        t = PromptDlg::Layout(L"Detect silence", L"Noise level (dB)", L"Minimum duration (s)");
        break;
    }
    case 6: {
        auto* x = new ViewerDlg;
        x->text = v1 ? *v1 : L"";
        if (v2) x->live = [v2] { return *v2; };
        dlg = x;
        t = ViewerDlg::Layout(L"FFmpeg log");
        break;
    }
    default:
        return nullptr;
    }
    dlg->modal = false;
    HWND h = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), (const DLGTEMPLATE*)t.data(), owner, Proc,
                                        (LPARAM)dlg);
    if (!h) delete dlg;
    return h;
}

// Presses OK on a test dialog. True when it accepted (outputs written); otherwise *error has the message.
bool OkForTest(HWND dlg, std::wstring* error) {
    g_lastError.clear();
    bool quiet = g_testQuiet;
    g_testQuiet = true;
    SendMessageW(dlg, WM_COMMAND, IDOK, 0);
    g_testQuiet = quiet;
    auto* d = (Dialog*)GetWindowLongPtrW(dlg, DWLP_USER);
    if (error) *error = g_lastError;
    return d && d->result == IDOK;
}

}  // namespace detail

}  // namespace edit::dialogs
