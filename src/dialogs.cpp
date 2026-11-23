#include "dialogs.h"
#include <commdlg.h>

namespace {

// Builds a DLGTEMPLATE + items in memory (DWORD-aligned items, WORD-aligned strings).
class DlgBuilder {
public:
    void Begin(const wchar_t* title, int w, int h, int items) {
        Align(4);
        DLGTEMPLATE t = {};
        t.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        t.cdit = (WORD)items;
        t.cx = (short)w;
        t.cy = (short)h;
        Put(&t, sizeof t);
        Word(0);  // menu
        Word(0);  // class
        Str(title);
        Word(9);  // font size
        Str(L"Segoe UI");
    }
    void Item(DWORD style, int x, int y, int w, int h, WORD id, WORD cls, const wchar_t* text) {
        Align(4);
        DLGITEMTEMPLATE it = {};
        it.style = style | WS_CHILD | WS_VISIBLE;
        it.x = (short)x;
        it.y = (short)y;
        it.cx = (short)w;
        it.cy = (short)h;
        it.id = id;
        Put(&it, sizeof it);
        Word(0xFFFF);
        Word(cls);  // 0x80 button, 0x81 edit, 0x82 static
        Str(text);
        Word(0);  // no creation data
    }
    const DLGTEMPLATE* Get() const { return (const DLGTEMPLATE*)buf_.data(); }

private:
    void Put(const void* p, size_t n) { buf_.insert(buf_.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void Word(WORD w) { Put(&w, 2); }
    void Str(const wchar_t* s) { Put(s, (wcslen(s) + 1) * 2); }
    void Align(size_t a) { while (buf_.size() % a) buf_.push_back(0); }
    std::vector<uint8_t> buf_;
};

struct InputState {
    const wchar_t* prompt;
    std::wstring* value;
};

INT_PTR CALLBACK InputProc(HWND d, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITDIALOG: {
        auto* st = (InputState*)l;
        SetWindowLongPtrW(d, DWLP_USER, l);
        SetDlgItemTextW(d, 100, st->prompt);
        SetDlgItemTextW(d, 101, st->value->c_str());
        HWND e = GetDlgItem(d, 101);
        SendMessageW(e, EM_SETSEL, 0, -1);
        SetFocus(e);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            auto* st = (InputState*)GetWindowLongPtrW(d, DWLP_USER);
            wchar_t buf[512];
            GetDlgItemTextW(d, 101, buf, 512);
            *st->value = buf;
            EndDialog(d, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

}  // namespace

bool InputBox(HWND owner, const wchar_t* title, const wchar_t* prompt, std::wstring* value) {
    DlgBuilder b;
    b.Begin(title, 220, 62, 4);
    b.Item(SS_LEFT, 7, 7, 206, 10, 100, 0x82, L"");
    b.Item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 7, 20, 206, 13, 101, 0x81, L"");
    b.Item(BS_DEFPUSHBUTTON | WS_TABSTOP, 109, 41, 50, 14, IDOK, 0x80, L"OK");
    b.Item(BS_PUSHBUTTON | WS_TABSTOP, 163, 41, 50, 14, IDCANCEL, 0x80, L"Cancel");
    InputState st = {prompt, value};
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), b.Get(), owner, InputProc, (LPARAM)&st) == IDOK;
}

static const wchar_t kMediaFilter[] =
    L"Media files\0*.mp4;*.m4v;*.mkv;*.webm;*.mov;*.avi;*.wmv;*.flv;*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.vob;*.ogv;*.3gp;"
    L"*.mp3;*.flac;*.wav;*.m4a;*.aac;*.ogg;*.opus;*.wma;*.mka\0All files\0*.*\0";

std::wstring PickOpenFile(HWND owner, const std::wstring& initialDir) {
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW o = {sizeof o};
    o.hwndOwner = owner;
    o.lpstrFilter = kMediaFilter;
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)std::size(file);
    o.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o) ? file : L"";
}

std::wstring PickSaveFile(HWND owner, const std::wstring& initialDir, const std::wstring& name, const wchar_t* filter,
                          const wchar_t* defExt) {
    wchar_t file[MAX_PATH * 4] = {};
    wcsncpy_s(file, name.c_str(), _TRUNCATE);
    OPENFILENAMEW o = {sizeof o};
    o.hwndOwner = owner;
    o.lpstrFilter = filter;
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)std::size(file);
    o.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    o.lpstrDefExt = defExt;
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&o) ? file : L"";
}

std::vector<std::wstring> PickOpenFiles(HWND owner, const std::wstring& initialDir) {
    std::vector<wchar_t> buf(64 * 1024, 0);
    OPENFILENAMEW o = {sizeof o};
    o.hwndOwner = owner;
    o.lpstrFilter = kMediaFilter;
    o.lpstrFile = buf.data();
    o.nMaxFile = (DWORD)buf.size();
    o.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT;
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&o)) return out;
    // One file: full path. Several: folder, then names, each NUL-terminated, list ends with an empty string.
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) {
        out.push_back(first);
        return out;
    }
    for (; *p; p += wcslen(p) + 1) out.push_back(first + L"\\" + p);
    return out;
}

namespace {
INT_PTR CALLBACK TextProc(HWND d, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemTextW(d, 101, (const wchar_t*)l);
        SendDlgItemMessageW(d, 101, EM_SETSEL, 0, 0);
        SetFocus(GetDlgItem(d, IDOK));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            EndDialog(d, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
}  // namespace

void ShowTextBox(HWND owner, const wchar_t* title, const std::wstring& text) {
    DlgBuilder b;
    b.Begin(title, 340, 280, 2);
    b.Item(ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 7, 7, 326, 247, 101, 0x81, L"");
    b.Item(BS_DEFPUSHBUTTON | WS_TABSTOP, 283, 259, 50, 14, IDOK, 0x80, L"OK");
    // Edit controls want CRLF line breaks.
    std::wstring crlf;
    for (wchar_t c : text) {
        if (c == L'\n') crlf += L'\r';
        crlf += c;
    }
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), b.Get(), owner, TextProc, (LPARAM)crlf.c_str());
}
