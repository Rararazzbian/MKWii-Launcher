<#
.SYNOPSIS
    Builds the launcher on Windows.

.DESCRIPTION
    A wrapper around the CMake presets already in this repository, plus the thing
    a command-line MSVC build needs and does not get for free: the compiler
    environment, which Ninja does not find on its own.

    The default is set up for iterating - incremental, no link-time
    optimisation, and only the launcher rather than every target. Add -LTO when
    building something to keep.

    Output goes to build\<config>\x64\Binaries, which is where the virtual
    network's peer test already expects to find it.

.PARAMETER Config
    release (default) or debug.

.PARAMETER Target
    What to build. Defaults to the launcher alone, which is what you want while
    working. "tests" builds the unit tests, including the virtual network's, and
    "all" builds everything.

.PARAMETER Fresh
    Delete the build directory first.

    Worth knowing: CMake caches which languages are enabled, so a change to that
    - the icon resource fix, for one - is only picked up by a fresh configure.

.PARAMETER LTO
    Link-time optimisation. Off by default because it makes linking
    considerably slower and that cost lands on every single build; worth turning
    on for one you are going to keep and use.

    Toggling it changes compile flags, so the next build after adding or
    removing it is a full rebuild.

.PARAMETER Jobs
    Parallel compile jobs. Defaults to the number of processors.

.EXAMPLE
    .\build.ps1                  # iterate: incremental, launcher only
    .\build.ps1 -LTO             # a build to keep
    .\build.ps1 -Fresh -LTO      # ... from scratch
    .\build.ps1 -Target tests    # unit tests
    .\build.ps1 -Config debug
#>

[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$Config = 'release',

    [string]$Target = 'dolphin-emu',

    [switch]$Fresh,

    [switch]$LTO,

    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = $PSScriptRoot
$buildDir = Join-Path $root "build\$Config\x64"
$configurePreset = "ninja-$Config-x64"

function Fail([string]$message) {
    Write-Host ''
    Write-Host "  $message" -ForegroundColor Red
    Write-Host ''
    exit 1
}

function Step([string]$message) {
    Write-Host ''
    Write-Host "==> $message" -ForegroundColor Cyan
}

# --- The compiler ----------------------------------------------------------
#
# Ninja does not find MSVC on its own the way the Visual Studio generator does;
# it needs the environment that vcvars sets up. Skipped when already inside a
# developer prompt, which is what VSCMD_VER means.

if (-not $env:VSCMD_VER) {
    Step 'Locating Visual Studio'

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        Fail 'vswhere.exe not found. Install Visual Studio with the "Desktop development with C++" workload.'
    }

    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsPath) {
        Fail 'No Visual Studio with the C++ toolset found. Install the "Desktop development with C++" workload.'
    }

    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { Fail "vcvars64.bat not found under $vsPath" }

    Write-Host "    $vsPath"

    # Run vcvars in cmd and copy the environment it produced back into this
    # session, because a batch file cannot change its parent's environment.
    & "${env:COMSPEC}" /s /c "`"$vcvars`" && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

foreach ($tool in @('cmake', 'ninja')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool is not on PATH. Both ship with the Visual Studio C++ workload."
    }
}

# --- Submodules ------------------------------------------------------------
#
# Opus is vendored for voice chat and is not optional, so a tree without
# submodules fails deep inside the configure with something unhelpful.

if (-not (Test-Path (Join-Path $root 'Externals\opus\opus\CMakeLists.txt'))) {
    Step 'Fetching submodules'
    & git -C $root submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { Fail 'git submodule update failed.' }
}

# --- Configure -------------------------------------------------------------

if ($Fresh -and (Test-Path $buildDir)) {
    Step "Removing $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

Step "Configuring ($configurePreset)"

$cacheArgs = @(
    # Off unless asked for: it is a large part of the link time, and that cost
    # lands on every build rather than only on the one being kept.
    "-DENABLE_LTO=$(if ($LTO) { 'ON' } else { 'OFF' })",

    # None of these are wanted by a launcher for one game, and each is build
    # time and binary size that buys nothing.
    '-DUSE_DISCORD_PRESENCE=OFF',
    '-DUSE_MGBA=OFF',
    '-DUSE_RETRO_ACHIEVEMENTS=OFF',
    '-DENABLE_AUTOUPDATE=OFF',

    # The memory inspector's dashboard server. Deliberately off: it opens a
    # listening socket, which is not something to hand to anyone else.
    '-DMKW_MEMORY_INSPECTOR_WEB=OFF'
)

# ccache if it happens to be installed; a large win on a rebuild, no loss if not.
if (Get-Command ccache -ErrorAction SilentlyContinue) {
    $cacheArgs += '-DENABLE_CCACHE=ON'
    Write-Host '    ccache found, enabling'
}

& cmake --preset $configurePreset @cacheArgs
if ($LASTEXITCODE -ne 0) { Fail 'Configure failed.' }

# --- Build -----------------------------------------------------------------

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$buildArgs = @('--build', $buildDir, '--parallel', $Jobs)
# "all" is spelled by not naming a target; Ninja's own "all" target excludes
# things that are deliberately built only on demand.
if ($Target -and $Target -ne 'all') { $buildArgs += @('--target', $Target) }

Step "Building $(if ($Target) { $Target } else { 'everything' }) with $Jobs jobs$(if ($LTO) { ', LTO on' })"

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { Fail 'Build failed.' }
$stopwatch.Stop()

# --- Done ------------------------------------------------------------------

$binaries = Join-Path $buildDir 'Binaries'
# DEBUG_POSTFIX in DolphinQt's CMakeLists appends a D to the debug build.
$exe = Join-Path $binaries $(if ($Config -eq 'debug') { 'DolphinD.exe' } else { 'Dolphin.exe' })

Write-Host ''
Write-Host "==> Built in $([int]$stopwatch.Elapsed.TotalMinutes)m $($stopwatch.Elapsed.Seconds)s" -ForegroundColor Green
if (Test-Path $exe) {
    Write-Host "    $exe"
} else {
    Write-Host "    $binaries"
}

# The build tree is where the user directory lives, this fork being
# unconditionally portable, so deleting it takes the configs and saves with it.
Write-Host ''
Write-Host '    Note: this build keeps its User folder beside the executable.' -ForegroundColor DarkGray
Write-Host '    -Fresh deletes it along with everything else.' -ForegroundColor DarkGray
Write-Host ''
