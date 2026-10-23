@echo off
rem Builds the 32-bit probe that runs a piece of the engine outside the game (topo_probe.cpp). Run from anywhere.
call "%~dp0..\..\engine\vsenv.cmd"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /EHsc /std:c++17 /W3 /MT /D_CRT_SECURE_NO_WARNINGS topo_probe.cpp /Fe:topo_probe.exe
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
del /q *.obj 2>nul
echo built topo_probe.exe
