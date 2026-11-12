# Fetches Fritchii at the commit in lichii/PIN and applies our patch series.
# Run from PowerShell on Windows, or from CI with pwsh.

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$pinPath = Join-Path $root "lichii\PIN"
$patchDir = Join-Path $root "lichii\patches"
$dest = Join-Path $root "fritchii-src"

if (-not (Test-Path $pinPath)) {
    throw "Missing $pinPath"
}

$pin = ConvertFrom-StringData (Get-Content $pinPath -Raw)
$repo = $pin.repo
$tag = $pin.tag
$commit = $pin.commit

if (-not $repo -or -not $tag -or -not $commit) {
    throw "lichii/PIN must contain repo, tag and commit"
}

Write-Host "Fritchii: $repo at $tag ($commit)"

if (Test-Path (Join-Path $dest ".git")) {
    Write-Host "Updating existing checkout"
    git -C $dest fetch --depth 1 --tags --force origin
} else {
    Write-Host "Cloning into $dest"
    git clone --filter=blob:none --no-checkout $repo $dest
    if ($LASTEXITCODE -ne 0) {
        throw "git clone failed"
    }
    git -C $dest fetch --depth 1 --tags --force origin
}

git -C $dest checkout --force "refs/tags/$tag"
if ($LASTEXITCODE -ne 0) {
    throw "Cannot check out tag $tag"
}
git -C $dest reset --hard $commit
git -C $dest clean -fd

$head = (git -C $dest rev-parse HEAD).Trim()
if ($head -ne $commit) {
    throw "Fritchii checkout is $head, expected $commit"
}

$patches = Get-ChildItem $patchDir -Filter "*.patch" | Sort-Object Name
if (-not $patches) {
    throw "No patches found in $patchDir"
}

foreach ($patch in $patches) {
    Write-Host "Applying $($patch.Name)"
    git -C $dest apply --check --ignore-whitespace $patch.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "Patch $($patch.Name) does not apply to Fritchii $commit"
    }
    git -C $dest apply --ignore-whitespace $patch.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to apply $($patch.Name)"
    }
}

# Our own changes to the co-op submodule (the camera override used by M2).
# These are full files, not a patch: Windows checkout line endings cannot
# break them. The submodule stays pinned to its upstream commit.
$overrideDir = Join-Path $root "lichii\submodule-overrides"
$submodule = Join-Path $root "external\oot-remake-multiplayer"
$hookDir = Join-Path $submodule "lichii\hook"
if (Test-Path $overrideDir) {
    foreach ($name in @("coop_camera.h", "coop_camera.cpp", "coop_camera_track.h",
                    "coop_puppet.h", "coop_puppet.cpp")) {
        $src = Join-Path $overrideDir $name
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $hookDir $name) -Force
        }
    }
}

Write-Host "Fritchii ready at $dest"
