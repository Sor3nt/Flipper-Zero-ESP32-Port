param(
    [ValidatePattern('^COM[1-9][0-9]*$')][string]$Port,
    [ValidateSet(115200,230400,460800,921600)][int]$BaudRate = 460800,
    [string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$Uploader,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $BuildDir) { $BuildDir = Join-Path $PSScriptRoot '..\build_t_embed_plus_i2c\bw16' }
$imageRoot = (Resolve-Path -LiteralPath $BuildDir).Path
$manifest = Get-Content -LiteralPath (Join-Path $imageRoot 'BUILD_INFO.json') -Raw | ConvertFrom-Json
if ($manifest.name -ne 'BW16 I2C controller 4.5 for CC1101 Plus' -or -not $manifest.source_built) {
    throw 'Expected a source-built I2C 4.5 image. No hardware opened.'
}
foreach ($name in @('imgtool_flashloader_amebad.bin','km0_boot_all.bin','km4_boot_all.bin','km0_km4_image2.bin')) {
    $file = Join-Path $imageRoot $name
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $manifest.files.$name.sha256) {
        throw "Image verification failed: $name. No hardware opened."
    }
}
if ((Get-FileHash -LiteralPath $Uploader -Algorithm SHA256).Hash -ne '0dc86f3c3dd32075c088810fd81ac475fdc4e0fc425f4bf1c2619ede25d8859d') {
    throw 'Expected the verified uploader from the working BW16 bundle. The SDK uploader uses different arguments. No hardware opened.'
}
if ($CheckOnly) { Write-Host 'PASS: I2C images and uploader verified. No hardware opened.'; exit 0 }
if (-not $Port) { throw 'Specify the BW16 USB port with -Port COMx.' }
Write-Host 'Disconnect BW16 at the Lilygo connector. Connect only BW16 USB.'
Write-Host 'Enter upload mode: hold BURN, press/release RST, release BURN.'
Write-Host "Uploading on $Port at $BaudRate; initial handshake is 115200."
$log = Join-Path $imageRoot ('flash-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
& $Uploader $imageRoot $Port '--auto=0' "--baudrate=$BaudRate" '--verbose=4' 2>&1 | Tee-Object -FilePath $log
$toolExit = $LASTEXITCODE
$result = Get-Content -LiteralPath $log -Raw
if ($toolExit -ne 0 -or $result -notmatch '(?s)verifying km0 km4 and app blocks\.\.\.\.ok\.\s+done\.') {
    throw "Upload was not verified. Preserve recovery files. Log: $log"
}
Write-Host 'SUCCESS: boot and application checksums verified. Press RST, then remove BW16 USB.'
Write-Host 'Fully Power Off Lilygo with its USB removed before connecting BW16.'
