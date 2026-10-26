# Windows CI smoke test. Until the D3D12/D3D11 backends land (WARP), this checks that the
# runner starts, enumerates devices (none on CI runners) and lists tests and suites.
param([Parameter(Mandatory = $true)][string]$BuildDir)
$ErrorActionPreference = "Stop"
$runner = Get-ChildItem (Join-Path $BuildDir "bin\*-run.exe") | Select-Object -First 1
if (-not $runner) { throw "runner not found in $BuildDir\bin" }
$out = Join-Path $BuildDir "smoke"
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $runner.FullName --version
if ($LASTEXITCODE -ne 0) { throw "--version failed" }
& $runner.FullName --list-devices --json | Out-File -Encoding utf8 (Join-Path $out "devices.json")
if ($LASTEXITCODE -ne 0) { throw "--list-devices failed" }
Get-Content (Join-Path $out "devices.json") -Raw | ConvertFrom-Json | Out-Null
& $runner.FullName --list-tests --json | Out-File -Encoding utf8 (Join-Path $out "tests.json")
if ($LASTEXITCODE -ne 0) { throw "--list-tests failed" }
$tests = Get-Content (Join-Path $out "tests.json") -Raw | ConvertFrom-Json
if ($tests.Count -lt 1) { throw "no tests registered" }
& $runner.FullName --list-suites | Out-Host
Write-Host "windows smoke OK ($($tests.Count) tests)"
