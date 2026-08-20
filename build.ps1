# Build the app and assemble a release folder, optionally zipped.
#
#   .\build.ps1              build, assemble dist\DndBattlemapGenerator
#   .\build.ps1 -Zip         ...and pack it into a dated .zip beside it
#   .\build.ps1 -Clean       throw away the build folder and configure from scratch
#
# The release folder is assembled by CMake on every build, so a plain
# `cmake --build build --config Release` does the same job; this script just
# adds the checks that are easy to forget.
param([switch]$Zip, [switch]$Clean)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

# The packaging step deletes and rewrites dist\, which fails while the packaged
# app is open. Close it rather than failing halfway through.
$running = Get-Process DndBattlemapGenerator -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "Closing the running app so the release folder can be rewritten..."
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 700
}

if ($Clean -and (Test-Path build)) {
    Write-Host "Removing the old build folder..."
    Remove-Item -Recurse -Force build
}

if (-not (Test-Path "build\CMakeCache.txt")) {
    Write-Host "Configuring..."
    cmake -B build -S . -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
}

Write-Host "Building..."
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$dist = Join-Path $PSScriptRoot "dist\DndBattlemapGenerator"
$exe = Join-Path $dist "DndBattlemapGenerator.exe"
if (-not (Test-Path $exe)) { throw "The build finished but $exe is missing." }

# A release with no styles or no tools still starts and is still useless.
$styles = (Get-ChildItem (Join-Path $dist "styles") -Filter *.json).Count
$tools = (Get-ChildItem (Join-Path $dist "tools") -Filter *.py).Count
$manual = Test-Path (Join-Path $dist "docs\Manual.pdf")
Write-Host ""
Write-Host ("Release: {0}" -f $dist)
Write-Host ("  styles : {0}" -f $styles)
Write-Host ("  tools  : {0}" -f $tools)
Write-Host ("  manual : {0}" -f $(if ($manual) { "yes" } else { "MISSING" }))
if ($styles -lt 10 -or $tools -lt 8 -or -not $manual) {
    throw "The release folder looks incomplete."
}

if ($Zip) {
    $stamp = Get-Date -Format "yyyy-MM-dd"
    $zipPath = Join-Path $PSScriptRoot ("dist\DndBattlemapGenerator-" + $stamp + ".zip")
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path $dist -DestinationPath $zipPath -CompressionLevel Optimal
    $size = (Get-Item -LiteralPath $zipPath).Length / 1MB
    Write-Host ""
    Write-Host ("Packed: {0}  ({1:N1} MB)" -f $zipPath, $size)
}

Write-Host ""
Write-Host "Done. Hand the folder (or the zip) to anyone - it needs no installer."
