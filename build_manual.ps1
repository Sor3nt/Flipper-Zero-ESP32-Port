# Manual build script bypassing ESP-IDF activation issues with usernames containing spaces

# Set up manual environment
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.4.1"
$env:IDF_TOOLS_PATH = "C:\Users\X1 Carbon\.espressif"
$env:FLIPPER_BOARD = "reaper_fury"

# Add all required tools to PATH - in order
$paths = @(
    "C:\Users\X1 Carbon\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\",
    "C:\Espressif\tools\ninja\1.12.1\",
    "C:\Users\X1 Carbon\.espressif\python_env\idf5.4_py3.11_env\Scripts\",
    "C:\Espressif\frameworks\esp-idf-v5.4.1\tools\",
    $env:PATH
)

$env:PATH = $paths -join ";"

Write-Host "✓ Environment variables set"
Write-Host "  FLIPPER_BOARD=$env:FLIPPER_BOARD"
Write-Host "  PATH updated with tools"

# Change to project directory
cd "C:\Users\X1 Carbon\Folder Baru\Flipper-Zero-ESP32-Port"

Write-Host "`n🔨 Starting build for $env:FLIPPER_BOARD..."
Write-Host "====================================================`n"

# Clean build directory
Remove-Item build_custom -Recurse -Force -ErrorAction SilentlyContinue 2>&1 | Out-Null

# Run build using Python directly with ESP-IDF environment variables
python C:\Espressif\frameworks\esp-idf-v5.4.1\tools\idf.py `
    -B build_custom `
    -DFLIPPER_BOARD=reaper_fury `
    -j 1 `
    build 2>&1 | Tee-Object -FilePath build_manual.log

# Check results
if ($LASTEXITCODE -eq 0) {
    Write-Host "`n✅ BUILD SUCCESSFUL!"
    if (Test-Path "build_custom/firmware.bin") {
        $size = (Get-Item "build_custom/firmware.bin").Length / 1MB
        Write-Host "   Firmware: build_custom/firmware.bin ($size MB)"
    }
} else {
    Write-Host "`n❌ BUILD FAILED (exit code: $LASTEXITCODE)"
    Write-Host "   Check build_manual.log for details"
    
    # Show last error lines
    Get-Content build_manual.log -Tail 50 | Select-String "error|FAILED" | Select-Object -Last 5
}
