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
$pythonArgs = @()
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    $python = Get-Command py -ErrorAction SilentlyContinue
    if ($python) {
        $pythonArgs = @("-3")
    }
}
if (-not $python) {
    throw "Python 3 is required to build the puppet pack. Install it with: winget install -e --id Python.Python.3.13 --scope user --accept-package-agreements --accept-source-agreements  Then open a new terminal."
}

# Native stderr is an ErrorRecord in Windows PowerShell. Keep the preference
# non-terminating here so a missing module reaches the install step instead
# of aborting the script.
$ErrorActionPreference = "Continue"

& $python.Source @pythonArgs -c "import zstandard" 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installing the 'zstandard' Python package..."
    & $python.Source @pythonArgs -m pip install --user zstandard
    if ($LASTEXITCODE -ne 0) {
        throw "Could not install zstandard. Run: $($python.Source) $($pythonArgs -join ' ') -m pip install --user zstandard"
    }
}

$env:PYTHONPATH = $PSScriptRoot
& $python.Source @pythonArgs $tool --romfs $Nup --user-dir $UserDir --mod-name $ModName
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
