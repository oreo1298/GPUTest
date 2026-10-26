# Builds the Windows portable zip from an existing CMake build directory.
#   pwsh scripts/package-windows.ps1 -BuildDir build/ci-windows [-OutDir dist]
# Needs: windeployqt of the Qt the GUI was built with on PATH, and a VS developer
# environment (VCToolsRedistDir) so the MSVC runtime DLLs can be shipped app-locally.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$info = @{}
Get-Content "$root/cmake/AppInfo.cmake" | ForEach-Object {
    if ($_ -match '^set\((APP_[A-Z_]+) "([^"]*)"\)') { $info[$Matches[1]] = $Matches[2] }
}
$rev = (git -C $root rev-parse --short=8 HEAD 2>$null)
if (-not $rev) { $rev = "local" }
$name = "$($info.APP_DISPLAY_NAME)-$($info.APP_VERSION)-$rev-windows-x64"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage = Join-Path (Resolve-Path $OutDir) $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }

cmake --install $BuildDir --prefix $stage
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed" }

$gui = Join-Path $stage "$($info.APP_GUI_NAME).exe"
if (Test-Path $gui) {
    windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime $gui
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }
}

# App-local MSVC runtime (permitted by the VC++ redistributable terms).
if ($env:VCToolsRedistDir) {
    $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
    if ($crt) { Copy-Item "$($crt.FullName)\*.dll" $stage }
}

Copy-Item "$root/LICENSE" "$stage/LICENSE.txt"
if (Test-Path "$root/THIRD_PARTY.md") { Copy-Item "$root/THIRD_PARTY.md" $stage }
if (Test-Path "$root/scripts/hw-report.ps1") { Copy-Item "$root/scripts/hw-report.ps1" $stage }
Set-Content -Path "$stage/portable.txt" -Value "Settings and results are stored next to the executables while this file exists."

$zip = Join-Path (Resolve-Path $OutDir) "$name-portable.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage/*" -DestinationPath $zip
Write-Host "Portable zip: $zip"
