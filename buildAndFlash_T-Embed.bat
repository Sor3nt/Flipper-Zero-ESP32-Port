@echo off
setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "BOARD=lilygo_t_embed_cc1101"
set "BUILD_DIR=build_t_embed"
set "IDF_TARGET=esp32s3"
set "PORT=%ESPPORT%"
set "BUILD_ONLY=0"
set "RUN_MONITOR=0"

if not defined EXPORT_SCRIPT set "EXPORT_SCRIPT=%USERPROFILE%\esp\esp-idf\export.bat"
if defined ESP_IDF_EXPORT_SCRIPT set "EXPORT_SCRIPT=%ESP_IDF_EXPORT_SCRIPT%"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--port"       ( set "PORT=%~2" & shift & shift & goto parse_args )
if /I "%~1"=="-p"           ( set "PORT=%~2" & shift & shift & goto parse_args )
if /I "%~1"=="--monitor"    ( set "RUN_MONITOR=1" & shift & goto parse_args )
if /I "%~1"=="-m"           ( set "RUN_MONITOR=1" & shift & goto parse_args )
if /I "%~1"=="--build-only" ( set "BUILD_ONLY=1" & shift & goto parse_args )
if /I "%~1"=="--help"       goto usage
if /I "%~1"=="-h"           goto usage
echo Unknown argument: %~1 1>&2
goto usage
:args_done

if "%BUILD_ONLY%"=="0" if "%PORT%"=="" call :detect_port
if "%BUILD_ONLY%"=="0" if "%PORT%"=="" (
    echo No serial device found. Use --port COMx or set ESPPORT. 1>&2
    exit /b 1
)

if not exist "%EXPORT_SCRIPT%" (
    echo ESP-IDF export script not found: %EXPORT_SCRIPT% 1>&2
    echo Set ESP_IDF_EXPORT_SCRIPT or install ESP-IDF to %%USERPROFILE%%\esp\esp-idf 1>&2
    exit /b 1
)

echo Board:         %BOARD%
echo Target:        %IDF_TARGET%
echo Build dir:     %BUILD_DIR%
echo Using ESP-IDF: %EXPORT_SCRIPT%
echo Serial port:   %PORT%

call "%EXPORT_SCRIPT%" || exit /b 1

cd /d "%SCRIPT_DIR%"

if not exist "%BUILD_DIR%\build.ninja" (
    echo Setting IDF target to %IDF_TARGET%...
    idf.py -B "%BUILD_DIR%" set-target %IDF_TARGET% || exit /b 1
)

if "%RUN_MONITOR%"=="1" (
    if "%BUILD_ONLY%"=="1" (
        idf.py -B "%BUILD_DIR%" -DFLIPPER_BOARD=%BOARD% reconfigure build || exit /b 1
    ) else (
        idf.py -B "%BUILD_DIR%" -DFLIPPER_BOARD=%BOARD% -p "%PORT%" reconfigure build flash monitor || exit /b 1
    )
) else (
    if "%BUILD_ONLY%"=="1" (
        idf.py -B "%BUILD_DIR%" -DFLIPPER_BOARD=%BOARD% reconfigure build || exit /b 1
    ) else (
        idf.py -B "%BUILD_DIR%" -DFLIPPER_BOARD=%BOARD% -p "%PORT%" reconfigure build flash || exit /b 1
    )
)
exit /b 0

:detect_port
rem Try PowerShell first (gives clean COMxx list of present USB serial devices)
set "PORT_COUNT=0"
for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "Get-CimInstance -ClassName Win32_PnPEntity | Where-Object { $_.Caption -match '\(COM[0-9]+\)' -and ($_.Caption -match 'USB' -or $_.Caption -match 'Serial' -or $_.Caption -match 'CP210' -or $_.Caption -match 'CH34' -or $_.Caption -match 'FTDI' -or $_.Caption -match 'Silicon Labs' -or $_.Caption -match 'JTAG') } | ForEach-Object { ($_.Caption -replace '.*\((COM[0-9]+)\).*','$1') }" 2^>nul`) do (
    set /a PORT_COUNT+=1
    set "PORT=%%P"
    set "PORT_!PORT_COUNT!=%%P"
)
if "%PORT_COUNT%"=="1" exit /b 0
if "%PORT_COUNT%"=="0" (
    set "PORT="
    exit /b 0
)
echo Multiple serial devices found: 1>&2
for /l %%I in (1,1,%PORT_COUNT%) do call echo   %%PORT_%%I%% 1>&2
echo Use --port COMx or set ESPPORT. 1>&2
set "PORT="
exit /b 0

:usage
echo Usage: %~nx0 [--port ^<COMx^>] [--monitor] [--build-only]
echo.
echo Builds and flashes the ESP32-S3 firmware for LilyGo T-Embed CC1101.
echo.
echo Options:
echo   --port ^<COMx^>   Serial device to flash. Default: auto-detect via PowerShell/WMI
echo   --monitor       Open idf.py monitor after flashing
echo   --build-only    Build only, skip flashing
echo.
echo Environment:
echo   ESPPORT                  Overrides the auto-detected serial device
echo   ESP_IDF_EXPORT_SCRIPT    Overrides the ESP-IDF export.bat path
echo                            (default: %%USERPROFILE%%\esp\esp-idf\export.bat)
exit /b 0
