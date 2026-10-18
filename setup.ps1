param([string]$Cli='arduino-cli')
$ErrorActionPreference='Stop'
$workspace = $PSScriptRoot
$working = Join-Path $workspace 'work'
$cliDirectory = Join-Path $working 'tools/arduino-cli'
$sdkTools = Join-Path $working 'tools'
New-Item -ItemType Directory -Path $cliDirectory -Force | Out-Null
$inputCli = (Get-Command $Cli -ErrorAction Stop).Source
$localCli = Join-Path $cliDirectory 'arduino-cli.exe'
if ([IO.Path]::GetFullPath($inputCli) -ne [IO.Path]::GetFullPath($localCli)) { Copy-Item -LiteralPath $inputCli -Destination $localCli }
$data = (Join-Path $working 'arduino-data').Replace('\','/')
$downloads = (Join-Path $working 'arduino-downloads').Replace('\','/')
$user = (Join-Path $working 'arduino-user').Replace('\','/')
$configuration = Join-Path $working 'arduino-cli.yaml'
@"
board_manager:
  additional_urls:
    - https://espressif.github.io/arduino-esp32/package_esp32_index.json
directories:
  data: '$data'
  downloads: '$downloads'
  user: '$user'
"@ | Set-Content -LiteralPath $configuration -Encoding UTF8
& $localCli --config-file $configuration core update-index
if ($LASTEXITCODE -ne 0) { throw 'Core index update failed' }
& $localCli --config-file $configuration core install esp32:esp32@3.2.0
if ($LASTEXITCODE -ne 0) { throw 'ESP32 core installation failed' }
& $localCli --config-file $configuration lib update-index
if ($LASTEXITCODE -ne 0) { throw 'Library index update failed' }
foreach ($library in @('ESP32_Display_Panel@1.0.4','ESP32_IO_Expander@1.1.1','esp-lib-utils@0.2.3','lvgl@8.4.0','ArduinoJson@7.4.2')) {
    & $localCli --config-file $configuration lib install $library
    if ($LASTEXITCODE -ne 0) { throw "Library installation failed: $library" }
}
& (Join-Path $workspace 'firmware/SanDiegoFlightRadar/setup-sdk.ps1') -Workspace $workspace
Write-Output 'Pinned toolchain is ready. Run firmware/SanDiegoFlightRadar/build.ps1 next.'
