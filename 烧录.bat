@echo off
setlocal
rem 颖网 ESP32-S3 烧录：固件 + 验证码模型
rem 用法：烧录.bat COM5
rem 前提：装好 ESP-IDF v5.5.x，并在当前 shell 里 export 过环境
rem 提示信息故意用 ASCII：cmd 的代码页因机器而异，中文 echo 容易变乱码
set PORT=%1
if "%PORT%"=="" set PORT=COM5

echo.
echo   Port: %PORT%
echo   [1/2] flash firmware (bootloader + partition table + app)
idf.py -p %PORT% flash
if errorlevel 1 goto :err

echo.
echo   [2/2] write captcha model to 0xC10000 (storage partition, 383 KB)
python -m esptool --chip esp32s3 -p %PORT% -b 921600 write_flash 0xC10000 model_q.bin
if errorlevel 1 goto :err

echo.
echo   Done. Monitor with:  idf.py -p %PORT% monitor
echo   Notes:
echo     - step 2 only needed when model_q.bin changes
echo     - later updates can use OTA in the USB web tool
echo     - flash fails / timeout = board not in download mode:
echo       hold BOOT and tap RST
exit /b 0

:err
echo.
echo   Failed. See messages above.
echo   - "idf.py not found" -^> run ESP-IDF export.bat / export.sh first
echo   - "no esptool"       -^> use python from ESP-IDF env, or pip install esptool
echo   - details: README / docs
exit /b 1
