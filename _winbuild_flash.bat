@echo off
REM Flash firmware. Set ESPPORT to your T-Embed's COM port (default COM14).
REM Override ESP_IDF_DIR if your ESP-IDF v5.4 install lives elsewhere.
set "MSYSTEM="
if not defined ESP_IDF_DIR set "ESP_IDF_DIR=C:\Espressif\frameworks\esp-idf-v5.4.1"
if not defined ESPPORT set "ESPPORT=COM14"
call "%ESP_IDF_DIR%\export.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
idf.py -B build_t_embed -DFLIPPER_BOARD=lilygo_t_embed_cc1101 -p %ESPPORT% flash
