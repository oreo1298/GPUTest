# Builds the Windows portable zip (and with -Installer the Inno Setup installer) from an existing CMake build
# directory.
#   pwsh scripts/package-windows.ps1 -BuildDir build/ci-windows [-OutDir dist] [-Installer]
# Needs: windeployqt of the Qt the GUI was built with on PATH, and a VS developer
# environment (VCToolsRedistDir) so the MSVC runtime DLLs can be shipped app-locally.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$OutDir = "dist",
    [switch]$Installer
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$info = @{}
Get-Content "$root/cmake/AppInfo.cmake" | ForEach-Object {
    if ($_ -match '^set\((APP_[A-Z_]+) "([^"]*)"\)') { $info[$Matches[1]] = $Matches[2] }
}
# Values may refer to earlier ones (APP_GUI_NAME is "${APP_ID}"): expand them.
foreach ($k in @($info.Keys)) {
    foreach ($j in @($info.Keys)) { $info[$k] = $info[$k].Replace("`${$j}", $info[$j]) }
}
if ($info.APP_GUI_NAME -match '\$\{') { throw "could not read cmake/AppInfo.cmake" }
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
    $deployArgs = @("--release", "--no-translations", "--no-system-d3d-compiler", "--no-opengl-sw", "--no-compiler-runtime")
    # Only the SQLite driver is used (run history); skip drivers that need client libraries we do not ship.
    if ((windeployqt --help 2>&1 | Out-String) -match "exclude-plugins") {
        $deployArgs += @("--exclude-plugins", "qsqlmimer,qsqlodbc,qsqlpsql,qsqlmysql,qsqlibase,qsqloci,qsqldb2")
    }
    windeployqt @deployArgs $gui
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

if ($Installer) {
    # Inno Setup 6: on PATH, in its default folder, or installed with Chocolatey.
    $iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
    if (-not $iscc) { $iscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe" }
    if (-not (Test-Path $iscc)) {
        choco install innosetup -y --no-progress | Out-Host
        $iscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"
    }
    if (-not (Test-Path $iscc)) { throw "Inno Setup (ISCC.exe) not found" }
    $isccArgs = @("/Q", "/DAppName=$($info.APP_DISPLAY_NAME)", "/DAppVersion=$($info.APP_VERSION)",
                  "/DAppUrl=$($info.APP_HOMEPAGE)", "/DGuiExe=$($info.APP_GUI_NAME).exe", "/DSourceDir=$stage",
                  "/DOutputDir=$(Resolve-Path $OutDir)", "/DOutputBaseFilename=$name-setup",
                  (Join-Path $root "packaging/windows/installer.iss"))
    & $iscc @isccArgs
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
    Write-Host "Installer: $(Join-Path (Resolve-Path $OutDir) "$name-setup.exe")"
}
