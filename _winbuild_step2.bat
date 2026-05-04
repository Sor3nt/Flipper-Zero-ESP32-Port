@echo off
REM Windows ESP-IDF env activation. Override ESP_IDF_DIR to point at a
REM different ESP-IDF install if yours isn't at the default location.
set "MSYSTEM="
if not defined ESP_IDF_DIR set "ESP_IDF_DIR=C:\Espressif\frameworks\esp-idf-v5.4.1"
call "%ESP_IDF_DIR%\export.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
idf.py --version
