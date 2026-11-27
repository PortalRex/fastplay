// Edit mode actions (FastCut's RegisterActions with its default keys), menus, segment operations, playback helpers,
// project files, and the export / tools jobs.
#include "editor.h"
#include "../app.h"
#include "../controls.h"
#include "../dialogs.h"
#include "eddialogs.h"
#include "exporter.h"
#include "ffrun.h"
#include "media.h"
#include "segio.h"
#include <winioctl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <numeric>
#include <sstream>

namespace edit {

namespace {
bool FileExists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Puts `edited` in place of `src`: the original is renamed aside first (that fails while anything still has it open,
// so it doubles as the "is it free yet" check), then the edited file takes its name and creation time, then the
// original is deleted. Any failure on the way puts the original back. Returns an empty string on success.
std::wstring SwapInEdited(const std::wstring& src, const std::wstring& edited) {
    std::wstring aside = util::DirName(src) + L"\\" + util::FileName(src) + L".fastplay-old";
    FILETIME created = {};
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa)) created = fa.ftCreationTime;
    bool moved = false;
    for (int i = 0; i < 75 && !moved; i++) {  // up to 15 s: mpv, the thumbnailer and the scanners let go quickly
        moved = MoveFileExW(src.c_str(), aside.c_str(), 0) != 0;
        if (!moved) Sleep(200);
    }
    if (!moved) return L"the original is still open in another program";
    if (!MoveFileExW(edited.c_str(), src.c_str(), 0)) {
        DWORD e = GetLastError();
        MoveFileExW(aside.c_str(), src.c_str(), 0);
        return L"renaming the edited file failed (error " + std::to_wstring(e) + L")";
    }
    HANDLE h = CreateFileW(src.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        if (created.dwLowDateTime || created.dwHighDateTime) SetFileTime(h, &created, nullptr, nullptr);
        CloseHandle(h);
    }
    for (int i = 0; i < 25 && !DeleteFileW(aside.c_str()); i++) Sleep(200);
    return L"";
}

std::string ReadAll(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

bool WriteAll(const std::wstring& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

// DiskInfo.cs: which physical disk a path is on and whether it spins.
int PhysicalDisk(const std::wstring& path) {
    std::wstring full = util::FullPath(path);
    if (full.size() < 2 || full[1] != L':') return -1;
    std::wstring dev = L"\\\\.\\" + full.substr(0, 2);
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    STORAGE_DEVICE_NUMBER n = {};
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof n, &got, nullptr);
    CloseHandle(h);
    return ok ? (int)n.DeviceNumber : -1;
}

bool SameSpinningDisk(const std::wstring& a, const std::wstring& b) {
    if (!util::IsOnSpinningDisk(a)) return false;
    int da = PhysicalDisk(a), db = PhysicalDisk(b);
    return da >= 0 && da == db;
}

// The local SSD with the most free space (at least needBytes), e.g. L"C:\\"; empty when none.
std::wstring FastestDriveWithRoom(uint64_t needBytes) {
    std::wstring best;
    uint64_t bestFree = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        ULARGE_INTEGER freeBytes;
        if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr)) continue;
        if (freeBytes.QuadPart <= needBytes || freeBytes.QuadPart <= bestFree || util::IsOnSpinningDisk(root)) continue;
        best = root;
        bestFree = freeBytes.QuadPart;
    }
    return best;
}

std::pair<double, double> Pair(const std::wstring& input, double a, double b) {
    double x = a, y = b;
    size_t comma = input.find(L',');
    std::wstring p0 = util::Trim(input.substr(0, comma)), p1 = comma == std::wstring::npos ? L"" : util::Trim(input.substr(comma + 1));
    wchar_t* end = nullptr;
    if (!p0.empty()) {
        double v = wcstod(p0.c_str(), &end);
        if (end && !*end) x = v;
    }
    if (!p1.empty()) {
        double v = wcstod(p1.c_str(), &end);
        if (end && !*end) y = v;
    }
    return {x, y};
}

// Indented JSON for Stream details.
void Pretty(const segio::Json& j, int depth, std::string& out) {
    std::string pad((size_t)depth * 2, ' ');
    switch (j.kind) {
    case segio::Json::Null: out += "null"; break;
    case segio::Json::Bool: out += j.b ? "true" : "false"; break;
    case segio::Json::Number: out += j.s.empty() ? std::to_string(j.n) : j.s; break;
    case segio::Json::String: out += segio::JsonQuote(j.s); break;
    case segio::Json::Array:
        out += "[\n";
        for (size_t i = 0; i < j.arr.size(); i++) {
            out += pad + "  ";
            Pretty(*j.arr[i], depth + 1, out);
            out += i + 1 < j.arr.size() ? ",\n" : "\n";
        }
        out += pad + "]";
        break;
    case segio::Json::Object:
        out += "{\n";
        for (size_t i = 0; i < j.obj.size(); i++) {
            out += pad + "  " + segio::JsonQuote(j.obj[i].first) + ": ";
            Pretty(*j.obj[i].second, depth + 1, out);
            out += i + 1 < j.obj.size() ? ",\n" : "\n";
        }
        out += pad + "}";
        break;
    }
}

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}
}  // namespace

// ------------------------------------------------------------------------------------------------ actions & keys

void Editor::RegisterActions() {
    std::wstring cat;
    auto add = [&](const wchar_t* id, const wchar_t* label, std::function<void()> fn, const wchar_t* keys = L"", bool repeat = false) {
        actionIndex_[id] = actions_.size();
        actions_.push_back({id, label, cat, keys, repeat, std::move(fn)});
    };
    auto held = [&](const wchar_t* id, const wchar_t* label, std::function<void()> fn, const wchar_t* keys = L"") {
        add(id, label, std::move(fn), keys, true);
    };
    mpv_handle*& mpv = app_->mpv_;

    cat = L"Playback";
    add(L"play", L"Play / pause", [this, &mpv] {
        if (!mpv || !media_) return;
        playUntil_ = -1;
        if (mpvx::GetFlag(mpv, "eof-reached")) Seek(0);
        mpvx::SetFlag(mpv, "pause", !app_->bar_.paused);
    }, L"Space");
    add(L"pause", L"Pause", [&mpv] { mpvx::SetFlag(mpv, "pause", true); }, L"K");
    add(L"playseg", L"Play selected segment", [this] { PlaySegment(); }, L"P");
    add(L"loop", L"Loop selected segment", [this] { ToggleLoop(); }, L"Ctrl+L");
    held(L"slower", L"Slower", [this] { ChangeSpeed(1 / 1.5); }, L"J");
    held(L"faster", L"Faster", [this] { ChangeSpeed(1.5); }, L"L");
    add(L"speedreset", L"Normal speed (1\u00D7)", [this] { ChangeSpeed(0); }, L"Shift+K");
    add(L"speed025", L"Speed 0.25\u00D7", [this] { SetSpeed(0.25); }, L"1");
    add(L"speed05", L"Speed 0.5\u00D7", [this] { SetSpeed(0.5); }, L"2");
    add(L"speed15", L"Speed 1.5\u00D7", [this] { SetSpeed(1.5); }, L"3");
    add(L"speed2", L"Speed 2\u00D7", [this] { SetSpeed(2); }, L"4");
    add(L"mute", L"Mute", [this] { app_->DoAction(Act::Mute); }, L"Ctrl+M");
    held(L"volup", L"Volume up", [this] { app_->SetVolume(app_->bar_.volume + 5); }, L"Shift+Up");
    held(L"voldown", L"Volume down", [this] { app_->SetVolume(app_->bar_.volume - 5); }, L"Shift+Down");
    add(L"cycleaudio", L"Next audio track (preview)", [&mpv] { mpvx::CmdAsync(mpv, {"osd-msg", "cycle", "audio"}); });
    add(L"cyclesub", L"Next subtitle track (preview)", [&mpv] { mpvx::CmdAsync(mpv, {"osd-msg", "cycle", "sub"}); }, L"V");
    add(L"fullscreen", L"Fullscreen", [this] { app_->DoAction(Act::Fullscreen); }, L"F");

    cat = L"Navigation";
    held(L"prevframe", L"Previous frame", [this] { FrameStep(false); }, L"Comma, MouseX1");
    held(L"nextframe", L"Next frame", [this] { FrameStep(true); }, L"Period, MouseX2");
    held(L"backframes", L"Back N frames (see Frame jump)", [this] { FrameJump(-s_.frameJump); }, L"Shift+Comma");
    held(L"fwdframes", L"Forward N frames (see Frame jump)", [this] { FrameJump(s_.frameJump); }, L"Shift+Period");
    held(L"back1", L"Seek back (small step)", [this] { Seek(Pos() - s_.seekSmall); }, L"Left");
    held(L"fwd1", L"Seek forward (small step)", [this] { Seek(Pos() + s_.seekSmall); }, L"Right");
    held(L"back10", L"Seek back (large step)", [this] { Seek(Pos() - s_.seekLarge); }, L"Shift+Left");
    held(L"fwd10", L"Seek forward (large step)", [this] { Seek(Pos() + s_.seekLarge); }, L"Shift+Right");
    held(L"prevkey", L"Previous keyframe", [this] { JumpKeyframe(false); }, L"Ctrl+Left");
    held(L"nextkey", L"Next keyframe", [this] { JumpKeyframe(true); }, L"Ctrl+Right");
    held(L"prevcut", L"Previous cut point", [this] { JumpCut(false); }, L"Up");
    held(L"nextcut", L"Next cut point", [this] { JumpCut(true); }, L"Down");
    add(L"segstartgo", L"Go to segment start", [this] { if (auto* s = Sel()) Seek(s->start); }, L"Ctrl+Home");
    add(L"segendgo", L"Go to segment end", [this] { if (auto* s = Sel()) Seek(s->end); }, L"Ctrl+End");
    add(L"home", L"Go to start", [this] { Seek(0); }, L"Home");
    add(L"end", L"Go to end", [this] { Seek(media_ ? media_->duration : 0); }, L"End");
    add(L"gototime", L"Go to time...", [this] {
        util::FocusIfActive(timeBox_);
        SendMessageW(timeBox_, EM_SETSEL, 0, -1);
    }, L"Ctrl+G");

    cat = L"Segments";
    add(L"setstart", L"Set segment start", [this] { SetStart(); }, L"I");
    add(L"setend", L"Set segment end", [this] { SetEnd(); }, L"O");
    add(L"add", L"Add segment at playhead", [this] { AddSegment(); }, L"Equals");
    add(L"split", L"Split segment at playhead", [this] { SplitSegment(); }, L"S");
    add(L"duplicate", L"Duplicate segment", [this] { DuplicateSegment(); }, L"Ctrl+D");
    add(L"delete", L"Delete segment", [this] { DeleteSegment(); }, L"Delete, Backspace");
    add(L"clear", L"Delete all segments", [this] {
        if (segs_.empty()) return;
        PushUndo();
        segs_.clear();
        SegmentsChanged();
    });
    held(L"nudgestartback", L"Move start back (nudge)", [this] { Nudge(true, -1); }, L"Alt+Left");
    held(L"nudgestartfwd", L"Move start forward (nudge)", [this] { Nudge(true, 1); }, L"Alt+Right");
    held(L"nudgeendback", L"Move end back (nudge)", [this] { Nudge(false, -1); }, L"Shift+Alt+Left");
    held(L"nudgeendfwd", L"Move end forward (nudge)", [this] { Nudge(false, 1); }, L"Shift+Alt+Right");
    add(L"prevseg", L"Select previous segment", [this] { SelectSegment(-1); }, L"PageUp");
    add(L"nextseg", L"Select next segment", [this] { SelectSegment(1); }, L"PageDown");
    add(L"label", L"Label segment...", [this] { LabelSegment(); }, L"F2");
    add(L"editseg", L"Edit segment...", [this] { EditSegment(); }, L"Enter");
    add(L"toggleseg", L"Include / exclude segment", [this] { ToggleSegment(); }, L"X");
    add(L"selectall", L"Include all segments", [this] { SetAllSegments(true); }, L"Ctrl+A");
    add(L"selectnone", L"Exclude all segments", [this] { SetAllSegments(false); }, L"Ctrl+Shift+A");
    add(L"invert", L"Invert segments", [this] { InvertSegments(); }, L"Y");
    add(L"snapkf", L"Snap segment starts to keyframes", [this] { SnapToKeyframes(); }, L"Shift+S");
    add(L"segup", L"Move segment up the list", [this] { MoveSegment(-1); }, L"Ctrl+Up");
    add(L"segdown", L"Move segment down the list", [this] { MoveSegment(1); }, L"Ctrl+Down");
    add(L"sort", L"Sort segments by time", [this] { SortSegments(); });
    add(L"divide", L"Divide timeline into segments...", [this] { DivideTimeline(); });
    add(L"shift", L"Shift all segments...", [this] { ShiftSegments(); });
    add(L"fromchapters", L"Segments from chapters", [this] { SegmentsFromChapters(); });
    add(L"undo", L"Undo", [this] { Undo(); }, L"Ctrl+Z");
    add(L"redo", L"Redo", [this] { Redo(); }, L"Ctrl+Y, Ctrl+Shift+Z");

    cat = L"Timeline & view";
    held(L"zoomin", L"Zoom in", [this] { tl_.Zoom(1.5, Pos()); }, L"Ctrl+Equals");
    held(L"zoomout", L"Zoom out", [this] { tl_.Zoom(1 / 1.5, Pos()); }, L"Ctrl+Minus");
    add(L"zoomseg", L"Zoom to segment", [this] { if (auto* s = Sel()) tl_.ZoomTo(s->start, s->end); }, L"Z");
    add(L"zoomall", L"Zoom to fit", [this] { tl_.ResetView(); }, L"Shift+Z");
    add(L"sidebar", L"Show sidebar", [this] { ToggleSidebar(); }, L"Ctrl+B");
    add(L"thumbs", L"Show thumbnails", [this] {
        s_.showThumbs = !s_.showThumbs;
        ApplyTimelineHeight();
        UpdateChecks();
        SaveSettings();
    });
    add(L"wave", L"Show waveform", [this] {
        // On, but skipped for a big file on a hard disk: this loads it now instead of switching it off.
        bool skipped = s_.showWave && !tl_.wave && media_ && media_->Audio();
        if (!skipped) s_.showWave = !s_.showWave;
        ApplyTimelineHeight();
        UpdateChecks();
        SaveSettings();
        if (s_.showWave) LoadWaveIfNeeded(true);
    });
    add(L"keyframes", L"Show keyframes", [this] {
        s_.showKeyframes = !s_.showKeyframes;
        ApplyTimelineHeight();
        UpdateChecks();
        SaveSettings();
    });
    add(L"offset", L"Set timecode offset...", [this] { SetTimecodeOffset(); });
    add(L"viewmode", L"Back to View mode", [this] { app_->DoAction(Act::EditMode); }, L"E");

    cat = L"File & export";
    add(L"open", L"Open file(s)...", [this] {
        auto files = PickOpenFiles(app_->hwnd_, file_.empty() ? L"" : util::DirName(file_));
        std::vector<std::wstring> media;
        for (auto& f : files) {
            if (Ext(f) == L".llc") LoadProjectFile(f);
            else media.push_back(f);
        }
        if (media.empty()) return;
        // Like FastCut: add to the Files tab (the playlist) and open the first new one.
        if (app_->plist_.empty() && !app_->path_.empty()) {
            app_->plist_ = {app_->path_};
            app_->plPos_ = 0;
        }
        bool hadFile = !app_->path_.empty();
        app_->OpenFiles(media, hadFile);
        if (hadFile) {
            for (size_t i = 0; i < app_->plist_.size(); i++)
                if (util::Lower(app_->plist_[i]) == util::Lower(util::FullPath(media[0]))) app_->PlaylistActivate((int)i);
        }
        RefreshFileList();
    }, L"Ctrl+O");
    add(L"openurl", L"Download URL / HLS stream...", [this] { DownloadUrl(); });
    add(L"close", L"Close file", [this] { app_->CloseFile(); }, L"Ctrl+W");
    add(L"save", L"Save project (.llc) next to file", [this] { SaveProject(); }, L"Ctrl+S");
    add(L"loadproject", L"Load project...", [this] { LoadProjectDialog(); });
    add(L"importseg", L"Import segments...", [this] { ImportSegments(); });
    add(L"exportseg", L"Export segments as...", [this] { ExportSegmentsAs(); });
    add(L"export", L"Export", [this] { Export(false); }, L"Ctrl+E");
    add(L"exportsel", L"Export selected segment(s) only", [this] { Export(true); }, L"Ctrl+Shift+E");
    add(L"exportover", L"Export and overwrite the original...", [this] { Export(false, true); });
    add(L"batch", L"Export all files in the Files tab...", [this] { BatchExport(); });
    add(L"exportopts", L"Export options...", [this] {
        double speed = 1;
        int n = (int)std::count_if(segs_.begin(), segs_.end(), [](const Segment& s) { return s.enabled; });
        if (dialogs::ExportOptions(app_->hwnd_, s_, n, Ext(file_), &speed)) SaveSettings();
        UpdateChecks();
    });
    add(L"settings", L"Settings...", [this] { ShowSettings(); }, L"Ctrl+Comma");

    cat = L"Tools";
    add(L"snapshot", L"Snapshot (full resolution)", [this] { Snapshot(); }, L"C");
    add(L"snapsegs", L"Snapshots at segment starts", [this] { SnapshotSegments(); });
    add(L"snapevery", L"Snapshots every N seconds...", [this] { SnapshotEvery(); });
    add(L"snapfmt", L"Snapshots as PNG", [this] {
        s_.snapshotFormat = s_.snapshotFormat == L"png" ? L"jpg" : L"png";
        UpdateChecks();
        SaveSettings();
    });
    add(L"extract", L"Extract all tracks", [this] { ExtractTracks(); });
    add(L"concat", L"Merge files...", [this] { ConcatFiles(); });
    add(L"detectblack", L"Detect black scenes...", [this] { DetectBlack(); });
    add(L"detectsilence", L"Detect silence...", [this] { DetectSilence(); });
    add(L"detectscenes", L"Detect scene changes...", [this] { DetectScenes(); });
    add(L"streaminfo", L"Stream details", [this] { ShowStreamInfo(); }, L"Ctrl+Shift+I");
    add(L"log", L"FFmpeg log", [this] { dialogs::TextViewer(app_->hwnd_, L"FFmpeg log", ff::LogText(), [] { return ff::LogText(); }); }, L"F12");
    add(L"autosave", L"Auto-save segments per file", [this] {
        s_.autoSaveProject = !s_.autoSaveProject;
        UpdateChecks();
        SaveSettings();
    });
    add(L"confirm", L"Show export options before exporting", [this] {
        s_.confirmExport = !s_.confirmExport;
        UpdateChecks();
        SaveSettings();
    });
    add(L"hotkeys", L"Keyboard & mouse shortcuts... (fastplay.ini)", [this] {
        SaveSettings();
        app_->DoAction(Act::EditSettings);
    }, L"Ctrl+K");
    add(L"shortcuts", L"Shortcut cheat sheet", [this] { ShowTextBox(app_->hwnd_, L"Edit mode shortcuts", ActionsHelp()); }, L"F1");
}

// Edit-mode defaults that changed. SaveSettings writes every default into [edit_keys], so a value still equal to the
// old default means "never customised": it follows the new default.
void Editor::MigrateKeyDefaults() {
    static const struct { const wchar_t* id; const wchar_t* oldDefault; } kChanged[] = {
        {L"mute", L"M"},  // M adds a timeline marker in both modes now (View mode's mute is Ctrl+M as well)
    };
    for (const auto& c : kChanged) {
        auto it = actionIndex_.find(c.id);
        if (it == actionIndex_.end() || !app_->ini_.Has(L"edit_keys", c.id)) continue;
        if (util::Trim(app_->ini_.Get(L"edit_keys", c.id)) == c.oldDefault)
            app_->ini_.Set(L"edit_keys", c.id, actions_[it->second].defaults);
    }
}

void Editor::LoadKeymap() {
    keyToAction_.clear();
    keyText_.clear();
    MigrateKeyDefaults();
    for (const auto& a : actions_) {
        std::wstring list = app_->ini_.Has(L"edit_keys", a.id) ? app_->ini_.Get(L"edit_keys", a.id) : a.defaults;
        size_t pos = 0;
        bool first = true;
        while (pos <= list.size()) {
            size_t comma = list.find(L',', pos);
            if (comma == std::wstring::npos) comma = list.size();
            uint32_t c;
            if (ParseChord(list.substr(pos, comma - pos), &c)) {
                keyToAction_.emplace(c, a.id);
                if (first) keyText_[a.id] = ChordName(c);
                first = false;
            }
            pos = comma + 1;
        }
    }
}

std::wstring Editor::KeyText(const std::wstring& id) const {
    auto it = keyText_.find(id);
    return it == keyText_.end() ? L"" : it->second;
}

bool Editor::HandleChord(uint32_t chord, bool repeat) {
    auto it = keyToAction_.find(chord);
    if (it == keyToAction_.end()) return false;
    auto ai = actionIndex_.find(it->second);
    if (ai == actionIndex_.end()) return false;
    if (repeat && !actions_[ai->second].repeat) return true;
    Run(it->second);
    return true;
}

void Editor::Run(const std::wstring& id) {
    auto it = actionIndex_.find(id);
    if (it == actionIndex_.end()) return;
    try {
        actions_[it->second].run();
    } catch (const EditError& e) {
        SetStatus(L"Error: " + e.msg);
    } catch (const std::exception& e) {
        SetStatus(L"Error: " + util::Wide(e.what()));
    }
}

std::wstring Editor::ActionsHelp() const {
    std::wstring t = L"Edit mode shortcuts (change them in fastplay.ini, [edit_keys]). Keys not listed here fall back to\n"
                     L"View mode's MPC-HC keys.\n";
    std::wstring cat;
    for (const auto& a : actions_) {
        if (a.category != cat) {
            cat = a.category;
            t += L"\n" + cat + L"\n";
        }
        auto ki = app_->ini_.Has(L"edit_keys", a.id) ? app_->ini_.Get(L"edit_keys", a.id) : a.defaults;
        std::wstring label = L"  " + a.label;
        label.resize(std::max<size_t>(label.size(), 44), L' ');
        t += label + (ki.empty() ? L"-" : ki) + L"\n";
    }
    return t;
}

// ------------------------------------------------------------------------------------------------ menus

void Editor::BuildMenu(HMENU m, const std::vector<const wchar_t*>& ids) {
    for (const wchar_t* id : ids) {
        if (!id) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        auto it = actionIndex_.find(id);
        if (it == actionIndex_.end()) continue;
        const auto& a = actions_[it->second];
        std::wstring text = a.label;
        std::wstring k = KeyText(a.id);
        if (!k.empty()) text += L"\t" + k;
        bool checked = (a.id == L"thumbs" && s_.showThumbs) || (a.id == L"wave" && s_.showWave) ||
                       (a.id == L"keyframes" && s_.showKeyframes) || (a.id == L"autosave" && s_.autoSaveProject) ||
                       (a.id == L"snapfmt" && s_.snapshotFormat == L"png") || (a.id == L"confirm" && s_.confirmExport) ||
                       (a.id == L"loop" && looping_) || (a.id == L"sidebar" && sidebar_);
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), 10000 + it->second, text.c_str());
    }
}

void Editor::ShowMenu(const std::wstring& title, POINT screen) {
    HMENU m = CreatePopupMenu();
    if (title == L"File")
        BuildMenu(m, {L"open", L"openurl", L"close", nullptr, L"loadproject", L"save", L"importseg", L"exportseg", nullptr, L"export",
                      L"exportsel", L"exportover", L"batch", L"exportopts", nullptr, L"settings"});
    else if (title == L"Edit" || title == L"Segment")
        BuildMenu(m, {L"undo", L"redo", nullptr, L"setstart", L"setend", L"add", L"split", L"duplicate", L"delete", L"clear", nullptr,
                      L"nudgestartback", L"nudgestartfwd", L"nudgeendback", L"nudgeendfwd", nullptr, L"label", L"editseg", L"toggleseg",
                      L"selectall", L"selectnone", nullptr, L"invert", L"snapkf", L"segup", L"segdown", L"sort", L"divide", L"shift",
                      L"fromchapters"});
    else if (title == L"Playback")
        BuildMenu(m, {L"play", L"slower", L"faster", L"speedreset", L"speed025", L"speed05", L"speed15", L"speed2", nullptr,
                      L"prevframe", L"nextframe", L"backframes", L"fwdframes", L"prevkey", L"nextkey", L"prevcut", L"nextcut",
                      L"segstartgo", L"segendgo", L"gototime", nullptr, L"playseg", L"loop", nullptr, L"mute", L"volup", L"voldown",
                      L"cycleaudio", L"cyclesub"});
    else if (title == L"View")
        BuildMenu(m, {L"zoomin", L"zoomout", L"zoomseg", L"zoomall", nullptr, L"sidebar", L"thumbs", L"wave", L"keyframes", nullptr,
                      L"fullscreen", L"offset", nullptr, L"viewmode"});
    else if (title == L"Tools")
        BuildMenu(m, {L"snapshot", L"snapsegs", L"snapevery", L"snapfmt", nullptr, L"extract", L"concat", nullptr, L"detectblack",
                      L"detectsilence", L"detectscenes", nullptr, L"streaminfo", L"log", nullptr, L"autosave", L"confirm", nullptr,
                      L"hotkeys", L"shortcuts"});
    UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN, screen.x, screen.y, app_->hwnd_, nullptr);
    DestroyMenu(m);
    if (id >= 10000 && id - 10000 < actions_.size()) Run(actions_[id - 10000].id);
}

void Editor::AppendMenuItems(HMENU m) {
    // FastPlay's right-click menu while editing: the segment commands first.
    HMENU seg = CreatePopupMenu();
    BuildMenu(seg, {L"setstart", L"setend", L"add", L"split", L"delete", nullptr, L"label", L"editseg", L"toggleseg", nullptr,
                    L"export", L"exportsel"});
    AppendMenuW(m, MF_POPUP, (UINT_PTR)seg, L"Segments");
}

bool Editor::RunMenuId(UINT id) {
    if (id < 10000 || id - 10000 >= actions_.size()) return false;
    Run(actions_[id - 10000].id);
    return true;
}

void Editor::UpdateChecks() {
    if (!created_) return;
    UpdateExportBar();
    auto& bar = exportBar_.items;
    bar[11].text = s_.merge ? L"Merge cuts" : L"Separate files";
    std::wstring ext = Ext(file_);
    std::wstring f = s_.format;
    std::wstring label = f == L"same" ? L"Same (" + (ext.empty() ? std::wstring(L"input") : ext.substr(1)) + L")" : f;
    if (f != L"same")
        for (auto& c : label) c = (wchar_t)towupper(c);
    bar[9].text = label;
    controlRow_.items[11].glyph = app_->bar_.mute ? glyph::Mute : glyph::Volume;
    controlRow_.items[12].max = app_->bar_.volumeMax;
    controlRow_.items[12].value = app_->bar_.volume;
    exportBar_.Relayout();
    controlRow_.Invalidate();
}

bool Editor::WillReencode() const {
    if (!media_) return false;
    double seconds = 0, sum = 0, mx = 0;
    int on = 0;
    for (const auto& s : segs_)
        if (s.enabled && s.end > s.start) {
            on++;
            sum += s.Duration();
            mx = std::max(mx, s.Duration());
        }
    seconds = on == 0 ? media_->duration : s_.merge ? sum : mx;
    std::vector<int> streams;
    for (const auto& st : media_->streams)
        if (st.keep) streams.push_back(st.index);
    return Exporter::NeedsReencode(*media_, s_, streams, seconds);
}

void Editor::UpdateExportBar() {
    bool re = WillReencode();
    auto& bar = exportBar_.items;
    bar[7].text = s_.discordReady ? L"Discord \u2713" : L"Discord off";
    bar[7].checked = s_.discordReady;
    bar[8].visible = s_.discordReady;
    bar[8].text = s_.discordSizeMB > 0 ? L"\u2264 " + std::to_wstring(s_.discordSizeMB) + L" MB" : L"Any size";
    // A re-encode is frame-exact, so the cut mode (and its keyframe shading and warnings) doesn't apply.
    bar[10].text = re ? L"Exact (H.264)" : s_.cutMode == L"exact" ? L"Normal cut" : s_.cutMode == L"smart" ? L"Smart cut" : L"Keyframe cut";
    bar[10].enabled = !re;
    std::wstring codec = media_ && media_->Video() ? util::Wide(media_->Video()->codec) : L"";
    for (auto& c : codec) c = (wchar_t)towupper(c);
    bar[10].tip = re ? L"This " + codec + L" video is re-encoded to H.264 for Discord, so cuts are frame-exact"
                     : L"How segments are cut (click to change)";
    tl_.cutMode = re ? L"smart" : s_.cutMode;
    tl_.Invalidate();
    cards_.Refresh();
}

std::wstring Editor::CutWarning(int idx) const {
    if (idx < 0 || idx >= (int)segs_.size()) return L"";
    const auto& s = segs_[idx];
    const auto& k = kf_.pts;
    if (!s.enabled || k.empty() || !media_ || !media_->Video() || WillReencode()) return L"";
    wchar_t b[64];
    if (s_.cutMode == L"keyframe") {
        auto it = std::upper_bound(k.begin(), k.end(), s.start + 0.0005);
        double early = it == k.begin() ? 0 : s.start - *(it - 1);
        if (early > 0.02) { swprintf(b, 64, L"starts %.2fs early", early); return b; }
        return L"";
    }
    if (s_.cutMode == L"exact") {
        auto it = std::lower_bound(k.begin(), k.end(), s.start - 0.0005);
        if (it == k.end() || *it >= s.end) return L"no video: no keyframe inside";
        double late = *it - s.start;
        if (late > 0.02) { swprintf(b, 64, L"video starts %.2fs late", late); return b; }
        return L"";
    }
    const std::string& c = media_->Video()->codec;
    return c == "h264" || c == "hevc" ? L"" : L"smart cut can't re-encode " + util::Wide(c);
}

// ------------------------------------------------------------------------------------------------ time helpers

void Editor::Seek(double t) {
    if (!media_) return;
    t = std::clamp(t, 0.0, media_->duration);
    tl_.position = t;
    holdPos_ = t;
    holdPosUntil_ = GetTickCount() + 300;  // show the target until mpv's position catches up
    app_->SeekTo(t, true);
    tl_.Invalidate();
    if (GetFocus() != timeBox_) SetWindowTextW(timeBox_, FmtT(t).c_str());
}

double Editor::Pos() const { return tl_.position; }
std::wstring Editor::FmtT(double t) const { return Fmt(t + timeOffset_); }

bool Editor::ParseT(const std::wstring& s, double* t) const {
    double v;
    if (!TryParseTime(util::Trim(s), &v)) return false;
    *t = v - timeOffset_;
    return true;
}

// ------------------------------------------------------------------------------------------------ segments

Segment Editor::NewSeg(double a, double b) {
    Segment s;
    s.start = a;
    s.end = b;
    s.color = nextColor_++;
    return s;
}

std::vector<Segment> Editor::Colorize(std::vector<Segment> list) {
    double d = media_ ? media_->duration : 1e18;
    for (auto& s : list) {
        s.start = std::clamp(s.start, 0.0, d);
        s.end = std::clamp(s.end, s.start, d);
        s.color = nextColor_++;
    }
    return list;
}

const Segment* Editor::Sel() const { return tl_.selected >= 0 && tl_.selected < (int)segs_.size() ? &segs_[tl_.selected] : nullptr; }
Segment* Editor::SelMut() { return tl_.selected >= 0 && tl_.selected < (int)segs_.size() ? &segs_[tl_.selected] : nullptr; }

std::vector<int> Editor::Targets() const {
    std::vector<int> r;
    if (marked_.size() > 1) {
        for (int i : marked_)
            if (i < (int)segs_.size()) r.push_back(i);
    } else if (Sel()) {
        r.push_back(tl_.selected);
    }
    return r;
}

void Editor::PushUndo() {
    undo_.push_back(segs_);
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
}

void Editor::Undo() {
    if (undo_.empty()) return;
    redo_.push_back(segs_);
    segs_ = std::move(undo_.back());
    undo_.pop_back();
    SegmentsChanged();
}

void Editor::Redo() {
    if (redo_.empty()) return;
    undo_.push_back(segs_);
    segs_ = std::move(redo_.back());
    redo_.pop_back();
    SegmentsChanged();
}

void Editor::SegmentsChanged(bool rebuildList) {
    if (tl_.selected >= (int)segs_.size()) tl_.selected = (int)segs_.size() - 1;
    if (rebuildList) RebuildSegList();
    else UpdateSegHeader();
    tl_.Invalidate();
    SaveAutosave();
}

void Editor::RebuildSegList() {
    for (auto it = marked_.begin(); it != marked_.end();) it = *it >= (int)segs_.size() ? marked_.erase(it) : std::next(it);
    if (marked_.size() <= 1) {
        marked_.clear();
        if (tl_.selected >= 0) marked_.insert(tl_.selected);
    }
    cards_.current = tl_.selected;
    cards_.Refresh();
    cards_.EnsureVisible(tl_.selected);
    if (created_ && tab_ == 0) ShowTab(0);
    UpdateSegHeader();
}

void Editor::UpdateSegHeader() {
    if (!created_) return;
    int on = 0;
    double total = 0;
    for (const auto& s : segs_)
        if (s.enabled) {
            on++;
            total += s.Duration();
        }
    tabStrip_.items[0].text = segs_.empty() ? L"Segments" : L"Segments  " + std::to_wstring(on) + L"/" + std::to_wstring(segs_.size());
    tabStrip_.Relayout();
    cards_.Refresh();
    UpdateCutBoxes();
    if (media_) {
        size_t kept = std::count_if(media_->streams.begin(), media_->streams.end(), [](const StreamInfo& s) { return s.keep; });
        exportBar_.items[0].text = std::to_wstring(kept) + L"/" + std::to_wstring(media_->streams.size()) + L" tracks";
    }
    std::wstring size = media_ && media_->bitRate > 0 ? L"  \u00B7  \u2248 " + [&] {
        double b = total * media_->bitRate / 8;
        wchar_t s[32];
        if (b >= 1e9) swprintf(s, 32, L"%.1f GB", b / 1e9);
        else if (b >= 1e6) swprintf(s, 32, L"%.0f MB", b / 1e6);
        else swprintf(s, 32, L"%.0f KB", b / 1e3);
        return std::wstring(s);
    }() : L"";
    segSummary_.items[0].text = segs_.empty() ? L"" : std::to_wstring(on) + L" of " + std::to_wstring(segs_.size()) + L" included  \u00B7  " +
                                                         Fmt(total, false) + size;
    segSummary_.Invalidate();
    UpdateExportBar();
    auto& exp = exportBar_.items[12];
    if (!jobRunning_ || !jobOnExport_) {
        exp.text = on > 1 && s_.merge ? L"Export " + std::to_wstring(on) + L" \u2192 1" : on > 1 ? L"Export " + std::to_wstring(on) : L"Export";
        exp.enabled = media_ != nullptr && !jobRunning_;
    }
    exportBar_.Relayout();
}

void Editor::UpdateCutBoxes() {
    const Segment* s = Sel();
    if (GetFocus() != cutStartBox_) SetWindowTextW(cutStartBox_, s ? FmtT(s->start).c_str() : L"--:--:--.---");
    if (GetFocus() != cutEndBox_) SetWindowTextW(cutEndBox_, s ? FmtT(s->end).c_str() : L"--:--:--.---");
    uint32_t c = s ? Palette()[s->color % Palette().size()] : theme::Card;
    controlRow_.items[0].fill = c;
    controlRow_.items[14].fill = c;
    controlRow_.Invalidate();
}

void Editor::SetCutTime(bool isStart, const std::wstring& text) {
    double t;
    if (!media_ || !ParseT(text, &t)) {
        UpdateCutBoxes();
        return;
    }
    t = std::clamp(t, 0.0, media_->duration);
    Segment* s = SelMut();
    if (!s) {
        Seek(t);
        Run(isStart ? L"setstart" : L"setend");
        return;
    }
    if (isStart ? t >= s->end : t <= s->start) {
        SetStatus(isStart ? L"Start must be before the end." : L"End must be after the start.");
        UpdateCutBoxes();
        return;
    }
    PushUndo();
    if (isStart) s->start = t;
    else s->end = t;
    SegmentsChanged();
    Seek(t);
}

void Editor::SyncListSelection() {
    marked_.clear();
    if (tl_.selected >= 0) marked_.insert(tl_.selected);
    cards_.current = tl_.selected;
    cards_.EnsureVisible(tl_.selected);
    cards_.Refresh();
    tl_.Invalidate();
    UpdateCutBoxes();
}

void Editor::SelectIndex(int i, uint32_t mods, bool seek) {
    if (i < 0 || i >= (int)segs_.size()) return;
    if ((mods & MK_SHIFT) && tl_.selected >= 0) {
        marked_.clear();
        for (int k = std::min(i, tl_.selected); k <= std::max(i, tl_.selected); k++) marked_.insert(k);
    } else if (mods & MK_CONTROL) {
        if (tl_.selected >= 0) marked_.insert(tl_.selected);
        if (!marked_.insert(i).second && marked_.size() > 1) {
            marked_.erase(i);
            i = *marked_.rbegin();
        }
    } else {
        marked_.clear();
        marked_.insert(i);
    }
    tl_.selected = i;
    cards_.current = i;
    cards_.EnsureVisible(i);
    cards_.Refresh();
    tl_.Invalidate();
    UpdateCutBoxes();
    if (seek) Seek(segs_[i].start);
}

void Editor::ReorderSegment(int from, int to) {
    if (from < 0 || from >= (int)segs_.size() || to < 0 || to >= (int)segs_.size() || from == to) return;
    PushUndo();
    Segment item = segs_[from];
    segs_.erase(segs_.begin() + from);
    segs_.insert(segs_.begin() + to, item);
    tl_.selected = to;
    marked_.clear();
    SegmentsChanged();
}

void Editor::InsertAndSelect(Segment s) {
    segs_.push_back(s);
    tl_.selected = (int)segs_.size() - 1;
    SegmentsChanged();
}

void Editor::SetStart() {
    if (!media_) return;
    double t = Pos();
    PushUndo();
    Segment* s = SelMut();
    if (!s) {
        double end = media_->duration;
        for (const auto& x : segs_)
            if (x.start > t) end = std::min(end, x.start);
        InsertAndSelect(NewSeg(t, end));
        return;
    }
    if (t >= s->end) s->end = media_->duration;
    if (t >= s->end) {
        undo_.pop_back();
        SetStatus(L"Start must be before the end.");
        return;
    }
    s->start = t;
    SegmentsChanged();
}

void Editor::SetEnd() {
    if (!media_) return;
    double t = Pos();
    PushUndo();
    Segment* s = SelMut();
    if (!s) {
        double start = 0;
        for (const auto& x : segs_)
            if (x.end < t) start = std::max(start, x.end);
        InsertAndSelect(NewSeg(start, t));
        return;
    }
    if (t <= s->start) {
        undo_.pop_back();
        SetStatus(L"End must be after the start.");
        return;
    }
    s->end = t;
    SegmentsChanged();
}

void Editor::AddSegment() {
    if (!media_) return;
    double t = Pos(), end = media_->duration;
    for (const auto& x : segs_)
        if (x.start > t + 0.001) end = std::min(end, x.start);
    if (end - t < 0.01) end = media_->duration;
    if (end - t < 0.01) {
        SetStatus(L"No room for a segment here.");
        return;
    }
    PushUndo();
    InsertAndSelect(NewSeg(t, end));
}

void Editor::SplitSegment() {
    double t = Pos();
    int i = -1;
    if (const Segment* s0 = Sel(); s0 && t > s0->start && t < s0->end) i = tl_.selected;
    else
        for (size_t k = 0; k < segs_.size(); k++)
            if (t > segs_[k].start + 0.001 && t < segs_[k].end - 0.001) { i = (int)k; break; }
    if (i < 0) {
        SetStatus(L"Playhead is not inside a segment.");
        return;
    }
    PushUndo();
    Segment b = NewSeg(t, segs_[i].end);
    b.name = segs_[i].name;
    b.tags = segs_[i].tags;
    b.enabled = segs_[i].enabled;
    segs_[i].end = t;
    segs_.insert(segs_.begin() + i + 1, b);
    tl_.selected = i + 1;
    SegmentsChanged();
}

void Editor::DeleteSegment() {
    auto t = Targets();
    if (t.empty()) return;
    PushUndo();
    for (auto it = t.rbegin(); it != t.rend(); ++it) segs_.erase(segs_.begin() + *it);
    tl_.selected = std::min(t[0], (int)segs_.size() - 1);
    marked_.clear();
    SegmentsChanged();
}

void Editor::DuplicateSegment() {
    const Segment* s = Sel();
    if (!s) return;
    PushUndo();
    Segment copy = *s;
    copy.color = nextColor_++;
    segs_.insert(segs_.begin() + tl_.selected + 1, copy);
    tl_.selected++;
    SegmentsChanged();
}

void Editor::SelectSegment(int dir) {
    if (segs_.empty()) return;
    tl_.selected = std::clamp(tl_.selected + dir, 0, (int)segs_.size() - 1);
    SyncListSelection();
    Seek(segs_[tl_.selected].start);
}

void Editor::LabelSegment() {
    auto t = Targets();
    if (t.empty()) return;
    std::wstring name = segs_[t[0]].name;
    std::wstring title = t.size() > 1 ? L"Label " + std::to_wstring(t.size()) + L" segments" : L"Label segment";
    if (!dialogs::Prompt(app_->hwnd_, title.c_str(), L"Label", &name)) return;
    PushUndo();
    for (int i : t) segs_[i].name = util::Trim(name);
    SegmentsChanged();
}

void Editor::EditSegment() {
    Segment* s = SelMut();
    if (!s) return;
    Segment copy = *s;
    auto parse = [this](const std::wstring& text, double* t) { return ParseT(text, t); };
    if (!dialogs::EditSegment(app_->hwnd_, copy, [this](double t) { return FmtT(t); }, parse)) return;
    if (copy.end <= copy.start) {
        SetStatus(L"Invalid times: end must be after start.");
        return;
    }
    PushUndo();
    segs_[tl_.selected] = copy;
    SegmentsChanged();
}

void Editor::ToggleSegment() {
    auto t = Targets();
    if (t.empty()) return;
    PushUndo();
    bool on = !segs_[t[0]].enabled;
    for (int i : t) segs_[i].enabled = on;
    SegmentsChanged();
}

void Editor::SetAllSegments(bool on) {
    if (segs_.empty()) return;
    PushUndo();
    for (auto& s : segs_) s.enabled = on;
    SegmentsChanged();
}

void Editor::MoveSegment(int dir) {
    int i = tl_.selected, j = i + dir;
    if (i < 0 || j < 0 || j >= (int)segs_.size()) return;
    PushUndo();
    std::swap(segs_[i], segs_[j]);
    tl_.selected = j;
    SegmentsChanged();
}

void Editor::SortSegments() {
    if (segs_.size() < 2) return;
    PushUndo();
    int selColor = Sel() ? Sel()->color : -1;
    double selStart = Sel() ? Sel()->start : 0;
    std::stable_sort(segs_.begin(), segs_.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
    tl_.selected = 0;
    for (size_t i = 0; i < segs_.size(); i++)
        if (segs_[i].color == selColor && segs_[i].start == selStart) tl_.selected = (int)i;
    SegmentsChanged();
}

void Editor::InvertSegments() {
    if (!media_) return;
    std::vector<Segment> on;
    for (const auto& s : segs_)
        if (s.enabled) on.push_back(s);
    std::stable_sort(on.begin(), on.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
    std::vector<Segment> gaps;
    double cur = 0;
    for (const auto& s : on) {
        if (s.start > cur + 0.001) gaps.push_back(NewSeg(cur, s.start));
        cur = std::max(cur, s.end);
    }
    if (media_->duration > cur + 0.001) gaps.push_back(NewSeg(cur, media_->duration));
    PushUndo();
    segs_ = gaps;
    tl_.selected = segs_.empty() ? -1 : 0;
    SegmentsChanged();
}

void Editor::DivideTimeline() {
    if (!media_) return;
    std::wstring input = L"10";
    if (!dialogs::Prompt(app_->hwnd_, L"Divide timeline", L"Number of segments, or a length like 30s / 1:30", &input)) return;
    input = util::Trim(input);
    double len = 0;
    if (!input.empty() && (input.back() == L's' || input.find(L':') != std::wstring::npos)) {
        std::wstring v = input;
        while (!v.empty() && v.back() == L's') v.pop_back();
        if (!TryParseTime(v, &len) || len <= 0) {
            SetStatus(L"Invalid length.");
            return;
        }
    } else {
        int n = _wtoi(input.c_str());
        if (n <= 0) {
            SetStatus(L"Invalid number.");
            return;
        }
        len = media_->duration / n;
    }
    if (media_->duration / len > 10000) {
        SetStatus(L"That would create too many segments.");
        return;
    }
    PushUndo();
    segs_.clear();
    for (double t = 0; t < media_->duration - 0.001; t += len) segs_.push_back(NewSeg(t, std::min(media_->duration, t + len)));
    tl_.selected = 0;
    SegmentsChanged();
}

void Editor::ShiftSegments() {
    if (!media_ || segs_.empty()) return;
    std::wstring input = L"0";
    double d;
    if (!dialogs::Prompt(app_->hwnd_, L"Shift all segments", L"Seconds (negative = earlier)", &input)) return;
    std::wstring v = util::Trim(input);
    bool neg = !v.empty() && v[0] == L'-';
    if (!TryParseTime(neg ? v.substr(1) : v, &d)) return;
    if (neg) d = -d;
    PushUndo();
    for (auto& s : segs_) {
        s.start = std::clamp(s.start + d, 0.0, media_->duration);
        s.end = std::clamp(s.end + d, 0.0, media_->duration);
    }
    segs_.erase(std::remove_if(segs_.begin(), segs_.end(), [](const Segment& s) { return s.end - s.start < 0.001; }), segs_.end());
    SegmentsChanged();
}

void Editor::SegmentsFromChapters() {
    if (!media_) return;
    if (media_->chapters.empty()) {
        SetStatus(L"This file has no chapters.");
        return;
    }
    PushUndo();
    segs_.clear();
    for (const auto& c : media_->chapters) {
        Segment s = NewSeg(std::max(0.0, c.start), std::min(media_->duration, c.end));
        s.name = c.title;
        segs_.push_back(s);
    }
    tl_.selected = 0;
    SegmentsChanged();
}

void Editor::SnapToKeyframes() {
    const auto& k = kf_.pts;
    if (k.empty()) {
        SetStatus(L"Keyframes are still loading.");
        return;
    }
    std::vector<int> targets;
    if (marked_.size() > 1) targets = Targets();
    else
        for (size_t i = 0; i < segs_.size(); i++)
            if (segs_[i].enabled) targets.push_back((int)i);
    if (targets.empty()) return;
    PushUndo();
    int moved = 0;
    for (int idx : targets) {
        auto& s = segs_[idx];
        auto it = std::lower_bound(k.begin(), k.end(), s.start);
        if (it != k.end() && *it == s.start) continue;
        double a = it != k.begin() ? *(it - 1) : 1e300, b = it != k.end() ? *it : 1e300;
        double snap = std::abs(a - s.start) <= std::abs(b - s.start) ? a : b;
        if (snap >= s.end - 0.01) snap = a;  // never snap past the end
        if (snap >= 1e300 || std::abs(snap - s.start) < 0.0005) continue;
        s.start = snap;
        moved++;
    }
    if (moved == 0) {
        undo_.pop_back();
        SetStatus(L"Segment starts are already on keyframes.");
        return;
    }
    SegmentsChanged();
    SetStatus(L"Moved " + Plural(moved, L"segment start", L"segment starts") + L" to the nearest keyframe.");
}

// ------------------------------------------------------------------------------------------------ playback helpers

void Editor::ChangeSpeed(double factor) {
    mpv_handle* mpv = app_->mpv_;
    if (!mpv) return;
    double sp = factor == 0 ? 1 : std::clamp(mpvx::GetDouble(mpv, "speed", 1) * factor, 0.1, 16.0);
    SetSpeed(sp);
}

void Editor::SetSpeed(double speed) {
    if (!app_->mpv_) return;
    mpvx::SetDouble(app_->mpv_, "speed", speed);
    wchar_t b[32] = L"";
    if (std::abs(speed - 1) >= 0.001) swprintf(b, 32, L"%.3g\u00D7", speed);
    controlRow_.items[13].text = b;
    controlRow_.Relayout();
}

void Editor::FrameStep(bool forward) {
    if (!app_->mpv_ || !media_) return;
    holdPosUntil_ = 0;
    mpvx::CmdAsync(app_->mpv_, {forward ? "frame-step" : "frame-back-step"});
}

void Editor::FrameJump(int frames) {
    if (!media_) return;
    double fs = media_->Video() && media_->Video()->fps > 0 ? 1 / media_->Video()->fps : 1 / 30.0;
    mpvx::SetFlag(app_->mpv_, "pause", true);
    Seek(Pos() + frames * fs);
}

void Editor::Nudge(bool start, int dir) {
    Segment* s = SelMut();
    if (!s || !media_) return;
    double fs = media_->Video() && media_->Video()->fps > 0 ? 1 / media_->Video()->fps : 1 / 30.0;
    if (GetTickCount() - lastNudge_ > 1000) PushUndo();  // one undo step for a burst of nudges
    lastNudge_ = GetTickCount();
    double d = dir * s_.nudgeFrames * fs;
    if (start) s->start = std::clamp(s->start + d, 0.0, s->end - fs);
    else s->end = std::clamp(s->end + d, s->start + fs, media_->duration);
    double t = start ? s->start : s->end;
    SegmentsChanged();
    mpvx::SetFlag(app_->mpv_, "pause", true);
    Seek(t);
}

void Editor::JumpKeyframe(bool forward) {
    const auto& k = tl_.keyframes;
    if (k.empty()) return;
    double t = Pos();
    if (forward) {
        auto it = std::upper_bound(k.begin(), k.end(), t + 1e-6);
        if (it != k.end()) Seek(*it);
    } else {
        auto it = std::lower_bound(k.begin(), k.end(), t - 1e-6);
        if (it != k.begin()) Seek(*(it - 1));
    }
}

void Editor::JumpCut(bool forward) {
    std::vector<double> pts = {0, media_ ? media_->duration : 0};
    for (const auto& s : segs_) {
        pts.push_back(s.start);
        pts.push_back(s.end);
    }
    std::sort(pts.begin(), pts.end());
    double t = Pos();
    if (forward) {
        for (double p : pts)
            if (p > t + 0.0005) { Seek(p); return; }
    } else {
        for (auto it = pts.rbegin(); it != pts.rend(); ++it)
            if (*it < t - 0.0005) { Seek(*it); return; }
    }
}

void Editor::ToggleLoop() {
    mpv_handle* mpv = app_->mpv_;
    if (!mpv) return;
    const Segment* s = Sel();
    if (looping_ || !s) {
        mpvx::SetStr(mpv, "ab-loop-a", "no");
        mpvx::SetStr(mpv, "ab-loop-b", "no");
        looping_ = false;
    } else {
        mpvx::SetStr(mpv, "ab-loop-a", util::Utf8(Num(s->start)).c_str());
        mpvx::SetStr(mpv, "ab-loop-b", util::Utf8(Num(s->end)).c_str());
        looping_ = true;
        Seek(s->start);
        mpvx::SetFlag(mpv, "pause", false);
    }
    SetStatus(looping_ ? L"Looping selected segment" : L"Loop off");
}

void Editor::PlaySegment() {
    const Segment* s = Sel();
    if (!app_->mpv_ || !s) return;
    double end = s->end;
    Seek(s->start);
    playUntil_ = end;
    mpvx::SetFlag(app_->mpv_, "pause", false);
}

void Editor::SetTimecodeOffset() {
    std::wstring input = Fmt(timeOffset_);
    double off;
    if (!dialogs::Prompt(app_->hwnd_, L"Timecode offset", L"Offset added to displayed times (e.g. 01:00:00)", &input)) return;
    if (!TryParseTime(util::Trim(input), &off)) return;
    timeOffset_ = off;
    RebuildSegList();
    SetWindowTextW(timeBox_, FmtT(Pos()).c_str());
    tl_.Invalidate();
}

void Editor::ToggleSidebar() {
    sidebar_ = !sidebar_;
    app_->Layout();
}

void Editor::SetAllTracks(bool on) {
    if (!media_) return;
    for (auto& s : media_->streams) s.keep = on;
    RebuildTrackList();
    UpdateSegHeader();
}

void Editor::ShowStreamInfo() {
    if (!media_) return;
    std::string pretty;
    try {
        Pretty(*segio::ParseJson(media_->rawJson), 0, pretty);
    } catch (...) {
        pretty = media_->rawJson;
    }
    std::wstring text = util::Wide(pretty);
    dialogs::TextViewer(app_->hwnd_, (L"Streams \u2014 " + util::FileName(media_->path)).c_str(), text);
}

// ------------------------------------------------------------------------------------------------ project files

static std::wstring ProjectNextTo(const std::wstring& media) {
    size_t dot = media.find_last_of(L'.');
    size_t slash = media.find_last_of(L"\\/");
    std::wstring base = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? media.substr(0, dot) : media;
    return base + L"-proj.llc";
}

std::vector<Segment> Editor::LoadAutosave(const std::wstring& path) {
    try {
        std::wstring llc = ProjectNextTo(path);
        std::wstring src = FileExists(llc) ? llc : segio::FindAutosave(path);  // a project next to the file wins
        if (src.empty()) return {};
        return Colorize(segio::ReadProject(ReadAll(src), media_ ? media_->duration : 0));
    } catch (...) {
        return {};
    }
}

void Editor::SaveAutosave() {
    if (!s_.autoSaveProject || file_.empty() || !media_ || util::TestMode()) return;
    try {
        std::wstring p = segio::AutosavePath(file_);
        if (segs_.empty()) {
            // FastCut's old autosave is never touched, so an empty project here keeps it from coming back.
            if (FileExists(segio::LegacyAutosavePath(file_))) {
                SHCreateDirectoryExW(nullptr, util::DirName(p).c_str(), nullptr);
                WriteAll(p, segio::WriteProject(file_, segs_));
            } else {
                TryDelete(p);
            }
            return;
        }
        SHCreateDirectoryExW(nullptr, util::DirName(p).c_str(), nullptr);
        WriteAll(p, segio::WriteProject(file_, segs_));
    } catch (const EditError& e) {
        ff::AddLog(L"Autosave failed: " + e.msg);
    } catch (...) {
    }
}

void Editor::SaveProject() {
    if (file_.empty()) return;
    std::wstring p = ProjectNextTo(file_);
    if (!WriteAll(p, segio::WriteProject(file_, segs_))) {
        SetStatus(L"Could not save " + util::FileName(p));
        return;
    }
    SetStatus(L"Saved project " + util::FileName(p), p);
}

void Editor::LoadProjectDialog() {
    std::wstring f = PickFile(app_->hwnd_, false, file_.empty() ? L"" : util::DirName(file_), L"",
                              L"LosslessCut project\0*.llc\0All files\0*.*\0", L"llc");
    if (!f.empty()) LoadProjectFile(f);
}

bool Editor::LoadProjectFile(const std::wstring& llc) {
    try {
        std::string text = ReadAll(llc);
        auto root = segio::ParseJson(text);
        std::string mediaName = root ? root->Str("mediaFileName") : "";
        std::wstring mediaPath = mediaName.empty() ? L"" : util::DirName(llc) + L"\\" + util::Wide(mediaName);
        if (!mediaPath.empty() && FileExists(mediaPath) && util::Lower(mediaPath) != util::Lower(file_)) {
            // Opening restores it via LoadAutosave when it sits next to the media as <name>-proj.llc; otherwise it is
            // applied once the file has been probed (OnLoaded).
            pendingProject_ = llc;
            if (!active_) app_->DoAction(Act::EditMode);
            app_->OpenFiles({mediaPath}, false);
            return true;
        }
        if (!media_) {
            SetStatus(L"Open the media file for this project first.");
            return false;
        }
        PushUndo();
        segs_ = Colorize(segio::ReadProject(text, media_->duration));
        tl_.selected = segs_.empty() ? -1 : 0;
        SegmentsChanged();
        return true;
    } catch (const EditError& e) {
        SetStatus(L"Could not load project: " + e.msg);
    } catch (const std::exception& e) {
        SetStatus(L"Could not load project: " + util::Wide(e.what()));
    }
    return false;
}

void Editor::ImportSegments() {
    if (!media_) return;
    std::wstring f = PickFile(app_->hwnd_, false, util::DirName(file_), L"", segio::kImportFilter, nullptr);
    if (f.empty()) return;
    try {
        auto imported = segio::Import(f, media_->duration);
        if (imported.empty()) {
            SetStatus(L"No segments found in " + util::FileName(f));
            return;
        }
        PushUndo();
        segs_ = Colorize(std::move(imported));
        tl_.selected = 0;
        SegmentsChanged();
        SetStatus(L"Imported " + Plural(segs_.size(), L"segment", L"segments"));
    } catch (const EditError& e) {
        SetStatus(L"Import failed: " + e.msg);
    } catch (const std::exception& e) {
        SetStatus(L"Import failed: " + util::Wide(e.what()));
    }
}

void Editor::ExportSegmentsAs() {
    if (file_.empty() || segs_.empty()) {
        SetStatus(L"No segments to export.");
        return;
    }
    int index = 1;
    std::wstring f = PickFile(app_->hwnd_, true, util::DirName(file_), BaseName(file_) + L"-segments", segio::kExportFilter,
                              nullptr, &index);
    if (f.empty()) return;
    try {
        segio::Export(f, index, file_, segs_);
        SetStatus(L"Wrote " + util::FileName(f), f);
    } catch (const EditError& e) {
        SetStatus(L"Export failed: " + e.msg);
    }
}

// ------------------------------------------------------------------------------------------------ export & tools

std::vector<Marker> Editor::MarkersAfterCut(const std::vector<Segment>& list, const std::wstring& cutMode,
                                            double speed) const {
    // The pieces follow each other in list order; a keyframe cut starts each one at the keyframe before its start.
    std::vector<Marker> out;
    double offset = 0;
    for (const Segment& g : list) {
        double start = g.start;
        if (cutMode == L"keyframe" && !kf_.pts.empty()) {
            auto it = std::upper_bound(kf_.pts.begin(), kf_.pts.end(), g.start + 0.0005);
            if (it != kf_.pts.begin()) start = std::min(start, *(it - 1));
        }
        for (const Marker& mk : app_->markers_.Items())
            if (mk.t >= start && mk.t < g.end) out.push_back({(offset + mk.t - start) / speed, mk.label});
        offset += g.end - start;
    }
    std::sort(out.begin(), out.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    return out;
}

void Editor::Export(bool onlySelected, bool overwrite) {
    if (!media_ || jobRunning_) return;
    bool canOverwrite = media_->path.find(L"://") == std::wstring::npos;
    if (overwrite && !canOverwrite) {
        SetStatus(L"Only a file on disk can be overwritten.");
        return;
    }
    for (const auto& st : media_->streams)
        if (st.keep && st.encrypted) {
            SetStatus(L"This file is DRM-protected (copy-protected tracks), so it can't be cut or exported.", L"", 3);
            return;
        }
    std::vector<Segment> list;
    if (onlySelected) {
        for (int i : Targets())
            if (segs_[i].end > segs_[i].start) list.push_back(segs_[i]);
        if (list.empty()) {
            SetStatus(L"Select a segment to export first.");
            return;
        }
    } else {
        for (const auto& s : segs_)
            if (s.enabled && s.end > s.start) list.push_back(s);
    }
    if (list.empty()) {
        Segment whole;
        whole.end = media_->duration;
        list.push_back(whole);
    }
    double speed = 1;
    if ((s_.confirmExport || overwrite) && !util::TestMode()) {
        if (!dialogs::ExportOptions(app_->hwnd_, s_, (int)list.size(), Ext(media_->path), &speed,
                                    canOverwrite ? &overwrite : nullptr))
            return;
        SaveSettings();
        UpdateChecks();
    }
    if (overwrite && util::TestMode()) {
        // A test copy may only replace files inside its own FASTPLAY_EDIT_OUT folder, never the user's media.
        wchar_t env[MAX_PATH];
        std::wstring dir = GetEnvironmentVariableW(L"FASTPLAY_EDIT_OUT", env, MAX_PATH) ? util::Lower(util::FullPath(env)) : L"";
        if (dir.empty() || util::Lower(util::FullPath(media_->path)).rfind(dir + L"\\", 0) != 0) {
            SetStatus(L"Test mode: overwriting is only allowed inside FASTPLAY_EDIT_OUT.");
            return;
        }
    }
    if (overwrite && !util::TestMode() &&
        Box(L"Replace this file with the edited version?\n\n" + media_->path +
                L"\n\nTHE ORIGINAL CAN'T BE RECOVERED AFTERWARDS.",
            L"Overwrite the original", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    std::vector<int> streams;
    for (const auto& st : media_->streams)
        if (st.keep) streams.push_back(st.index);
    if (streams.empty()) {
        SetStatus(L"No tracks selected (Tracks tab).");
        return;
    }
    double seconds = 0;
    for (const auto& s : list) seconds += s.Duration();
    if (!overwrite) OfferFasterDrive(seconds / speed);
    int sel = (int)SendMessageW(rotationBox_, CB_GETCURSEL, 0, 0);
    int rotation = sel > 0 ? (sel - 1) * 90 : -1;
    auto m = media_;
    KeyframeIndex keyframes = kf_;
    EditSettings settings = JobSettings();
    bool verify = s_.verifyOutputs;
    if (overwrite) {
        // One lossless file in the source's own format: no Discord re-encode, size limit or audio mix, always checked.
        settings.overwriteSource = true;
        settings.merge = true;
        settings.format = L"same";
        settings.discordReady = false;
        settings.discordSizeMB = 0;
        settings.mixAudio = false;
        settings.copyToClipboard = false;
        verify = true;
    }
    RunJob(overwrite ? L"Overwrite" : L"Export", [this, m, keyframes, settings, streams, rotation, speed, list, verify](ProgressFn p, Cancel* ct) mutable {
        if (keyframes.pts.empty() && m->Video()) {
            p(-1, L"Waiting for the keyframe scan...");
            keyframes = Keyframes(*m, m->Video()->index, ct);
        }
        Exporter ex(*m, settings, streams, rotation, speed, keyframes, p);
        auto outs = ex.Run(list, ct);
        if (settings.overwriteSource) return ReplaceSource(ex, *m, list, settings, speed, outs, p, ct);
        std::wstring what = outs.size() == 1 ? L"Exported " + util::FileName(outs[0])
                                             : L"Exported " + std::to_wstring(outs.size()) + L" files to " + util::DirName(outs[0]);
        if (ex.Transcoded()) what += L" as H.264 for Discord";
        if (settings.copyToClipboard && !outs.empty()) {
            what += outs.size() == 1 ? L" (copied: Ctrl+V to paste it)" : L" (copied: Ctrl+V to paste them)";
            Post([this, outs] { util::CopyFilesToClipboard(app_->hwnd_, outs); });
        }
        if (verify) {
            p(-1, L"Checking outputs...");
            auto issues = ex.Verify(list, outs, ct);
            what += issues.empty() ? L" — checked OK" : L" — " + Plural(issues.size(), L"warning", L"warnings");
            if (!issues.empty()) ShowIssues(issues);
        }
        return std::make_pair(what, outs.empty() ? std::wstring() : outs[0]);
    }, true);
}

std::pair<std::wstring, std::wstring> Editor::ReplaceSource(Exporter& ex, const MediaInfo& m,
                                                            const std::vector<Segment>& list,
                                                            const EditSettings& settings, double speed,
                                                            const std::vector<std::wstring>& outs, ProgressFn p,
                                                            Cancel* ct) {
    if (outs.size() != 1) throw EditError(L"Overwrite expected one output file, got " + std::to_wstring(outs.size()));
    std::wstring src = m.path, edited = outs[0];
    // When the original stays, the edited file keeps a plain name next to it instead of the temporary one.
    auto keepEdited = [&] {
        std::wstring kept = UniquePath(util::DirName(src) + L"\\" + BaseName(src) + L"-edited" + Ext(edited));
        return MoveFileExW(edited.c_str(), kept.c_str(), 0) ? kept : edited;
    };
    p(-1, L"Checking the edited file...");
    auto issues = ex.Verify(list, outs, ct);
    if (!issues.empty()) {
        ShowIssues(issues);
        std::wstring kept = keepEdited();
        return {L"Not overwritten: the check found " + Plural(issues.size(), L"problem", L"problems") +
                    L". The edited version is " + util::FileName(kept),
                kept};
    }
    p(-1, L"Replacing the original...");
    // The UI thread notes where the markers land, then closes the file (playback, thumbnails, scanners).
    auto released = std::make_shared<std::promise<std::vector<Marker>>>();
    auto ready = released->get_future();
    Post([this, released, list, cutMode = settings.cutMode, speed] {
        std::vector<Marker> moved = MarkersAfterCut(list, cutMode, speed);
        app_->CloseFile();
        released->set_value(std::move(moved));
    });
    if (ready.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        std::wstring kept = keepEdited();
        return {L"Not overwritten (FastPlay didn't close the file). The edited version is " + util::FileName(kept), kept};
    }
    std::vector<Marker> moved = ready.get();
    std::wstring err = SwapInEdited(src, edited);
    std::wstring kept = err.empty() ? src : keepEdited();
    Post([this, src, moved, ok = err.empty()] {
        if (ok) {
            DeleteFileW(segio::AutosavePath(src).c_str());  // the old cut's segments don't fit the new file
            MarkerList ml;
            ml.Load(src);
            ml.RemoveAll();
            for (const Marker& mk : moved) {
                int i = ml.Add(mk.t, 0, nullptr);
                if (i >= 0 && !mk.label.empty()) ml.Rename(i, mk.label);
            }
        }
        app_->Open(src);
    });
    util::TimingLog(("overwrite: " + (err.empty() ? std::string("replaced ") + util::Utf8(src) : util::Utf8(err))).c_str());
    if (!err.empty()) return {L"Not overwritten: " + err + L". The edited version is " + util::FileName(kept), kept};
    return {L"Overwrote " + util::FileName(src) + L" with the edited version", src};
}

void Editor::OfferFasterDrive(double outputSeconds) {
    // Once: a big export that reads and writes the same hard drive (the slowest case, ~3x slower than writing to another
    // drive) offers to export to the roomiest SSD instead. The answer is remembered either way.
    if (!media_ || s_.sameDiskTipAnswered || util::TestMode()) return;
    uint64_t estimate = (uint64_t)(outputSeconds * std::max<long long>(media_->bitRate, 1) / 8);
    if (estimate < (1ull << 30) || !SameSpinningDisk(media_->path, OutDir())) return;
    std::wstring fast = FastestDriveWithRoom(estimate * 3);
    if (fast.empty()) return;
    std::wstring target = fast + L"FastPlay exports";
    std::wstring drive = util::FullPath(media_->path).substr(0, 2);
    wchar_t gb[32];
    swprintf(gb, 32, L"%.1f GB", estimate / 1e9);
    std::wstring text = std::wstring(L"This export (") + gb + L") is read from and written to the same hard drive (" + drive +
                        L"), which makes the drive jump back and forth and is about 3x slower than writing to another drive.\n\n"
                        L"Export to " + target + L" (an SSD) instead? You can change the output folder any time in Settings.";
    int answer = Box(text.c_str(), L"Faster export", MB_YESNO | MB_ICONQUESTION);
    s_.sameDiskTipAnswered = true;
    if (answer == IDYES) {
        CreateDirectoryW(target.c_str(), nullptr);
        s_.outDir = target;
    }
    SaveSettings();
}

void Editor::Snapshot() {
    if (!media_ || !media_->Video()) return;
    auto m = media_;
    double t = Pos();
    std::wstring dir = OutDir(), fmt = s_.snapshotFormat, label = FmtT(t);
    RunJob(L"Snapshot", [m, t, dir, fmt, label](ProgressFn p, Cancel* ct) {
        p(-1, L"Saving full-resolution snapshot at " + label + L"...");
        std::wstring path = tools::Snapshot(*m, t, dir, fmt, ct);
        return std::make_pair(L"Snapshot saved: " + util::FileName(path), path);
    });
}

void Editor::SnapshotSegments() {
    if (!media_ || !media_->Video() || segs_.empty()) return;
    auto m = media_;
    std::vector<double> times;
    for (const auto& s : segs_)
        if (s.enabled) times.push_back(s.start);
    std::wstring dir = OutDir(), fmt = s_.snapshotFormat;
    RunJob(L"Snapshots", [m, times, dir, fmt](ProgressFn p, Cancel* ct) {
        std::wstring last;
        for (size_t i = 0; i < times.size(); i++) {
            p((double)i / times.size(), L"Snapshot " + std::to_wstring(i + 1) + L"/" + std::to_wstring(times.size()));
            last = tools::Snapshot(*m, times[i], dir, fmt, ct);
        }
        return std::make_pair(L"Saved " + std::to_wstring(times.size()) + L" snapshots", last);
    });
}

void Editor::SnapshotEvery() {
    if (!media_ || !media_->Video()) return;
    std::wstring input = L"1";
    double interval;
    if (!dialogs::Prompt(app_->hwnd_, L"Snapshots", L"Interval in seconds (within the selected segment, or the whole file)", &input))
        return;
    if (!TryParseTime(util::Trim(input), &interval) || interval <= 0) return;
    auto m = media_;
    double a = 0, b = m->duration;
    if (const Segment* s = Sel()) {
        a = s->start;
        b = s->end;
    }
    if ((b - a) / interval > 20000) {
        SetStatus(L"That would create too many images.");
        return;
    }
    std::wstring dir = OutDir(), fmt = s_.snapshotFormat;
    RunJob(L"Snapshots", [m, interval, a, b, dir, fmt](ProgressFn p, Cancel* ct) {
        tools::SnapshotsEvery(*m, interval, a, b, dir, fmt, p, ct);
        return std::make_pair(L"Saved snapshots to " + dir, std::wstring());
    });
}

void Editor::ExtractTracks() {
    if (!media_) return;
    auto m = media_;
    std::wstring dir = OutDir();
    RunJob(L"Extract tracks", [m, dir](ProgressFn p, Cancel* ct) {
        auto outs = tools::ExtractTracks(*m, m->streams, dir, p, ct);
        return std::make_pair(L"Extracted " + Plural(outs.size(), L"track", L"tracks"), outs.empty() ? std::wstring() : outs.back());
    });
}

void Editor::ConcatFiles() {
    std::vector<std::wstring> files = app_->plist_;
    if (files.size() < 2) {
        SetStatus(L"Add two or more files on the Files tab (same codecs), in the order to join them.");
        return;
    }
    // A lossless join needs matching codec parameters; clips that differ are re-encoded instead.
    auto mismatch = std::make_shared<std::wstring>();
    RunJob(L"Checking files", [files, mismatch](ProgressFn p, Cancel* ct) {
        *mismatch = tools::ConcatMismatch(files, p, ct);
        return std::make_pair(mismatch->empty() ? L"Checked " + std::to_wstring(files.size()) + L" files: they can be joined losslessly"
                                                : L"Files differ: " + *mismatch,
                              std::wstring());
    }, false, [this, files, mismatch](bool ok) {
        if (!ok) return;  // cancelled or a file could not be read
        bool reencode = !mismatch->empty();
        if (reencode) {
            std::wstring q = *mismatch + L", so they can't be joined losslessly (the later files would play broken or not at all).\n\n"
                                         L"Re-encode them into one H.264 MP4 instead? This uses the GPU when it can.";
            if (Box(q.c_str(), L"Merge files", MB_YESNO | MB_ICONINFORMATION) != IDYES) return;
        }
        std::wstring ext = reencode ? L".mp4" : Ext(files[0]);
        std::wstring filter = reencode ? std::wstring(L"MP4|*.mp4|MKV|*.mkv|MOV|*.mov|")
                                       : L"Same as input|*" + ext + L"|MP4|*.mp4|MKV|*.mkv|MOV|*.mov|";
        for (auto& c : filter)
            if (c == L'|') c = L'\0';
        filter.push_back(L'\0');
        std::wstring out = PickFile(app_->hwnd_, true, util::DirName(files[0]), BaseName(files[0]) + L"-merged" + ext, filter.c_str(),
                                    ext.c_str() + 1);
        if (out.empty()) return;
        int sizeMB = s_.discordReady ? s_.discordSizeMB : 0;
        RunJob(L"Merge files", [files, out, reencode, sizeMB](ProgressFn p, Cancel* ct) {
            tools::ConcatFiles(files, out, reencode, sizeMB, p, ct);
            return std::make_pair(L"Merged " + std::to_wstring(files.size()) + L" files into " + util::FileName(out) +
                                      (reencode ? L" (re-encoded)" : L""),
                                  out);
        });
    });
}

void Editor::DownloadUrl() {
    std::wstring url;
    if (!dialogs::Prompt(app_->hwnd_, L"Download", L"URL (HLS .m3u8 or any stream ffmpeg can read)", &url)) return;
    url = util::Trim(url);
    if (url.empty()) return;
    std::wstring filter = L"MP4|*.mp4|MKV|*.mkv|TS|*.ts|";
    for (auto& c : filter)
        if (c == L'|') c = L'\0';
    filter.push_back(L'\0');
    std::wstring out = PickFile(app_->hwnd_, true, L"", L"download.mp4", filter.c_str(), L"mp4");
    if (out.empty()) return;
    RunJob(L"Download", [url, out](ProgressFn p, Cancel* ct) {
        tools::Download(url, out, p, ct);
        return std::make_pair(L"Downloaded " + util::FileName(out), out);
    }, false, [this, out](bool ok) {
        if (ok) app_->OpenFiles({out}, false);
    });
}

void Editor::DetectBlack() {
    if (!media_ || !media_->Video()) return;
    std::wstring input = L"0.5, 0.10";
    if (!dialogs::Prompt(app_->hwnd_, L"Detect black scenes", L"Minimum duration (s), pixel threshold (0-1)", &input)) return;
    auto [d, th] = Pair(input, 0.5, 0.10);
    auto m = media_;
    auto found = std::make_shared<std::vector<std::pair<double, double>>>();
    RunJob(L"Black detection", [m, d = d, th = th, found](ProgressFn p, Cancel* ct) {
        *found = tools::DetectBlack(*m, d, th, p, ct);
        return std::make_pair(L"Found " + Plural(found->size(), L"black scene", L"black scenes"), std::wstring());
    }, false, [this, m, found](bool ok) {
        if (ok && media_ == m) ApplyDetected(*found);
    });
}

void Editor::DetectSilence() {
    if (!media_ || !media_->Audio()) return;
    std::wstring input = L"-50, 1";
    if (!dialogs::Prompt(app_->hwnd_, L"Detect silence", L"Noise level (dB), minimum duration (s)", &input)) return;
    auto [db, d] = Pair(input, -50, 1);
    auto m = media_;
    auto found = std::make_shared<std::vector<std::pair<double, double>>>();
    RunJob(L"Silence detection", [m, db = db, d = d, found](ProgressFn p, Cancel* ct) {
        *found = tools::DetectSilence(*m, db, d, p, ct);
        return std::make_pair(L"Found " + Plural(found->size(), L"silent part", L"silent parts"), std::wstring());
    }, false, [this, m, found](bool ok) {
        if (ok && media_ == m) ApplyDetected(*found);
    });
}

void Editor::DetectScenes() {
    if (!media_ || !media_->Video()) return;
    std::wstring input = L"0.3";
    if (!dialogs::Prompt(app_->hwnd_, L"Detect scene changes", L"Threshold (0-1, lower = more cuts)", &input)) return;
    double th = Pair(input, 0.3, 0).first;
    auto m = media_;
    auto cuts = std::make_shared<std::vector<double>>();
    RunJob(L"Scene detection", [m, th, cuts](ProgressFn p, Cancel* ct) {
        *cuts = tools::DetectScenes(*m, th, p, ct);
        return std::make_pair(L"Found " + Plural(cuts->size(), L"scene change", L"scene changes"), std::wstring());
    }, false, [this, m, cuts](bool ok) {
        if (!ok || media_ != m) return;
        std::vector<double> pts = {0};
        for (double c : *cuts)
            if (c > 0.05 && c < m->duration - 0.05) pts.push_back(c);
        pts.push_back(m->duration);
        std::vector<std::pair<double, double>> ranges;
        for (size_t i = 0; i + 1 < pts.size(); i++) ranges.push_back({pts[i], pts[i + 1]});
        ApplyDetected(ranges);
    });
}

void Editor::ApplyDetected(const std::vector<std::pair<double, double>>& found) {
    if (found.empty() || !media_) return;
    PushUndo();
    segs_.clear();
    for (const auto& f : found)
        if (f.second > f.first) segs_.push_back(NewSeg(f.first, std::min(f.second, media_->duration)));
    tl_.selected = segs_.empty() ? -1 : 0;
    SegmentsChanged();
}

void Editor::ShowSettings() {
    if (!dialogs::Settings(app_->hwnd_, s_)) return;
    SaveSettings();
    ApplyTimelineHeight();
    LoadWaveIfNeeded(false);
    UpdateChecks();
}

void Editor::BatchExport() {
    std::vector<std::wstring> files = app_->plist_;
    if (files.empty() && !app_->path_.empty()) files.push_back(app_->path_);
    if (files.empty()) {
        SetStatus(L"Add files on the Files tab first.");
        return;
    }
    std::vector<Segment> templ;
    for (const auto& s : segs_)
        if (s.enabled && s.end > s.start) templ.push_back(s);
    if (!dialogs::Batch(app_->hwnd_, s_, (int)files.size(), (int)templ.size())) return;
    SaveSettings();
    if (s_.batchMode == L"same" && templ.empty()) {
        SetStatus(L"The current file has no included segments to reuse.");
        return;
    }
    EditSettings settings = JobSettings();
    RunJob(L"Batch export", [this, files, templ, settings](ProgressFn p, Cancel* ct) {
        size_t done = 0, warnings = 0;
        std::vector<std::wstring> problems;
        std::wstring last;
        for (size_t f = 0; f < files.size(); f++) {
            const std::wstring& path = files[f];
            double base = (double)f / files.size(), span = 1.0 / files.size();
            std::wstring prefix = L"[" + std::to_wstring(f + 1) + L"/" + std::to_wstring(files.size()) + L"] ";
            ProgressFn fp = [p, base, span, prefix](double frac, const std::wstring& msg) {
                p(base + span * std::clamp(frac, 0.0, 1.0), prefix + msg);
            };
            try {
                MediaInfo m = Probe(path, ct);
                KeyframeIndex kfi = m.Video() ? Keyframes(m, m.Video()->index, ct) : KeyframeIndex{};
                // Segments for this file: the current ones, its own saved ones, or a trim (MainForm.BatchSegments).
                std::vector<Segment> list;
                if (settings.batchMode == L"trim") {
                    Segment s;
                    s.start = settings.batchTrimStart;
                    s.end = m.duration - settings.batchTrimEnd;
                    list.push_back(s);
                } else if (settings.batchMode == L"saved") {
                    std::wstring llc = ProjectNextTo(path);
                    std::wstring src = FileExists(llc) ? llc : segio::FindAutosave(path);
                    if (!src.empty()) list = segio::ReadProject(ReadAll(src), m.duration);
                    if (list.empty()) {
                        Segment s;
                        s.end = m.duration;
                        list.push_back(s);
                    }
                } else {
                    list = templ;
                }
                for (auto& x : list) x.end = std::min(x.end, m.duration);
                list.erase(std::remove_if(list.begin(), list.end(), [](const Segment& x) { return !x.enabled || x.start >= x.end - 0.01; }),
                           list.end());
                if (list.empty()) {
                    problems.push_back(util::FileName(path) + L": nothing left to export after trimming");
                    continue;
                }
                std::vector<int> streams;
                for (const auto& st : m.streams)
                    if (st.keep) streams.push_back(st.index);
                Exporter ex(m, settings, streams, -1, 1, kfi, fp);
                auto outs = ex.Run(list, ct);
                if (!outs.empty()) last = outs.back();
                done++;
                if (settings.verifyOutputs) {
                    fp(1, L"checking outputs...");
                    auto issues = ex.Verify(list, outs, ct);
                    warnings += issues.size();
                    problems.insert(problems.end(), issues.begin(), issues.end());
                }
            } catch (const Cancelled&) {
                throw;
            } catch (const EditError& e) {
                std::wstring msg = e.msg;
                size_t nl = msg.find_last_of(L'\n');
                problems.push_back(util::FileName(path) + L": " + (nl == std::wstring::npos ? msg : msg.substr(nl + 1)));
            }
        }
        if (!problems.empty()) ShowIssues(problems);
        std::wstring msg = L"Batch: exported " + std::to_wstring(done) + L" of " + std::to_wstring(files.size()) + L" files";
        msg += problems.empty() ? (settings.verifyOutputs ? L" — checked OK" : L"") : L" — " + Plural(problems.size(), L"problem", L"problems");
        return std::make_pair(msg, last);
    }, true);
}

}  // namespace edit
