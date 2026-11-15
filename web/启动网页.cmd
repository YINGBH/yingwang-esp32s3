@echo off
rem Start the local web server and open the browser.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0launcher.ps1"
exit /b 0
