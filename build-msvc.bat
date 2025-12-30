@echo off
setlocal enabledelayedexpansion

REM 需要在 Developer Command Prompt for VS 中运行（保证 cl/rc 可用）
where cl >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cl.exe not found. Please run this script in "Developer Command Prompt for VS".
  exit /b 1
)
where rc >nul 2>nul
if errorlevel 1 (
  echo [ERROR] rc.exe not found. Please run this script in "Developer Command Prompt for VS".
  exit /b 1
)

set OUT=build
if not exist "%OUT%" mkdir "%OUT%"

rc /nologo /I "src" /fo "%OUT%\\resource.res" "resource.rc"
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /O2 /MT ^
  /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  /W4 /EHsc /utf-8 ^
  "src\\main.cpp" "%OUT%\\resource.res" ^
  /link /SUBSYSTEM:WINDOWS /OUT:"%OUT%\\ScreenSaverReminderCPP.exe" ^
  user32.lib gdi32.lib shell32.lib comctl32.lib comdlg32.lib ole32.lib advapi32.lib

if errorlevel 1 exit /b 1

echo [OK] Built: %OUT%\\ScreenSaverReminderCPP.exe
