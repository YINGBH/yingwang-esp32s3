@echo off
rem Stop the local web server.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*serve.ps1*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
echo Web server stopped.
timeout /t 2 >nul
