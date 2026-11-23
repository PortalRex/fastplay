// Tiny modal dialogs built from in-memory templates (no .rc dialog resources).
#pragma once
#include "util.h"

// One-line text input. Returns false on cancel.
bool InputBox(HWND owner, const wchar_t* title, const wchar_t* prompt, std::wstring* value);
// File pickers (comdlg32 is delay-loaded, so it costs nothing until used).
std::wstring PickOpenFile(HWND owner, const std::wstring& initialDir);
std::vector<std::wstring> PickOpenFiles(HWND owner, const std::wstring& initialDir);  // multi-select
// Read-only text in a resizable-looking modal box (help, properties).
void ShowTextBox(HWND owner, const wchar_t* title, const std::wstring& text);
std::wstring PickSaveFile(HWND owner, const std::wstring& initialDir, const std::wstring& name, const wchar_t* filter,
                          const wchar_t* defExt);
// General picker: any filter (double-NUL list); filterIndex in/out is 1-based like OPENFILENAME.
std::wstring PickFile(HWND owner, bool save, const std::wstring& initialDir, const std::wstring& name, const wchar_t* filter,
                      const wchar_t* defExt, int* filterIndex = nullptr);
