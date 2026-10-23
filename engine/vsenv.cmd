@echo off
rem Sets up the 32-bit (x86) Visual C++ tools for the script that calls it. Nothing to do in a Developer Command Prompt
rem for x86; otherwise vswhere finds the newest Visual Studio (or Build Tools) that has the C++ tools.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio was not found. Install it, or its Build Tools, with "Desktop development with C++".
  exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%I in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (
  echo Visual Studio has no C++ tools: add "Desktop development with C++" in the Visual Studio Installer.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
  echo vcvarsall.bat x86 of Visual Studio failed.
  exit /b 1
)
exit /b 0
