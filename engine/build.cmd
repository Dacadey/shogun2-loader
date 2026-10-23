@echo off
rem Builds the 32-bit version.dll proxy (engine fixes) into engine\build\. Run from anywhere. Needs Visual Studio with
rem the C++ tools (vsenv.cmd finds it) or an x86 Developer Command Prompt.
call "%~dp0vsenv.cmd"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /EHsc /std:c++17 /W3 /MT /LD /D_CRT_SECURE_NO_WARNINGS /Fo:build\ src\engine.cpp ^
   /link /DEF:src\exports.def /OUT:build\version.dll /IMPLIB:build\version_proxy.lib /NOLOGO
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo built build\version.dll
