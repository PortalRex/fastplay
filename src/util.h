// Small shared helpers: string conversion, paths, timing.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace util {

std::string Utf8(const std::wstring& w);
std::wstring Wide(const char* s);
inline std::wstring Wide(const std::string& s) { return Wide(s.c_str()); }

std::wstring ExeDir();
// Folder for settings and caches: next to the exe when fastplay.ini exists there (portable), else %APPDATA%\FastPlay.
const std::wstring& DataDir();
std::wstring LocalCacheDir();  // %LOCALAPPDATA%\FastPlay (shader cache)
std::wstring FullPath(const std::wstring& p);
std::wstring FileName(const std::wstring& p);
std::wstring DirName(const std::wstring& p);
std::wstring Lower(std::wstring s);
std::wstring Trim(const std::wstring& s);

// "1:02:03" or "02:03"; with millis "1:02:03.456".
std::wstring FormatTime(double sec, bool millis = false);
bool ParseTime(const std::wstring& s, double* out);

// Milliseconds since this process was created (includes loader time before main).
double MsSinceProcessStart();
void TimingLog(const char* what);  // appends to FASTPLAY_TIMING file when set
bool TestMode();                   // FASTPLAY_TEST=1 or visible: muted, never activates, no settings/history writes
bool TestOffScreen();              // FASTPLAY_TEST=1: and placed off every screen
// Test copies are driven with mouse window messages (tools\fpmouse.ps1), without a real button press, so Windows
// takes the mouse capture straight back; drags then keep going until the WM_LBUTTONUP the test sends.
inline bool KeepDragOnCaptureLoss() { return TestMode(); }
// Keyboard focus to h only when its window is already the active one. SetFocus on an inactive window activates it,
// which pulls a hidden test copy to the foreground; a real click has activated the window before the handler runs.
inline void FocusIfActive(HWND h) {
    if (GetActiveWindow() == GetAncestor(h, GA_ROOT)) SetFocus(h);
}
// FASTPLAY_INSTANCE=<name>: separate single-instance namespace (window class + mutex), so a test warm instance
// never talks to the user's real FastPlay. Empty normally.
const std::wstring& InstanceSuffix();
std::wstring MainClassName();

// True when the file's volume sits on a drive with a seek penalty (spinning disk).
bool IsOnSpinningDisk(const std::wstring& path);

// Puts files on the clipboard the way Explorer's Copy does (CF_HDROP + "copy" drop effect), so Ctrl+V attaches
// them in Discord or pastes them in Explorer. Test mode only logs them: the user's clipboard is never touched.
bool CopyFilesToClipboard(HWND owner, const std::vector<std::wstring>& files);

}  // namespace util
