# Build YHorizon-JM firmware on Windows (CLion cmake/ninja, CubeIDE ARM GCC).
# Usage: .\tools\build.ps1 [-Preset gd32f303|gd32f303-debug|gd32f303-factory]
# Run .\tools\setup-env.ps1 first on a new machine.

[CmdletBinding()]
param(
    [ValidateSet('gd32f303', 'gd32f303-debug', 'gd32f303-factory')]
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
