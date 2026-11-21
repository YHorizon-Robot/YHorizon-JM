# One-click Windows setup for YHorizon-JM.
# Discovers CLion / CubeIDE / GD32EB if present; otherwise downloads portable
# CMake, Ninja, ARM GCC, OpenOCD into tools/toolchain/ (gitignored). Optional SDK venv.
#
#   powershell -ExecutionPolicy Bypass -File .\tools\setup-env.ps1
#   .\tools\setup-env.ps1 -Force      # re-download portable tools
#   .\tools\setup-env.ps1 -SkipHost   # firmware tools only
#   .\tools\setup-env.ps1 -Build      # setup then compile

[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$SkipHost,
    [switch]$SkipFlash,
    [switch]$Build
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

. "$PSScriptRoot\yhjm-env.ps1"
Import-YhjmToolsEnv

$Root = $script:YhjmRoot
$Tools = $script:ToolchainRoot
$Cache = Join-Path $Tools 'cache'

# Pinned portable packages (x64 Windows zips).
$CmakeVer = '3.31.12'
$NinjaVer = '1.12.1'
$ArmVer = '12.3.1-1.2'
$OcdVer = '0.12.0-7'

$CmakeUrl = "https://github.com/Kitware/CMake/releases/download/v$CmakeVer/cmake-$CmakeVer-windows-x86_64.zip"
$NinjaUrl = "https://github.com/ninja-build/ninja/releases/download/v$NinjaVer/ninja-win.zip"
$ArmUrl = "https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v$ArmVer/xpack-arm-none-eabi-gcc-$ArmVer-win32-x64.zip"
$OcdUrl = "https://github.com/xpack-dev-tools/openocd-xpack/releases/download/v$OcdVer/xpack-openocd-$OcdVer-win32-x64.zip"

function Write-YhjmStep {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Invoke-YhjmDownload {
    param(
        [string]$Url,
        [string]$OutFile
    )
    if ((-not $Force) -and (Test-Path -LiteralPath $OutFile)) {
        Write-Host "cached: $OutFile"
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutFile) | Out-Null
    Write-Host "download: $Url"
    $tmp = "$OutFile.partial"
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        & curl.exe -L --fail --retry 3 -o $tmp $Url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
    }
    else {
        Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
    }
    Move-Item -LiteralPath $tmp -Destination $OutFile -Force
}

function Expand-YhjmZip {
    param(
        [string]$ZipFile,
        [string]$DestDir
    )
    if (Test-Path -LiteralPath $DestDir) {
        Remove-Item -LiteralPath $DestDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if ($tar) {
        & tar.exe -xf $ZipFile -C $DestDir
        if ($LASTEXITCODE -ne 0) { throw "unzip failed: $ZipFile" }
    }
    else {
        Expand-Archive -LiteralPath $ZipFile -DestinationPath $DestDir -Force
    }
}

function Get-YhjmSingleRoot {
    param([string]$Dir)
    $kids = @(Get-ChildItem -LiteralPath $Dir -Force)
    if ($kids.Count -eq 1 -and $kids[0].PSIsContainer) {
        return $kids[0].FullName
    }
    return $Dir
}

function Install-YhjmCmake {
    $existing = Find-YhjmCmake
    $portable = Join-Path $Tools 'cmake\bin\cmake.exe'
    if ((-not $Force) -and $existing) {
        Write-Host "cmake: $existing"
        return $existing
    }
    Write-YhjmStep "CMake $CmakeVer"
    $zip = Join-Path $Cache "cmake-$CmakeVer-windows-x86_64.zip"
    Invoke-YhjmDownload $CmakeUrl $zip
    $extract = Join-Path $Tools 'cmake-extract'
    Expand-YhjmZip $zip $extract
    $root = Get-YhjmSingleRoot $extract
    $dest = Join-Path $Tools 'cmake'
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Move-Item -LiteralPath $root -Destination $dest
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    if (-not (Test-Path -LiteralPath $portable)) { throw "cmake.exe missing after extract" }
    Write-Host "cmake: $portable"
    return $portable
}

function Install-YhjmNinja {
    $existing = Find-YhjmNinja
    $portable = Join-Path $Tools 'ninja\ninja.exe'
    if ((-not $Force) -and $existing) {
        Write-Host "ninja: $existing"
        return $existing
    }
    Write-YhjmStep "Ninja $NinjaVer"
    $zip = Join-Path $Cache "ninja-$NinjaVer-win.zip"
    Invoke-YhjmDownload $NinjaUrl $zip
    $dest = Join-Path $Tools 'ninja'
    Expand-YhjmZip $zip $dest
    if (-not (Test-Path -LiteralPath $portable)) { throw "ninja.exe missing after extract" }
    Write-Host "ninja: $portable"
    return $portable
}

function Install-YhjmArmGcc {
    $existing = Find-YhjmArmGcc
    if ((-not $Force) -and $existing) {
        Write-Host "arm-none-eabi-gcc: $existing"
        return $existing
    }
    Write-YhjmStep "ARM GCC $ArmVer (portable, ~300 MB)"
    $zip = Join-Path $Cache "xpack-arm-none-eabi-gcc-$ArmVer-win32-x64.zip"
    Invoke-YhjmDownload $ArmUrl $zip
    $extract = Join-Path $Tools 'arm-gnu-extract'
    Expand-YhjmZip $zip $extract
    $root = Get-YhjmSingleRoot $extract
    $dest = Join-Path $Tools 'arm-gnu'
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Move-Item -LiteralPath $root -Destination $dest
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    $gcc = Join-Path $dest 'bin\arm-none-eabi-gcc.exe'
    if (-not (Test-Path -LiteralPath $gcc)) { throw "arm-none-eabi-gcc.exe missing after extract" }
    Write-Host "arm-none-eabi-gcc: $gcc"
    return $gcc
}

function Install-YhjmOpenOcd {
    if ($SkipFlash) { return $null }
    $existing = Find-YhjmOpenOcd
    if ((-not $Force) -and $existing -and (Test-Path -LiteralPath $existing.Exe)) {
        Write-Host "openocd: $($existing.Exe)"
        if ($existing.HasGd32) {
            Write-Host "openocd scripts: $($existing.Scripts) (gd32f30x)"
        }
        else {
            Write-Host "openocd scripts: $($existing.Scripts) (stm32f1x fallback, FLASH_SIZE=256 KB)"
        }
        return $existing
    }
    Write-YhjmStep "OpenOCD $OcdVer (xPack; no gd32f30x driver, flash uses stm32f1x fallback)"
    $zip = Join-Path $Cache "xpack-openocd-$OcdVer-win32-x64.zip"
    Invoke-YhjmDownload $OcdUrl $zip
    $extract = Join-Path $Tools 'openocd-extract'
    Expand-YhjmZip $zip $extract
    $root = Get-YhjmSingleRoot $extract
    $dest = Join-Path $Tools 'openocd'
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Move-Item -LiteralPath $root -Destination $dest
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    $exe = Join-Path $dest 'bin\openocd.exe'
    $scripts = Find-YhjmOpenOcdScriptsDir $dest
    if (-not (Test-Path -LiteralPath $exe)) { throw "openocd.exe missing after extract" }
    Write-Host "openocd: $exe"
    return [pscustomobject]@{
        Exe     = $exe
        Scripts = $scripts
        HasGd32 = Test-YhjmOpenOcdHasGd32 $scripts
    }
}

function Install-YhjmHost {
    if ($SkipHost) { return }
    Write-YhjmStep "RS05 / DM4310 joint_gui (Python)"
    $py = Find-YhjmPython
    if (-not $py) {
        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if ($winget) {
            Write-Host "winget install Python.Python.3.12 (user scope)"
            & winget.exe install --id Python.Python.3.12 -e --scope user --accept-package-agreements --accept-source-agreements
            $env:Path = "$env:LOCALAPPDATA\Programs\Python\Python312;$env:LOCALAPPDATA\Programs\Python\Python312\Scripts;" + $env:Path
            $py = Find-YhjmPython
        }
    }
    if (-not $py) {
        Write-Host "Python not found. Install 3.12+ then re-run, or skip with -SkipHost." -ForegroundColor Yellow
        Write-Host "https://www.python.org/downloads/"
        return
    }

    $venv = Join-Path $script:SdkPython '.venv'
    $venvPy = Join-Path $venv 'Scripts\python.exe'
    if ($Force -or -not (Test-Path -LiteralPath $venvPy)) {
        Write-Host "venv: $venv"
        & $py.Exe @($py.Prefix) -m venv $venv
        if ($LASTEXITCODE -ne 0) { throw "python venv failed" }
    }
    $req = Join-Path $script:SdkPython 'requirements.txt'
    Write-Host "pip: $req"
    & $venvPy -m pip install --upgrade pip
    if ($LASTEXITCODE -ne 0) { throw "pip upgrade failed" }
    & $venvPy -m pip install -r $req
    if ($LASTEXITCODE -ne 0) { throw "pip install failed" }
    Write-Host "sdk python: $venvPy"
}

function Write-YhjmEnvFile {
    param(
        [string]$CmakeExe,
        [string]$NinjaExe,
        [string]$GccExe,
        [object]$OpenOcd
    )
    New-Item -ItemType Directory -Force -Path $Tools | Out-Null
    $cmakeDir = if ($CmakeExe) { Split-Path -Parent $CmakeExe } else { '' }
    $ninjaDir = if ($NinjaExe) { Split-Path -Parent $NinjaExe } else { '' }
    $gccDir = if ($GccExe) { Split-Path -Parent $GccExe } else { '' }
    $ocdExe = if ($OpenOcd) { $OpenOcd.Exe } else { '' }
    $ocdScripts = if ($OpenOcd) { $OpenOcd.Scripts } else { '' }
    $ocdGd32 = if ($OpenOcd -and $OpenOcd.HasGd32) { '1' } else { '0' }

    $pathParts = @($gccDir, $cmakeDir, $ninjaDir) | Where-Object { $_ }
    if ($OpenOcd) { $pathParts += (Split-Path -Parent $OpenOcd.Exe) }

    $lines = @(
        '# Generated by tools/setup-env.ps1. Do not edit.'
        "`$env:ARM_NONE_EABI_TOOLCHAIN_PATH = '$gccDir'"
        "`$env:YHJM_OPENOCD_EXE = '$ocdExe'"
        "`$env:YHJM_OPENOCD_SCRIPTS = '$ocdScripts'"
        "`$env:YHJM_OPENOCD_HAS_GD32 = '$ocdGd32'"
        "`$env:Path = '$($pathParts -join ';');' + `$env:Path"
    )
    $utf8 = New-Object System.Text.UTF8Encoding $false
    $envFile = Join-Path $PSScriptRoot 'env.ps1'
    [System.IO.File]::WriteAllLines($envFile, $lines, $utf8)
    Write-Host "wrote: $envFile"
}

Write-Host "YHorizon-JM setup  $Root"
New-Item -ItemType Directory -Force -Path $Tools | Out-Null

$cmakeExe = Install-YhjmCmake
$ninjaExe = Install-YhjmNinja
$gccExe = Install-YhjmArmGcc
$openocd = Install-YhjmOpenOcd
Install-YhjmHost
Write-YhjmEnvFile -CmakeExe $cmakeExe -NinjaExe $ninjaExe -GccExe $gccExe -OpenOcd $openocd
. (Join-Path $PSScriptRoot 'env.ps1')

Write-YhjmStep "Summary"
Write-Host "cmake              $cmakeExe"
Write-Host "ninja              $ninjaExe"
Write-Host "arm-none-eabi-gcc  $gccExe"
if ($openocd) {
    Write-Host "openocd            $($openocd.Exe)"
    if ($openocd.HasGd32) {
        Write-Host "flash driver       gd32f30x"
    }
    else {
        Write-Host "flash driver       stm32f1x fallback (256 KB, no mass_erase)"
        Write-Host "prefer GD32EB OpenOCD if you already have it; re-run this script after installing."
    }
}
else {
    Write-Host "openocd            (skipped)"
}
Write-Host ""
Write-Host "Next:"
Write-Host "  .\tools\build.ps1"
Write-Host "  .\tools\flash.ps1          # CMSIS-DAP + SWD"
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    Write-Host "  Install Rust: https://rustup.rs    # host-rs and the communication SDK"
}
else {
    Write-Host "  cargo build --release --locked --manifest-path host-rs/Cargo.toml -p yfoc-host"
    Write-Host "  python sdk/build_native.py"
}
Write-Host "  .\host-rs\target\release\yfoc-host.exe"
Write-Host "Windows PCAN needs PCAN-Basic: https://www.peak-system.com/PCAN-Basic.239.0.html"

if ($Build) {
    Write-YhjmStep "Build"
    & (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
