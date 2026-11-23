@echo off
rem Builds and runs the Edit-mode dialog test (no on-screen windows): tools\edit-tests\build-dialogs.cmd
rem Images go to bench\edit-tests\dialogs\ (delete them when done).
setlocal
cd /d "%~dp0..\.."
if defined VCINSTALLDIR goto :have_vc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vc
if not exist build\edit-tests\dialogs mkdir build\edit-tests\dialogs
if not exist bench\edit-tests\dialogs mkdir bench\edit-tests\dialogs
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /permissive- /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
  /Fobuild\edit-tests\dialogs\ /Fdbuild\edit-tests\dialogs\ tools\edit-tests\test_dialogs.cpp src\edit\eddialogs.cpp ^
  /Fe:build\edit-tests\dialogs\test_dialogs.exe /link /SUBSYSTEM:CONSOLE ^
  user32.lib gdi32.lib advapi32.lib ole32.lib shell32.lib || exit /b 1
build\edit-tests\dialogs\test_dialogs.exe "%CD%\bench\edit-tests\dialogs"
