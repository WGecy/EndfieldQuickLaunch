@echo off
setlocal
cd /d "%~dp0"
g++ -std=c++20 -O2 -municode -Wall -Wextra -Wpedantic -Wshadow ^
    -static -static-libgcc -static-libstdc++ ^
    -o selfcheck.exe selfcheck.cpp
if errorlevel 1 (
  echo [ERROR] Build failed.
  exit /b 1
)
echo [OK] selfcheck.exe built. Run it to verify parent-process spoofing.
endlocal
