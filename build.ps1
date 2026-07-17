#requires -Version 5.1
<#
    build.ps1 - Standalone build for the ESP-NOW Wireless UART Bridge
    -----------------------------------------------------------------
    Sets up the full toolchain environment (no PlatformIO required) and
    drives the ESP8266_RTOS_SDK v3.4 cmake/ninja build.

    Usage:
        .\build.ps1                 # configure + build
        .\build.ps1 -Clean          # wipe build/ first, then build
        .\build.ps1 -Flash          # build, then flash all required images
        .\build.ps1 -Flash -Monitor # build, flash, then open serial monitor
        .\build.ps1 -Port COM5      # override serial port (else $env:ESPPORT)

    Tool locations can be overridden with environment variables:
        IDF_PATH, ESP_TOOLCHAIN, ESP_CMAKE_DIR, ESP_TOOLS_BIN
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port = $env:ESPPORT,
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'
$ProjectDir = $PSScriptRoot
$BuildDir   = Join-Path $ProjectDir 'build'

# ---------------------------------------------------------------------------
# Tool locations (override via environment variables if yours differ)
# ---------------------------------------------------------------------------
if (-not $env:IDF_PATH) { $env:IDF_PATH = 'D:\Project\ESP8266_RTOS_SDK' }
$Toolchain = if ($env:ESP_TOOLCHAIN) { $env:ESP_TOOLCHAIN } else { 'C:\Espressif\xtensa-lx106-elf\bin' }
$CMakeDir  = if ($env:ESP_CMAKE_DIR) { $env:ESP_CMAKE_DIR } else { 'C:\Program Files\CMake\bin' }
$ToolsBin  = if ($env:ESP_TOOLS_BIN) { $env:ESP_TOOLS_BIN } else { 'C:\Tools\bin' }

# ---------------------------------------------------------------------------
# Locate ninja: prefer PATH, else the winget package dir, else C:\Tools\bin
# ---------------------------------------------------------------------------
function Find-Ninja {
    $onPath = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($onPath) { return Split-Path $onPath.Source }

    $candidates = @(
        (Join-Path $ToolsBin 'ninja.exe'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe')
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return Split-Path $c } }
    throw "ninja.exe not found. Install it: winget install Ninja-build.Ninja"
}
$NinjaDir = Find-Ninja

# ---------------------------------------------------------------------------
# Compose PATH: our tools first, then the existing system PATH.
# Deliberately does NOT depend on PlatformIO.
# ---------------------------------------------------------------------------
$env:PATH = @($CMakeDir, $NinjaDir, $Toolchain, $ToolsBin, $env:PATH) -join ';'

# ---------------------------------------------------------------------------
# Validate the toolchain before doing anything expensive.
# ---------------------------------------------------------------------------
function Assert-Tool([string]$Exe, [string]$Hint) {
    $cmd = Get-Command $Exe -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "Required tool '$Exe' not found on PATH. $Hint" }
    return $cmd.Source
}

Write-Host '== Toolchain ==' -ForegroundColor Cyan
if (-not (Test-Path $env:IDF_PATH)) {
    throw "IDF_PATH does not exist: $($env:IDF_PATH). Set `$env:IDF_PATH to your ESP8266_RTOS_SDK checkout."
}
Write-Host ("  IDF_PATH  : {0}" -f $env:IDF_PATH)
Write-Host ("  cmake     : {0}" -f (Assert-Tool 'cmake.exe'  'Install: winget install Kitware.CMake'))
Write-Host ("  ninja     : {0}" -f (Assert-Tool 'ninja.exe'  'Install: winget install Ninja-build.Ninja'))
Write-Host ("  compiler  : {0}" -f (Assert-Tool 'xtensa-lx106-elf-gcc.exe' 'Set $env:ESP_TOOLCHAIN to the toolchain bin dir.'))
Write-Host ("  mconf-idf : {0}" -f (Assert-Tool 'mconf-idf.exe' "Copy mconf-idf.exe into $ToolsBin (kconfig menuconfig tool)."))
Write-Host ("  python    : {0}" -f (Assert-Tool 'python.exe'  'Install Python 3 and add it to PATH.'))
Write-Host ("  git       : {0}" -f (Assert-Tool 'git.exe'     'Install Git and add it to PATH.'))
Write-Host ''

# ---------------------------------------------------------------------------
# Clean (optional)
# ---------------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host '== Clean: removing build/ ==' -ForegroundColor Cyan
    Remove-Item -Recurse -Force $BuildDir
}

# ---------------------------------------------------------------------------
# Configure (cmake) - only when the build dir has no cache yet.
#   -DPYTHON_DEPS_CHECKED=1        : skip the SDK's stale Python version
#                                    ceilings (our newer packages work fine).
#   -DCMAKE_POLICY_VERSION_MINIMUM : cmake 4.x dropped support for the SDK's
#                                    ancient cmake_minimum_required declarations.
# ---------------------------------------------------------------------------
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cacheFile)) {
    Write-Host '== Configure (cmake) ==' -ForegroundColor Cyan
    # NOTE: quote each -D as a literal. Unquoted, PowerShell mangles the
    # bareword ending in "=3.5" and cmake receives "3" (an invalid policy
    # version), breaking configure. Quoting passes the value verbatim.
    & cmake.exe -G Ninja -S $ProjectDir -B $BuildDir `
        '-DPYTHON_DEPS_CHECKED=1' `
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
    if ($LASTEXITCODE -ne 0) {
        # A half-written cache poisons every later run (the skip guard below
        # would keep reusing it). Remove it so the next invocation retries clean.
        if (Test-Path $cacheFile) { Remove-Item -Force $cacheFile }
        throw "cmake configure failed ($LASTEXITCODE)"
    }
} else {
    Write-Host '== Configure: cache exists, skipping (use -Clean to reconfigure) ==' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Build (ninja via cmake)
# ---------------------------------------------------------------------------
Write-Host '== Build ==' -ForegroundColor Cyan
& cmake.exe --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$fw = Join-Path $BuildDir 'wireless_uart.bin'
Write-Host ''
Write-Host ("== Build OK -> {0} ==" -f $fw) -ForegroundColor Green

# ---------------------------------------------------------------------------
# Flash (optional): writes bootloader + partition table + app.
# Offsets are the ESP8266 defaults for a 1MB esp01_1m layout.
# ---------------------------------------------------------------------------
if ($Flash) {
    if (-not $Port) {
        throw "No serial port. Pass -Port COMx or set `$env:ESPPORT."
    }
    $env:ESPPORT = $Port
    $env:ESPBAUD = [string]$Baud
    Write-Host ("== Flash (port {0}, {1} baud) ==" -f $Port, $Baud) -ForegroundColor Cyan
    & cmake.exe --build $BuildDir --target flash -- -j1
    if ($LASTEXITCODE -ne 0) { throw "flash failed ($LASTEXITCODE)" }
    Write-Host '== Flash OK ==' -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Monitor (optional): open the SDK serial monitor.
# ---------------------------------------------------------------------------
if ($Monitor) {
    if (-not $Port) {
        throw "No serial port. Pass -Port COMx or set `$env:ESPPORT."
    }
    Write-Host ("== Monitor (port {0}) - Ctrl+] to exit ==" -f $Port) -ForegroundColor Cyan
    & python.exe (Join-Path $env:IDF_PATH 'tools\idf_monitor.py') `
        --port $Port --baud $Baud (Join-Path $BuildDir 'wireless_uart.elf')
}
