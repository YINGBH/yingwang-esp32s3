@echo off
setlocal
cd /d "%~dp0"

rem 抓校园网登录（电脑端）
rem 用法：双击，或在命令行 抓登录.cmd probe / proxy / check
rem 说明：.cmd 里的提示用 ASCII，避免 cmd 代码页不同导致中文变乱码；
rem       脚本自己的中文输出由 tools/capture_login.py 跟随控制台代码页处理

rem 1) 同目录有没有免安装 exe？
if exist "%~dp0capture_login.exe" goto :use_exe

rem 2) 没有就找 Python（跳过 Windows 商店的假别名）
set "PY="
python -c "import sys" >nul 2>nul && set "PY=python"
if defined PY goto :use_py
py -3 -c "import sys" >nul 2>nul && set "PY=py -3"
if defined PY goto :use_py
if exist "%LOCALAPPDATA%\Programs\Python\Python313\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python313\python.exe"
if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
if exist "%LOCALAPPDATA%\Programs\Python\Python311\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python311\python.exe"
if exist "%LOCALAPPDATA%\Programs\Python\Python310\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python310\python.exe"
if exist "C:\Espressif\tools\idf-python\3.11.2\python.exe" set "PY=C:\Espressif\tools\idf-python\3.11.2\python.exe"
if defined PY goto :use_py

echo [ERROR] Neither capture_login.exe nor Python found.
echo         Option A: download capture-login.exe from the Release page
echo                   and put it next to this file.
echo         Option B: install Python 3.8+ ^(check "Add to PATH"^).
echo         Option C: use ESP-IDF's python:
echo                   C:\Espressif\tools\idf-python\3.11.2\python.exe
pause
exit /b 1

:use_exe
set "RUN=%~dp0capture_login.exe"
set "WHAT=capture_login.exe (no Python needed)"
goto :banner

:use_py
set "RUN=%PY% "%~dp0tools\capture_login.py""
set "WHAT=%PY%"

:banner
echo.
echo  ============================================================
echo   Capture campus portal login   (PC side)
echo.
echo   probe   detect portal_base / portal_ac / portal_ssid  [default]
echo   proxy   run a local proxy and record a real login
echo   check   tell whether you are online / need login
echo.
echo   using: %WHAT%
echo  ============================================================
echo.

%RUN% %*
set RC=%ERRORLEVEL%
echo.
if "%1"=="" pause
exit /b %RC%
