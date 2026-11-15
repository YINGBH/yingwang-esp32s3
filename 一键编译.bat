@echo off
setlocal
rem 一键：下载字体 -^> 生成点阵 -^> 编译
rem 前提：装好 ESP-IDF v5.5.x，且能跑 idf.py
rem 提示信息故意用 ASCII：cmd 的代码页因机器而异，中文 echo 容易变乱码

if not exist "main\app_config.h" (
    echo [INFO] main\app_config.h not found, creating from template
    copy /y "main\app_config.example.h" "main\app_config.h" >nul
    echo        Edit it: your WiFi SSID/password and broadband account.
)

echo ============================================
echo  1/4  download open-source fonts
echo ============================================
python tools\fetch_fonts.py || goto :err

echo.
echo ============================================
echo  2/4  generate TFT bitmap font
echo ============================================
python tools\gen_font.py || goto :err

echo.
echo ============================================
echo  3/4  generate e-ink bitmap font
echo ============================================
python tools\gen_esl_font.py || goto :err

echo.
echo ============================================
echo  4/4  build firmware
echo ============================================
idf.py build || goto :err

echo.
echo Build OK. To flash:
echo     idf.py -p COM5 flash
echo Captcha model (storage partition, only when changed):
echo     python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0xC10000 model_q.bin
exit /b 0

:err
echo.
echo   Failed. See messages above.
exit /b 1
