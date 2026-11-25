// Timeline markers (Steam game recording style): M drops one at the playhead, Shift+PageDown / Shift+PageUp jump
// between them (and the file's chapters), PageDown / PageUp step through them before going to the next file.
// Labels are optional; right-click a marker on the seekbar to label or delete it.
#include "app.h"
#include "controls.h"
#include "dialogs.h"
#include "edit/editor.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr double kSameSpot = 0.25;    // a new marker this close to an existing one is the same marker
constexpr double kAtPlayhead = 1.5;   // "the marker at the playhead" for delete / label
}  // namespace

double App::CurrentPos() {
    if (EditActive()) return editor_->Position();
    if (SeekHeld()) return seekHoldPos_;
    return mpvx::GetDouble(mpv_, "time-pos", bar_.pos);
}

void App::SeekToTime(double t) {
    if (EditActive()) editor_->JumpTo(t);
    else SeekTo(t, true);
}

void App::MarkersChanged() {
    RefreshBar();
    if (editor_) editor_->MarkersChanged();
}

void App::GoToMarker(int i) {
    const auto& ms = markers_.Items();
    if (i < 0 || i >= (int)ms.size()) return;
    SeekToTime(ms[i].t);
    mpvx::ShowText(mpv_, markers_.Describe(i), 2000);
}

bool App::JumpMarker(int dir, bool quietAtEnd) {
    // Markers and chapters together, in time order (a marker wins over a chapter at the same spot).
    struct Point { double t; int marker; int chapter; };
    std::vector<Point> pts;
    const auto& ms = markers_.Items();
    for (int i = 0; i < (int)ms.size(); i++) pts.push_back({ms[i].t, i, -1});
    for (int i = 0; i < (int)bar_.chapters.size(); i++) {
        double t = bar_.chapters[i];
        bool dup = std::any_of(ms.begin(), ms.end(), [&](const Marker& m) { return std::fabs(m.t - t) < 0.1; });
        if (!dup) pts.push_back({t, -1, i});
    }
    std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.t < b.t; });

    double pos = CurrentPos();
    // Going back while playing: a point passed less than a second ago counts as "here", so the key goes to the one
    // before it (like a music player's previous-track button).
    double grace = bar_.paused || EditActive() ? 0.1 : 1.0;
    const Point* hit = nullptr;
    if (dir > 0) {
        for (const Point& p : pts)
            if (p.t > pos + 0.1) { hit = &p; break; }
    } else {
        for (auto it = pts.rbegin(); it != pts.rend(); ++it)
            if (it->t < pos - grace) { hit = &*it; break; }
    }
    if (!hit) {
        if (!quietAtEnd) mpvx::ShowText(mpv_, pts.empty() ? L"No markers (M adds one)" : dir > 0 ? L"No more markers" : L"No earlier markers", 1500);
        return false;
    }
    if (hit->marker >= 0) {
        GoToMarker(hit->marker);
    } else {
        SeekToTime(hit->t);
        mpvx::ShowText(mpv_, L"Chapter " + std::to_wstring(hit->chapter + 1) + L" (" + util::FormatTime(hit->t) + L")", 2000);
    }
    return true;
}

void App::RenameMarker(int i) {
    const auto& ms = markers_.Items();
    if (i < 0 || i >= (int)ms.size()) return;
    std::wstring v = ms[i].label;
    std::wstring prompt = L"Label for the marker at " + util::FormatTime(ms[i].t) + L" (empty for none):";
    if (!InputBox(hwnd_, L"Marker label", prompt.c_str(), &v)) return;
    markers_.Rename(i, v);
    MarkersChanged();
    mpvx::ShowText(mpv_, markers_.Describe(i), 2000);
}

void App::DeleteMarker(int i) {
    if (i < 0 || i >= markers_.Count()) return;
    std::wstring what = markers_.Describe(i);
    markers_.Remove(i);
    MarkersChanged();
    mpvx::ShowText(mpv_, L"Deleted " + what, 2000);
}

void App::ClearMarkers() {
    if (markers_.Empty()) return;
    std::wstring q = L"Delete all " + std::to_wstring(markers_.Count()) + L" markers of this file?";
    if (!util::TestMode() && MessageBoxW(hwnd_, q.c_str(), L"FastPlay", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    markers_.RemoveAll();
    MarkersChanged();
    mpvx::ShowText(mpv_, L"All markers deleted", 2000);
}

bool App::MarkerAction(Act a) {
    switch (a) {
    case Act::AddMarker: {
        if (!bar_.hasFile) return true;
        int existing = -1;
        int i = markers_.Add(CurrentPos(), kSameSpot, &existing);
        if (i < 0) {
            mpvx::ShowText(mpv_, L"Already marked: " + markers_.Describe(existing), 2000);
            return true;
        }
        MarkersChanged();
        std::wstring k = keys_.ShortcutText(Act::RenameMarker);
        mpvx::ShowText(mpv_, L"Added " + markers_.Describe(i) + (k.empty() ? L"" : L"   " + k + L" labels it"), 2500);
        util::TimingLog(("marker added at " + std::to_string(markers_.Items()[i].t)).c_str());
        return true;
    }
    case Act::DeleteMarker:
    case Act::RenameMarker: {
        int i = markers_.Nearest(CurrentPos(), kAtPlayhead);
        if (i < 0) {
            mpvx::ShowText(mpv_, markers_.Empty() ? L"No markers (M adds one)" : L"No marker at the playhead", 1500);
            return true;
        }
        if (a == Act::DeleteMarker) DeleteMarker(i);
        else RenameMarker(i);
        return true;
    }
    case Act::NextMarker: JumpMarker(1, false); return true;
    case Act::PrevMarker: JumpMarker(-1, false); return true;
    case Act::ClearMarkers: ClearMarkers(); return true;
    default: return false;
    }
}

void App::BuildMarkerMenu(HMENU m) {
    bool f = bar_.hasFile && mpv_;
    bool any = !markers_.Empty();
    auto item = [&](Act a, bool enabled) {
        std::wstring t = FindAction(a)->label;
        std::wstring k = keys_.ShortcutText(a);
        if (!k.empty()) t += L"\t" + k;
        AppendMenuW(m, MF_STRING | (enabled ? 0 : MF_GRAYED), kCmdAction + (UINT)a, t.c_str());
    };
    item(Act::AddMarker, f);
    item(Act::PrevMarker, f && (any || !bar_.chapters.empty()));
    item(Act::NextMarker, f && (any || !bar_.chapters.empty()));
    item(Act::RenameMarker, f && any);
    item(Act::DeleteMarker, f && any);
    item(Act::ClearMarkers, f && any);
    if (!any) return;
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const auto& ms = markers_.Items();
    double pos = bar_.pos;
    int cur = markers_.Nearest(pos, kAtPlayhead);
    for (int i = 0; i < (int)ms.size() && i < 500; i++) {
        std::wstring t = std::to_wstring(i + 1) + L"   " + util::FormatTime(ms[i].t);
        if (!ms[i].label.empty()) t += L"   " + ms[i].label;
        for (size_t p = 0; (p = t.find(L'&', p)) != std::wstring::npos; p += 2) t.insert(p, 1, L'&');  // no mnemonics
        AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), kCmdMarker + (UINT)i, t.c_str());
    }
}

void App::MarkerMenu(int i, POINT screen) {
    if (i < 0 || i >= markers_.Count()) return;
    enum : UINT { kGo = 1, kLabel, kDelete, kDeleteAll };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, markers_.Describe(i).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kGo, L"Go to marker");
    AppendMenuW(m, MF_STRING, kLabel, L"Label...");
    AppendMenuW(m, MF_STRING, kDelete, L"Delete");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kDeleteAll, L"Delete all markers...");
    thumbWnd_->Hide();
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd_, nullptr);
    DestroyMenu(m);
    switch (id) {
    case kGo: GoToMarker(i); break;
    case kLabel: RenameMarker(i); break;
    case kDelete: DeleteMarker(i); break;
    case kDeleteAll: ClearMarkers(); break;
    }
}
