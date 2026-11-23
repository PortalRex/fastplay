// Owner-drawn strip of flat controls (FastCut's FlatButton/Slider/ProgressStrip look) for Edit mode's menu strip,
// control row, export bar, tab strip and icon bars. One HWND per strip; items are just rectangles, laid out by the
// owner. Child windows (EDIT, COMBOBOX) may live inside a strip; it answers their WM_CTLCOLOR* in the dark theme.
#pragma once
#include "common.h"

namespace edit {

struct PanelItem {
    enum Kind { Button, Label, Slider, Progress } kind = Button;
    RECT rc = {};
    std::wstring glyph;          // Segoe MDL2 Assets code point(s), drawn left of the text
    std::wstring text;
    std::wstring tip;            // tooltip (shown after a short hover)
    uint32_t fill = 0xFFFFFFFF;  // background (COLORREF); 0xFFFFFFFF = transparent until hovered
    uint32_t color = 0xFFFFFFFF; // text color; 0xFFFFFFFF = theme text
    bool round = false;          // circle (play button)
    bool largeIcon = false;
    bool bold = false;
    bool mono = false;           // Consolas (time labels)
    bool checked = false;        // toggles: underline accent
    bool enabled = true;
    bool visible = true;
    bool dropdown = false;       // shows a small ▾
    bool alignRight = false;     // labels
    double value = 0;            // Slider 0..max, Progress 0..1 (< 0 = indeterminate)
    double max = 1;
    std::function<void()> click;                 // buttons
    std::function<void(double)> changed;         // sliders (value while dragging)
};

class Panel {
public:
    bool Create(HINSTANCE inst, HWND parent, uint32_t background);
    HWND Hwnd() const { return hwnd_; }
    void SetDpi(int dpi);
    int S(int v) const { return MulDiv(v, dpi_, 96); }

    std::vector<PanelItem> items;
    std::function<void(Panel&, const RECT& client)> layout;  // positions items (and child windows)
    std::function<void(Panel&, HDC, const RECT& client)> paintExtra;  // under the items (e.g. the transport pill)
    std::function<void(POINT screen)> rightClick;

    PanelItem& Add(PanelItem it) { items.push_back(std::move(it)); return items.back(); }
    void Relayout();   // runs layout and repaints
    void Invalidate();
    int TextWidth(const std::wstring& glyph, const std::wstring& text, bool bold = false, bool largeIcon = false) const;
    int ButtonWidth(const PanelItem& it) const;  // natural width of a button's contents + padding
    RECT ItemScreenRect(const PanelItem& it) const;

    ~Panel();

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT On(UINT m, WPARAM w, LPARAM l);
    void Paint(HDC dc, const RECT& rc);
    int HitTest(POINT p) const;
    void UpdateTip(int item);
    HWND hwnd_ = nullptr, tip_ = nullptr;
    int dpi_ = 96;
    uint32_t bg_ = 0;
    HBRUSH editBrush_ = nullptr;
    int hot_ = -1, pressed_ = -1, tipItem_ = -1;
    bool sliderDrag_ = false;
};

// Segoe MDL2 Assets glyphs used by FastCut (Theme.cs).
namespace glyph {
inline const wchar_t Play[] = L"", Pause[] = L"", ToStart[] = L"", ToEnd[] = L"",
                     Prev[] = L"", Next[] = L"", Cut[] = L"", Camera[] = L"",
                     Volume[] = L"", Mute[] = L"", Plus[] = L"", Trash[] = L"",
                     Up[] = L"", Down[] = L"", Tag[] = L"", Sort[] = L"",
                     Folder[] = L"", Swap[] = L"", Check[] = L"", Video[] = L"",
                     List[] = L"", Rotate[] = L"", Undo[] = L"", Redo[] = L"",
                     Warning[] = L"", ErrorBadge[] = L"", Info[] = L"";
}
HFONT IconFont(int dpi, bool large = false);  // cached per DPI

// Dark popup menus / scrollbars for this process (uxtheme's undocumented SetPreferredAppMode; no-op if absent).
void EnableDarkMenus();
void DarkTitleBar(HWND h, bool on);

}  // namespace edit
