@echo off
rem ============================================================
rem TypeAnything one-click installer
rem Double-click this file. UAC will prompt for elevation, then a
rem dialog box asks for your DeepSeek API key. After that, the
rem PowerShell installer drops the binaries into your Weasel
rem installation directory and registers everything.
rem ============================================================

setlocal
cd /d "%~dp0"

rem Re-launch elevated if not already admin.
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo.
echo ============================================================
echo  TypeAnything Installer
echo ============================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-TypeAnything.ps1"

echo.
pause
