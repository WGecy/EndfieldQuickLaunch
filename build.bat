@echo off
setlocal
cd /d "%~dp0"

where g++ >nul 2>nul
if errorlevel 1 (
  echo [ERROR] g++ not found in PATH.
  echo         Install MinGW-w64 ^(or MSYS2: pacman -S mingw-w64-x86_64-gcc^) and retry.
  exit /b 1
)
where windres >nul 2>nul
if errorlevel 1 (
  echo [ERROR] windres not found in PATH ^(ships with MinGW-w64 binutils^).
  exit /b 1
)

rem -mwindows        : GUI subsystem, no console window on double-click.
rem -static*          : no MinGW runtime DLLs required next to the exe.
rem -ffunction-sections -fdata-sections + --gc-sections : drop unreferenced code.
rem -s                 : strip symbols.
set "CXXFLAGS=-std=c++20 -O2 -municode -mwindows -DUNICODE -D_UNICODE -Wall -Wextra -Wpedantic -Wshadow -finput-charset=UTF-8 -fexec-charset=UTF-8 -static -static-libgcc -static-libstdc++ -ffunction-sections -fdata-sections -Wl,--gc-sections -s"
set "LIBS=-lwintrust -lcrypt32 -lshell32 -ladvapi32 -lversion"

echo [1/3] Compiling manifest resource ...
windres -I res -o manifest_res.o res\resources.rc
if errorlevel 1 (
  echo [ERROR] windres failed.
  exit /b 1
)

echo [2/3] Linking EndfieldQuickLaunch.exe - Release build, writes nothing to disk ...
g++ %CXXFLAGS% -o EndfieldQuickLaunch.exe src\main.cpp manifest_res.o %LIBS%
if errorlevel 1 (
  echo [ERROR] Release build failed.
  del /q manifest_res.o >nul 2>nul
  exit /b 1
)

rem Debug build only difference: -DEQL_DEBUG turns on last-run.log and config.ini writing.
echo [3/3] Linking EndfieldQuickLaunch-debug.exe - Debug build, writes log and config ...
g++ %CXXFLAGS% -DEQL_DEBUG -o EndfieldQuickLaunch-debug.exe src\main.cpp manifest_res.o %LIBS%
if errorlevel 1 (
  echo [ERROR] Debug build failed.
  del /q manifest_res.o >nul 2>nul
  exit /b 1
)

del /q manifest_res.o >nul 2>nul
echo [OK] EndfieldQuickLaunch.exe          - no log, no config, no leftovers.
echo [OK] EndfieldQuickLaunch-debug.exe    - writes last-run.log and config.ini.
endlocal
