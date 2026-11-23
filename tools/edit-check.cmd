@echo off
rem Compile-checks some edit-mode sources (no link) with FastPlay's flags:  tools\edit-check.cmd <tag> src\edit\a.cpp ...
rem Objects go to build\edit-check\<tag>\ so parallel checks don't collide.
setlocal
cd /d "%~dp0.."
if defined VCINSTALLDIR goto :have_vc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vc
set TAG=%1
shift
if not exist build\edit-check\%TAG% mkdir build\edit-check\%TAG%
set FILES=
:collect
if "%~1"=="" goto :go
set FILES=%FILES% %1
shift
goto :collect
:go
cl /nologo /c /utf-8 /O2 /MT /EHsc /std:c++17 /permissive- /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
  /Ithird_party\mpv\include /Fobuild\edit-check\%TAG%\ %FILES%
