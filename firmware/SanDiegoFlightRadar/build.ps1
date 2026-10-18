param([string]$Cli = "$PSScriptRoot/../../work/tools/arduino-cli/arduino-cli.exe", [string]$Config = "$PSScriptRoot/../../work/arduino-cli.yaml", [switch]$Clean)
$ErrorActionPreference = 'Stop'
$projectPath = (Resolve-Path -LiteralPath $PSScriptRoot).Path.Replace('\','/')
$buildPath = [IO.Path]::GetFullPath("$PSScriptRoot/../../work/build-flight-radar")
$fqbn = 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,LoopCore=1,EventsCore=1'
$flags = '-I"' + $projectPath + '"'
$cleanArgs = @()
if ($Clean) { $cleanArgs = @('--clean') }
& $Cli --config-file $Config compile @cleanArgs --fqbn $fqbn --build-path $buildPath --build-property "compiler.cpp.extra_flags=$flags" --build-property "compiler.c.extra_flags=$flags" $PSScriptRoot
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }
Copy-Item -LiteralPath "$buildPath/SanDiegoFlightRadar.ino.merged.bin" -Destination "$PSScriptRoot/SanDiegoFlightRadar.merged.bin"
Copy-Item -LiteralPath "$buildPath/SanDiegoFlightRadar.ino.bin" -Destination "$PSScriptRoot/SanDiegoFlightRadar.app.bin"
Copy-Item -LiteralPath "$buildPath/SanDiegoFlightRadar.ino.partitions.bin" -Destination "$PSScriptRoot/SanDiegoFlightRadar.partitions.bin"
$images = 'SanDiegoFlightRadar.app.bin','SanDiegoFlightRadar.partitions.bin','SanDiegoFlightRadar.merged.bin'
$hashes = foreach ($name in $images) { (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $PSScriptRoot $name)).Hash + '  ' + $name }
$hashes | Set-Content -LiteralPath "$PSScriptRoot/SHA256.txt" -Encoding Ascii
