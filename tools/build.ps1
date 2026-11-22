# Build YHorizon-JM firmware on Windows (CLion cmake/ninja, CubeIDE ARM GCC).
# Usage: .\tools\build.ps1 [-Preset <configure preset from firmware/CMakePresets.json>]
# Run .\tools\setup-env.ps1 first on a new machine.

[CmdletBinding()]
param(
    [ValidateSet('gd32f303', 'gd32f303-debug', 'gd32f303-factory', 'gd32f303-dm4310', 'gd32f303-rs05', 'gd32f303-cybergear', 'gd32f303-ak60-6', 'gd32f303-rmd-x', 'gd32f303-lk-mf', 'gd32f303-hightorque', 'gd32f303-c620', 'gd32f303-c610')]
    [string]$Preset = 'gd32f303'
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\yhjm-env.ps1"
Import-YhjmToolsEnv

$Firmware = $script:FirmwareRoot

function Find-Cmake {
    $exe = Find-YhjmCmake
    if ($exe) { return $exe }
    throw "cmake not found. Run .\tools\setup-env.ps1 (or install CLion / CMake)."
}

$cmake = Find-Cmake
Write-Host "cmake: $cmake"

Push-Location $Firmware
try {
    & $cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
    & $cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
    Write-Host "OK: $Firmware\build\$Preset\YHorizon-JM.elf"
}
finally {
    Pop-Location
}
