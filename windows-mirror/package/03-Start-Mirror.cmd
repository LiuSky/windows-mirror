@echo off
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp003-Start-Mirror.ps1"
set "VALERIA_EXIT=%ERRORLEVEL%"
echo.
pause
exit /b %VALERIA_EXIT%
