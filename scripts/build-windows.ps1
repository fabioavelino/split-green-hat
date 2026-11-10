# Builds the Windows x64 split-green-hat kit.
#
# Requirements: Visual Studio 2022 C++ tools, CMake, Ninja, Qt 6.8.3
# (msvc2022_64 with qtcharts), glslangValidator on PATH.
#
# Usage:
#   .\scripts\build-windows.ps1 -QtDir C:\Qt\6.8.3\msvc2022_64
# or set QTDIR.

param(
    [string]$QtDir = $env:QTDIR,
    [int]$Jobs = 8,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

if (-not $QtDir) {
    throw "QtDir is required. Pass -QtDir or set QTDIR to the Qt 6.8.3 msvc2022_64 folder."
}

$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "fritchii-src"
$build = Join-Path $src "build\split"

& (Join-Path $PSScriptRoot "fetch-fritchii.ps1")

# Fritchii turns four MSVC warning classes into errors; its own CI suppresses
# them the same way. MSVC appends _CL_ after every other flag.
$env:_CL_ = "/wd4456 /wd4457 /wd4458 /wd4459"

cmake -S $src -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=$Configuration `
    -DBUILD_TESTING=OFF `
    -DYUZU_TESTS=OFF `
    -DYUZU_CMD=OFF `
    -DYUZU_ROOM=OFF `
    -DYUZU_ROOM_STANDALONE=OFF `
    -DYUZU_USE_BUNDLED_QT=OFF `
    -DYUZU_USE_BUNDLED_SDL3=ON `
    -DENABLE_UPDATE_CHECKER=OFF `
    -DUSE_DISCORD_PRESENCE=OFF `
    -DLICHII_SPLIT=ON `
    "-DLICHII_SPLIT_DIR=$root" `
    "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$root\cmake\trim_paths.cmake" `
    "-DCMAKE_PREFIX_PATH=$QtDir"

if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed"
}

cmake --build $build --target yuzu --parallel $Jobs
if ($LASTEXITCODE -ne 0) {
    throw "Build failed"
}

$bin = Join-Path $build "bin"
$exe = Join-Path $bin "lichii.exe"
if (-not (Test-Path $exe)) {
    throw "Build did not produce $exe"
}

windeployqt --release --no-translations $exe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed"
}

$userConfig = Join-Path $bin "user\config"
New-Item -ItemType Directory -Force -Path $userConfig | Out-Null
Copy-Item (Join-Path $root "config\split.ini") (Join-Path $userConfig "split.ini") -Force
Copy-Item (Join-Path $root "README.md") (Join-Path $bin "README.md") -Force
Copy-Item (Join-Path $root "LICENSE") (Join-Path $bin "LICENSE") -Force
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") (Join-Path $bin "THIRD_PARTY_NOTICES.md") -Force

$zip = Join-Path $root "split-green-hat-windows-x64.zip"
if (Test-Path $zip) {
    Remove-Item $zip -Force
}
Compress-Archive -Path (Join-Path $bin "*") -DestinationPath $zip

Write-Host "Kit: $zip"
