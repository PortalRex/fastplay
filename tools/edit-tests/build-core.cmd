@echo off
rem Builds the edit-mode core test harness:  tools\edit-tests\build-core.cmd   ->  build\edit-tests\core\test_core.exe
setlocal
cd /d "%~dp0..\.."
if defined VCINSTALLDIR goto :have_vc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vc
if not exist build\edit-tests\core mkdir build\edit-tests\core
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /permissive- /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
  /Ithird_party\mpv\include /Fobuild\edit-tests\core\ /Fdbuild\edit-tests\core\ ^
  src\util.cpp src\settings.cpp src\mp4index.cpp src\mkvindex.cpp src\edit\common.cpp src\edit\ffrun.cpp src\edit\media.cpp ^
  src\edit\segio.cpp tools\edit-tests\test_core.cpp ^
  /Fe:build\edit-tests\core\test_core.exe /link user32.lib shell32.lib advapi32.lib ole32.lib || exit /b 1
echo Built build\edit-tests\core\test_core.exe
