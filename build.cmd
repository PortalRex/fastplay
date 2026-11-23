@echo off
rem Builds dist\fastplay.exe (and tools\bench\fpbench.exe) with MSVC. Run from any prompt; sets up vcvars itself.
setlocal
cd /d "%~dp0"
if defined VCINSTALLDIR goto :have_vc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vc
if not exist build mkdir build
if not exist dist mkdir dist

set CFLAGS=/nologo /utf-8 /O2 /GL /MT /EHsc /std:c++17 /permissive- /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Ithird_party\mpv\include /Fobuild\ /Fdbuild\
rc /nologo /fo build\fastplay.res res\fastplay.rc || exit /b 1
cl %CFLAGS% src\*.cpp build\fastplay.res /Fe:dist\fastplay.exe /link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
  user32.lib gdi32.lib shell32.lib comdlg32.lib advapi32.lib ole32.lib comctl32.lib delayimp.lib ^
  /DELAYLOAD:shell32.dll /DELAYLOAD:comdlg32.dll /DELAYLOAD:ole32.dll /DELAYLOAD:comctl32.dll || exit /b 1

rem libmpv next to the exe (hard link when possible: same bytes, no extra 120 MB on disk).
if not exist dist\libmpv-2.dll (
  mklink /H dist\libmpv-2.dll third_party\mpv\libmpv-2.dll >nul 2>&1 || copy /y third_party\mpv\libmpv-2.dll dist\ >nul
)

if exist tools\bench\fpbench.cpp (
  cl /nologo /O2 /MT /EHsc /std:c++17 /permissive- /W3 /DUNICODE /D_UNICODE /await /Fobuild\ /Fdbuild\ tools\bench\fpbench.cpp ^
    /Fe:tools\bench\fpbench.exe /link d3d11.lib dxgi.lib windowsapp.lib user32.lib ole32.lib || exit /b 1
)
echo Built dist\fastplay.exe
