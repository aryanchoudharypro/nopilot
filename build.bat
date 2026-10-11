@echo off
setlocal
cd /d "%~dp0"

if defined INCLUDE goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto notools

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto notools

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto notools

:build
cl /nologo /O1 /W3 /GS- /GR- nopilot.cpp /Fe:nopilot.exe /link /SUBSYSTEM:WINDOWS /NODEFAULTLIB /ENTRY:Entry /OPT:REF /OPT:ICF kernel32.lib user32.lib winmm.lib advapi32.lib shell32.lib
exit /b %errorlevel%

:notools
echo Could not find the MSVC C++ build tools. Install "Desktop development with C++" from the Visual Studio installer.
exit /b 1
