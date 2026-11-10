# Builds the M1 debug-puppet pack from the user's own .nup.
#
# Usage:
#   .\make-puppet-pack.ps1 -Nup "C:\path\game.nup"
#   .\make-puppet-pack.ps1 -Nup "C:\path\game.nup" -UserDir "C:\kit\user"

param(
    [string]$Nup,
    [string]$UserDir = (Join-Path $PSScriptRoot "user"),
    [string]$ModName = "split-puppet"
)

$ErrorActionPreference = "Stop"

if (-not $Nup) {
    $Nup = Read-Host "Path to your Ocarina of Time .nup file"
}
if (-not (Test-Path $Nup)) {
    throw "Game file not found: $Nup"
}

$tool = Join-Path $PSScriptRoot "tools\puppet_pack\build.py"
if (-not (Test-Path $tool)) {
    throw "Missing $tool. Extract the whole kit first."
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    throw "Python 3 is required to build the puppet pack. Install Python 3 from python.org, then run this again."
}

& $python.Source -c "import zstandard" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installing the 'zstandard' Python package..."
    & $python.Source -m pip install --user zstandard
    if ($LASTEXITCODE -ne 0) {
        throw "Could not install zstandard. Run: $($python.Source) -m pip install --user zstandard"
    }
}

$env:PYTHONPATH = $PSScriptRoot
& $python.Source $tool --romfs $Nup --user-dir $UserDir --mod-name $ModName
if ($LASTEXITCODE -ne 0) {
    throw "The puppet pack tool failed."
}

$mod = Join-Path $UserDir "load\0400486024DEC000\$ModName"
if (-not (Test-Path $mod)) {
    throw "The tool finished but $mod was not created."
}

Write-Host ""
Write-Host "Puppet pack ready: $mod"
Write-Host "The M1 kit loads it automatically when the game starts."
