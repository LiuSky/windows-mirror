@echo off
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp004-Restore-Apple-MI02.ps1"
set "VALERIA_EXIT=%ERRORLEVEL%"
echo.
pause
exit /b %VALERIA_EXIT%
