@echo off
REM Build firmware for LilyGo T-Embed CC1101 (covers the Plus variant too).
REM Override ESP_IDF_DIR if your ESP-IDF v5.4 install lives elsewhere.
set "MSYSTEM="
if not defined ESP_IDF_DIR set "ESP_IDF_DIR=C:\Espressif\frameworks\esp-idf-v5.4.1"
call "%ESP_IDF_DIR%\export.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
set "FLIPPER_BOARD=lilygo_t_embed_cc1101"
idf.py -B build_t_embed -DFLIPPER_BOARD=lilygo_t_embed_cc1101 set-target esp32s3
if errorlevel 1 exit /b 1
idf.py -B build_t_embed -DFLIPPER_BOARD=lilygo_t_embed_cc1101 reconfigure build
