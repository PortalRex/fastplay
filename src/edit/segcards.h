// SegmentCards (Theme.cs): the Segments tab's scrollable list of segment cards (color bar, number, label, start →
// end, duration, tags, include checkbox, warning line), owner-drawn with GDI, virtualized for long lists, cards
// draggable to reorder. Plus small shared dark-theme painting helpers for edit mode.
#pragma once
#include "common.h"
#include <set>

namespace edit {

struct SegmentCardsCallbacks {
    std::function<void(int index, uint32_t mods)> itemClicked;  // mods: MK_CONTROL | MK_SHIFT
    std::function<void(int index)> toggleRequested;               // include checkbox
    std::function<void(int index)> editRequested;                 // double-click
    std::function<void(int from, int to)> reorder;                // drag-and-drop
    std::function<void(POINT screen, int index)> contextMenu;     // right-click (index may be -1)
};

class SegmentCards {
public:
    bool Create(HINSTANCE inst, HWND parent, SegmentCardsCallbacks cb);
    HWND Hwnd() const { return hwnd_; }
    void SetDpi(int dpi);
    void Refresh();                // segments changed (count/contents)
    void EnsureVisible(int index);

    // Owned by the editor; read on paint.
    const std::vector<Segment>* segments = nullptr;
    const std::set<int>* marked = nullptr;
    int current = -1;
    std::function<std::wstring(double)> fmt;                // time text
    std::function<std::wstring(int index)> warning;         // e.g. "starts 0.37 s early" ("" = none)

    SegmentCards();   // defined in segcards.cpp (= default) so Impl stays private
    ~SegmentCards();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    HWND hwnd_ = nullptr;
    friend struct Impl;
};

// Edit-mode colors (Theme.cs, exact FastCut values), COLORREF (0x00BBGGRR).
namespace theme {
constexpr uint32_t Bg = 0x001A1818, Panel = 0x00242121, Card = 0x00302C2C, CardHover = 0x00393434,
                   CardSelected = 0x00423A3A, Border = 0x00443E3E, Text = 0x00ECE8E8, Dim = 0x009E9696,
                   Faint = 0x00665F5F, Accent = 0x00FF8438, Export = 0x0062A022, Warn = 0x003CB4F5,
                   Success = 0x007DC846, Error = 0x005F5FF0;
HFONT UiFont(int dpi, bool bold = false);   // cached per DPI; do not delete
HFONT MonoFont(int dpi, bool large = false);
}  // namespace theme

}  // namespace edit
