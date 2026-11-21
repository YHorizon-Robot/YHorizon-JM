# Flash YHorizon-JM onto GD32F303 with OpenOCD (CMSIS-DAP / SWD).
# Prefer GigaDevice OpenOCD (`gd32f30x`). xPack/stock OpenOCD uses
# tools/openocd-gd32f303-stm32f1x.cfg (256 KB, no mass_erase).
# Usage:
#   .\tools\flash.ps1
#   .\tools\flash.ps1 -BuildBeforeFlash
#   .\tools\flash.ps1 -Preset gd32f303-debug

param(
    [ValidateSet('gd32f303', 'gd32f303-debug', 'gd32f303-factory')]
    [string]$Preset = 'gd32f303',

    [string]$ElfFile = '',
    [string]$OpenOcdExe = '',
    [string]$OpenOcdScripts = '',
    [string]$ConfigFile = '',

    [Alias('Build')]
    [switch]$BuildBeforeFlash,
    [switch]$NoResetRun
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\yhjm-env.ps1"
Import-YhjmToolsEnv

$Firmware = $script:FirmwareRoot

function Invoke-Native {
    param(
        [string]$Exe,
        [string[]]$Arguments
    )
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

$ocd = Find-YhjmOpenOcd
if ($OpenOcdExe -eq '') {
    if ($ocd) { $OpenOcdExe = $ocd.Exe }
}
if ($OpenOcdScripts -eq '') {
    if ($ocd) { $OpenOcdScripts = $ocd.Scripts }
}

$hasGd32 = $false
if ($ocd) { $hasGd32 = [bool]$ocd.HasGd32 }
if ($OpenOcdScripts) {
    $hasGd32 = Test-YhjmOpenOcdHasGd32 $OpenOcdScripts
}

if ($ConfigFile -eq '') {
    if ($hasGd32) {
        $ConfigFile = Join-Path $PSScriptRoot 'openocd-gd32f303.cfg'
    }
    else {
        $ConfigFile = Join-Path $PSScriptRoot 'openocd-gd32f303-stm32f1x.cfg'
    }
}

if ($ElfFile -eq '') {
    $ElfFile = Join-Path $Firmware "build\$Preset\YHorizon-JM.elf"
}

if ($BuildBeforeFlash) {
    $buildScript = Join-Path $PSScriptRoot 'build.ps1'
    if (-not (Test-Path -LiteralPath $buildScript)) {
        throw "Build script not found: $buildScript"
    }
    & $buildScript -Preset $Preset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if (-not $OpenOcdExe -or -not (Test-Path -LiteralPath $OpenOcdExe)) {
    throw "OpenOCD executable not found. Run .\tools\setup-env.ps1 or pass -OpenOcdExe."
}
if (-not $OpenOcdScripts -or -not (Test-Path -LiteralPath $OpenOcdScripts)) {
    throw "OpenOCD scripts directory not found. Run .\tools\setup-env.ps1 or pass -OpenOcdScripts."
}
if (-not (Test-Path -LiteralPath $ConfigFile)) {
    throw "OpenOCD config not found: $ConfigFile"
}
if (-not (Test-Path -LiteralPath $ElfFile)) {
    throw "ELF file not found: $ElfFile. Run .\tools\build.ps1 -Preset $Preset first."
}

$resolvedElf = (Resolve-Path -LiteralPath $ElfFile).Path
$elfForOpenOcd = $resolvedElf.Replace('\', '/')

# Program with page-erase as needed, then verify and reset.
# Do not stm32f1x mass_erase.
$commands = @(
    'tcl port disabled',
    'gdb port disabled'
)
if ($NoResetRun) {
    $commands += "program `"$elfForOpenOcd`" verify exit"
} else {
    $commands += "program `"$elfForOpenOcd`" verify reset exit"
}

$ocdArgs = @(
    '-s', $OpenOcdScripts,
    '-f', $ConfigFile
)
foreach ($command in $commands) {
    $ocdArgs += @('-c', $command)
}

Write-Host "Preset: $Preset"
Write-Host "ELF: $resolvedElf"
Write-Host "OpenOCD: $OpenOcdExe"
if ($hasGd32) {
    Write-Host "Flash: gd32f30x program+verify (no mass_erase)"
} else {
    Write-Host "Flash: stm32f1x fallback 256 KB program+verify (no mass_erase)"
}

Invoke-Native -Exe $OpenOcdExe -Arguments $ocdArgs
Write-Host "OK: flashed $resolvedElf"
