@echo off
rem Builds tex_test.exe and checks the texture fast path of the freshly built version.dll (engine\build.cmd): first on
rem broken and forged .dds headers, which it has to leave to D3DX, then against D3DX on a sample of the game's own
rem textures. On the first run tools\dds_sample.py (Python 3) picks the sample from the game's .pack files into
rem build\tex_sample; the game folder is only read: SHOGUN2_DIR, by default Steam's usual one. Needs the DirectX June
rem 2010 runtime (d3dx11_42.dll), which the game installs.
setlocal EnableExtensions DisableDelayedExpansion
call "%~dp0..\vsenv.cmd"
if errorlevel 1 exit /b 1
pushd "%~dp0" || exit /b 1
if not defined SHOGUN2_DIR set "SHOGUN2_DIR=C:\Program Files (x86)\Steam\steamapps\common\Total War SHOGUN 2"
if not exist "%SHOGUN2_DIR%\data" (
  echo Total War: SHOGUN 2 was not found. Set SHOGUN2_DIR to the game folder, the one with shogun2.exe.
  exit /b 1
)
if not exist ..\build\version.dll (
  echo Build version.dll first: engine\build.cmd
  exit /b 1
)
if not exist build mkdir build
cl /nologo /O2 /EHsc /std:c++17 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /Fo:build\ tex_test.cpp /Fe:build\tex_test.exe /link /NOLOGO
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
copy /y ..\build\version.dll build\fixes.dll >nul || (echo Could not copy version.dll & exit /b 1)
rem the sample counts once dds_sample.py has finished it (build\tex_sample\complete); a broken one is made again
if not exist build\tex_sample\complete (
  if exist build\tex_sample rmdir /s /q build\tex_sample
  python -I ..\..\tools\dds_sample.py "%SHOGUN2_DIR%\data" build\tex_sample
  if errorlevel 1 (
    echo The texture sample could not be made: tools\dds_sample.py needs Python 3.
    exit /b 1
  )
  type nul > build\tex_sample\complete
)
build\tex_test.exe "%~dp0build\tex_sample" "%~dp0build\fixes.dll"
set RESULT=%errorlevel%
popd
exit /b %RESULT%
