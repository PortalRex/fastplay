// Dialogs.cs: Edit-mode dialogs as Win32 modal dialogs built from in-memory templates (dark where the system lets
// us, Segoe UI 9, per-monitor DPI via DS_SETFONT scaling). All return false on Cancel and leave inputs untouched.
#pragma once
#include "common.h"

namespace edit::dialogs {

// SegmentDialog: label, start, end (time text via fmt/parse), tags, include.
bool EditSegment(HWND owner, Segment& seg, const std::function<std::wstring(double)>& fmt,
                 const std::function<bool(const std::wstring&, double*)>& parse);

// ExportDialog: format, cut mode, merge, name/merge templates, output folder (Browse...), speed (FPS change),
// preserve metadata/chapters, chapters from segments, avoid negative timestamps, fast start, verify, confirm,
// Discord-ready + size limit. Writes the choices into s and *speed.
extern const wchar_t* const kFormats[];  // "same", "mp4", "mkv", ... (ExportDialog.Formats)
extern const int kFormatCount;
bool ExportOptions(HWND owner, EditSettings& s, int segCount, const std::wstring& inputExt, double* speed);

// SettingsDialog: seek steps, frame jump, nudge frames, snapshot format, thumbnails/waveform/keyframes rows,
// auto-save, confirm/verify, output folder.
bool Settings(HWND owner, EditSettings& s);

// BatchDialog: same segments for all / each file's saved segments / trim N s from start and M s from end.
bool Batch(HWND owner, EditSettings& s, int files, int currentSegments);

// Numbers / text prompts used by Divide, Shift, Snapshots every N s, detection parameters, timecode offset.
bool Prompt(HWND owner, const wchar_t* title, const wchar_t* label, std::wstring* value);
// Two-field prompt (e.g. silence: noise dB + minimum duration). Labels/values parallel.
bool Prompt2(HWND owner, const wchar_t* title, const wchar_t* label1, std::wstring* value1, const wchar_t* label2,
             std::wstring* value2);

// TextViewer: read-only, resizable, monospace, copyable; liveSource (optional) refreshes it once a second (FFmpeg log).
void TextViewer(HWND owner, const wchar_t* title, const std::wstring& text,
                std::function<std::wstring()> liveSource = {});

// Folder picker (IFileDialog, FOS_PICKFOLDERS). Empty on cancel.
std::wstring PickFolder(HWND owner, const std::wstring& initial);

}  // namespace edit::dialogs
