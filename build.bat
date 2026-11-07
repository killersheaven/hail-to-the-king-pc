@echo off
rem Builds the game from your own disc. See README.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
echo.
pause
