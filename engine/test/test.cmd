@echo off
rem Builds hooks_test.exe, puts the freshly built version.dll (engine\build.cmd) next to it and runs it against a copy
rem of the game's engine and its DLLs in build\input (no game start): the patches (the engine's present and an overlay
rem like Steam's among them), then an engine of another build (left alone), loads_without_vsync=0 (the engine's present
rem left as it is), the engine unloaded as at the end of the game (version.dll has to stay; 31 s), the main thread out
rem of stack (logged, with a minidump), eleven inis as players may write them (the settings line and the "ini:" lines
rem version.dll logs), a game folder with Cyrillic letters (its name in the log, in UTF-8), and one whose path is too
rem long for the log's name to fit in MAX_PATH (the engine still has to load), and the DLL as an ASI plugin loaded
rem before the engine (all the checks of the first run again, with its settings next to it, only next to the program as
rem 0.5.2 had them, nowhere and in both places; the log next to the settings that count).
rem The game folder is only read: SHOGUN2_DIR, by default Steam's usual one (the Steam version 1.1.0 of the game). The
rem minidumps of the tests go to build\dumps (SHOGUN2_ENGINE_DUMP_DIR), not to the game's crash_dumps folder.
rem Extra arguments go to the first run:
rem [number_of_threads] [--crash]
setlocal EnableExtensions DisableDelayedExpansion
call "%~dp0..\vsenv.cmd"
if errorlevel 1 exit /b 1
pushd "%~dp0" || exit /b 1
if not defined SHOGUN2_DIR set "SHOGUN2_DIR=C:\Program Files (x86)\Steam\steamapps\common\Total War SHOGUN 2"
if not exist "%SHOGUN2_DIR%\empire.retail.dll" (
  echo Total War: SHOGUN 2 was not found. Set SHOGUN2_DIR to the game folder, the one with shogun2.exe.
  exit /b 1
)
if not exist ..\build\version.dll (
  echo Build version.dll first: engine\build.cmd
  exit /b 1
)
if not exist build\input mkdir build\input
for %%F in (empire.retail.dll shogun2.exe binkw32.dll libeay32.dll mss32.dll ssleay32.dll steam_api.dll tbb.dll tbbmalloc.dll) do (
  copy /y "%SHOGUN2_DIR%\%%F" build\input\ >nul || (echo Could not copy %%F from the game folder & exit /b 1)
)
cl /nologo /O2 /EHsc /std:c++17 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /Fo:build\ hooks_test.cpp /Fe:build\hooks_test.exe ^
   /link shell32.lib user32.lib /NOLOGO
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
copy /y ..\build\version.dll build\ >nul || (echo Could not copy version.dll next to hooks_test.exe & exit /b 1)
set "INPUT=%~dp0build\input"
set "PACK=%SHOGUN2_DIR%\data\boot.pack"
set "SHOGUN2_ENGINE_DUMP_DIR=%~dp0build\dumps"
set FAILED=0
build\hooks_test.exe "%INPUT%" "%PACK%" %*
if %errorlevel% neq 0 set FAILED=1
echo.
echo ==== another build of the engine
build\hooks_test.exe "%INPUT%" "%PACK%" --other-build
if %errorlevel% neq 0 set FAILED=1
echo.
echo ==== loads_without_vsync=0: the engine's present left as it is
build\hooks_test.exe "%INPUT%" "%PACK%" --no-vsync-hook
if %errorlevel% neq 0 set FAILED=1
echo.
echo ==== the engine unloaded, as at the end of the game
build\hooks_test.exe "%INPUT%" "%PACK%" --unload
if %errorlevel% neq 0 (echo FAIL  the process ended with %errorlevel% & set FAILED=1)
findstr /C:"unloaded (the process ends)" build\shogun2_engine.log
if %errorlevel% neq 0 (echo FAIL  version.dll was not there to the end of the process & set FAILED=1)
echo.
echo ==== the main thread out of stack (the process ends there)
build\hooks_test.exe "%INPUT%" "%PACK%" --stack-overflow
build\hooks_test.exe --check-dump
if %errorlevel% neq 0 set FAILED=1
echo.
echo ==== settings written as a player may write them
for /L %%N in (0,1,10) do (
  build\hooks_test.exe "%INPUT%" "%PACK%" --ini-case %%N
  if errorlevel 1 set FAILED=1
)
echo.
echo ==== a game folder with Cyrillic letters
build\hooks_test.exe "%INPUT%" "%PACK%" --cyrillic-folder
if %errorlevel% neq 0 set FAILED=1
echo.
echo ==== a game folder with a long path
build\hooks_test.exe "%INPUT%" "%PACK%" --long-folder
if %errorlevel% neq 0 set FAILED=1
echo.
echo ==== the DLL as an ASI plugin, loaded before the engine
build\hooks_test.exe "%INPUT%" "%PACK%" --asi-plugin
if %errorlevel% neq 0 set FAILED=1
echo.
popd
if %FAILED%==1 (echo SOME TESTS FAILED & exit /b 1)
echo ALL PASSED
