@echo off
rem Builds the Android app (.apk) from your own disc. See README-android in games\hail-to-the-king\dist.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Android %*
echo.
pause
