@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0BUILD.ps1" %*
set "build_result=%errorlevel%"
pause
exit /b %build_result%
