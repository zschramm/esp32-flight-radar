param([string]$Workspace = "$PSScriptRoot/../..")
$ErrorActionPreference = 'Stop'
$workRoot = [IO.Path]::GetFullPath((Join-Path $Workspace 'work')).TrimEnd('\')
$sdkPath = Join-Path $workRoot 'arduino-data/packages/esp32/tools/esp32-arduino-libs/idf-release_v5.4-2f7dcd86-v1'
$archivePath = Join-Path $workRoot 'tools/esp32-3.2.0-h.zip'
$stagingPath = Join-Path $workRoot 'sdk-staging'
$expectedHash = '3D6A0CA36C644B1AD1B3B1C405423C7E2FCD947AE8BD2A733D54289C7D540B07'
if (Test-Path -LiteralPath "$sdkPath/.flight-radar-xip") { return }
if (!(Test-Path -LiteralPath $archivePath)) {
    & curl.exe -L --fail --silent --show-error 'https://dl.espressif.com/AE/esp-arduino-libs/esp32-3.2.0-h.zip' -o $archivePath
    if ($LASTEXITCODE -ne 0) { throw 'SDK download failed' }
}
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $expectedHash) { throw 'SDK checksum mismatch' }
if (!(Test-Path -LiteralPath $stagingPath)) { Expand-Archive -LiteralPath $archivePath -DestinationPath $stagingPath }
$sdkSource = Get-ChildItem -LiteralPath $stagingPath -Directory -Filter 'arduino-esp32-libs-all-*' | Select-Object -First 1
if (!$sdkSource) { throw 'Unexpected SDK archive layout' }
# Every directory operation is restricted to this task's isolated work directory.
foreach ($candidate in @($sdkPath, "$sdkPath.standard", $sdkSource.FullName)) {
    $absoluteTarget = [IO.Path]::GetFullPath($candidate)
    if (!$absoluteTarget.StartsWith($workRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'SDK path is outside the workspace' }
}
if (Test-Path -LiteralPath $sdkPath) {
    if (Test-Path -LiteralPath "$sdkPath.standard") { throw 'An SDK backup already exists; inspect before continuing' }
    Move-Item -LiteralPath $sdkPath -Destination "$sdkPath.standard"
}
Move-Item -LiteralPath $sdkSource.FullName -Destination $sdkPath
$expectedHash | Set-Content -LiteralPath "$sdkPath/.flight-radar-xip"
Write-Output 'Installed verified Espressif 3.2.0-h PSRAM-XIP SDK into the isolated workspace.'
