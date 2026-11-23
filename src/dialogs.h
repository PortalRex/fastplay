// Tiny modal dialogs built from in-memory templates (no .rc dialog resources).
#pragma once
#include "util.h"

// One-line text input. Returns false on cancel.
bool InputBox(HWND owner, const wchar_t* title, const wchar_t* prompt, std::wstring* value);
// File pickers (comdlg32 is delay-loaded, so it costs nothing until used).
std::wstring PickOpenFile(HWND owner, const std::wstring& initialDir);
std::wstring PickSaveFile(HWND owner, const std::wstring& initialDir, const std::wstring& name, const wchar_t* filter,
                          const wchar_t* defExt);
