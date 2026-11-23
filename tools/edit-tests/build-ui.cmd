@echo off
rem Builds and runs the off-screen Timeline / SegmentCards render test:  tools\edit-tests\build-ui.cmd
rem Images land in bench\edit-tests\ui\ (BMP); the test window sits beyond every monitor and is never activated.
setlocal
cd /d "%~dp0..\.."
where cl >nul 2>&1 && goto :have_vc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vc
if not exist build\edit-tests mkdir build\edit-tests
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
  /Fobuild\edit-tests\ /Fdbuild\edit-tests\ tools\edit-tests\test_ui.cpp /Fe:build\edit-tests\test_ui.exe ^
  /link user32.lib gdi32.lib || exit /b 1
build\edit-tests\test_ui.exe
