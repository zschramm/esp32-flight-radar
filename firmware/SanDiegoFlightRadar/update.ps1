param(
    [string]$Port = 'COM6',
    [string]$Esptool = "$PSScriptRoot/../../work/arduino-data/packages/esp32/tools/esptool_py/4.9.dev3/esptool.exe",
    [switch]$Flash
)
$ErrorActionPreference = 'Stop'
if (-not $Flash) {
    Write-Output 'No device changes made. To install this update, run: .\update.ps1 -Port COM6 -Flash'
    Write-Output 'The update backs up settings, then writes only the application at 0x10000.'
    return
}

$appPath = Join-Path $PSScriptRoot 'SanDiegoFlightRadar.app.bin'
$partitionPath = Join-Path $PSScriptRoot 'SanDiegoFlightRadar.partitions.bin'
$manifestPath = Join-Path $PSScriptRoot 'SHA256.txt'
foreach ($path in @($Esptool, $appPath, $partitionPath, $manifestPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file missing: $path" }
}
if ((Get-Item -LiteralPath $appPath).Length -gt 0x300000) { throw 'Application exceeds the existing partition size.' }
$manifest = Get-Content -LiteralPath $manifestPath
foreach ($path in @($appPath, $partitionPath)) {
    $name = [IO.Path]::GetFileName($path)
    $expected = @($manifest | Where-Object { $_ -match ('^[0-9A-Fa-f]{64}  ' + [regex]::Escape($name) + '$') })
    if ($expected.Count -ne 1 -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected[0].Substring(0,64)) {
        throw "Checksum verification failed: $name"
    }
}

# All reads complete before writing. Never erase flash or write the merged image.
$backupPath = Join-Path $PSScriptRoot ('backups/' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backupPath -Force | Out-Null
$devicePartitions = Join-Path $backupPath 'partition-table.bin'
& $Esptool --chip esp32s3 --port $Port --baud 921600 read_flash 0x8000 0xC00 $devicePartitions
if ($LASTEXITCODE -ne 0) { throw 'Could not read the device partition table. No firmware was written.' }
if ((Get-FileHash -LiteralPath $devicePartitions -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $partitionPath -Algorithm SHA256).Hash) {
    throw 'Device partition layout differs from this build. Stopped to protect saved settings; no firmware was written.'
}
$nvsBackup = Join-Path $backupPath 'wifi-and-location.nvs.bin'
& $Esptool --chip esp32s3 --port $Port --baud 921600 read_flash 0x9000 0x5000 $nvsBackup
if ($LASTEXITCODE -ne 0 -or (Get-Item -LiteralPath $nvsBackup).Length -ne 0x5000) {
    throw 'Could not back up settings. No firmware was written.'
}
(Get-FileHash -LiteralPath $nvsBackup -Algorithm SHA256).Hash | Set-Content -LiteralPath (Join-Path $backupPath 'nvs-sha256.txt') -Encoding Ascii

# NVS (0x9000-0xDFFF), bootloader, partition table and filesystem are untouched.
& $Esptool --chip esp32s3 --port $Port --baud 921600 write_flash 0x10000 $appPath
if ($LASTEXITCODE -ne 0) { throw 'Application update failed. Settings remain outside the written region; retry the same update.' }
Write-Output 'Application updated. Saved Wi-Fi credentials and radar center were retained.'
Write-Output 'The settings backup is private and contains Wi-Fi credentials; keep it with this device.'
