# Windows CI smoke test: the runner starts and lists tests and suites; then, on Direct3D 12 with
# WARP (CI runners have no GPU), the smoke suite and every scene run offscreen with the debug layer,
# and must complete with zero debug-layer errors. The standard suite also runs at smoke size; its
# outcome is reported (annotations) without failing the job until every test is ported.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [switch]$StrictStandard,
    [int]$StandardTimeoutMinutes = 25
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$runner = Get-ChildItem (Join-Path $BuildDir "bin\*-run.exe") | Select-Object -First 1
if (-not $runner) { throw "runner not found in $BuildDir\bin" }
$out = Join-Path $BuildDir "smoke"
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Run([string]$what, [string[]]$runnerArgs, [int[]]$okCodes = @(0)) {
    Write-Host "== $what"
    & $runner.FullName @runnerArgs
    $code = $LASTEXITCODE
    if ($okCodes -notcontains $code) { throw "$what failed (exit $code)" }
    return $code
}

Run "version" @("--version") | Out-Null
& $runner.FullName --list-devices --json | Out-File -Encoding utf8 (Join-Path $out "devices.json")
if ($LASTEXITCODE -ne 0) { throw "--list-devices failed" }
$devices = Get-Content (Join-Path $out "devices.json") -Raw | ConvertFrom-Json
& $runner.FullName --list-tests --json | Out-File -Encoding utf8 (Join-Path $out "tests.json")
if ($LASTEXITCODE -ne 0) { throw "--list-tests failed" }
$tests = Get-Content (Join-Path $out "tests.json") -Raw | ConvertFrom-Json
if ($tests.Count -lt 1) { throw "no tests registered" }
& $runner.FullName --list-suites | Out-Host

$d3d12 = $devices.apis | Where-Object { $_.api -eq "d3d12" }
if (-not $d3d12 -or -not $d3d12.adapters -or $d3d12.adapters.Count -lt 1) {
    throw "no Direct3D 12 adapter (WARP) found: $(Get-Content (Join-Path $out 'devices.json') -Raw)"
}
Write-Host "Direct3D 12 adapters: $(($d3d12.adapters | ForEach-Object { $_.name }) -join ', ')"
& $runner.FullName --api d3d12 --caps --json | Out-File -Encoding utf8 (Join-Path $out "caps-d3d12.json")
if ($LASTEXITCODE -ne 0) { throw "--caps --api d3d12 failed" }
& $runner.FullName --api d3d12 --check-support | Out-File -Encoding utf8 (Join-Path $out "support-d3d12.txt")
Write-Host "== D3D12 capabilities (WARP)"
& $runner.FullName --api d3d12 --caps | Out-Host
Get-Content (Join-Path $out "support-d3d12.txt") | Out-Host

$common = @("--api", "d3d12", "--offscreen", "--resolution", "320x180", "--validation", "--fail-on-validation",
            "--log-level", "info")

# Core: must pass.
Run "D3D12 smoke suite" ($common + @("--suite", "smoke", "--dump-frames", "$out\frames-d3d12",
                                      "--out", "$out\smoke-d3d12.json", "--log-file", "$out\smoke-d3d12.log")) | Out-Null
python "$root/scripts/check-results.py" "$out\smoke-d3d12.json" --require-completed
if ($LASTEXITCODE -ne 0) { throw "D3D12 smoke results check failed" }

$scenes = "scene.lattice,scene.courtyard,scene.courtyard.lite,scene.atrium,scene.atrium.lite,scene.skyline,scene.glasshouse,scene.gallery"
Run "D3D12 scenes" ($common + @("--test", $scenes, "--duration", "smoke", "--quality", "low",
                                 "--dump-frames", "$out\scene-frames-d3d12", "--out", "$out\scenes-d3d12.json",
                                 "--log-file", "$out\scenes-d3d12.log")) | Out-Null
python "$root/scripts/check-results.py" "$out\scenes-d3d12.json" --require-completed
if ($LASTEXITCODE -ne 0) { throw "D3D12 scene results check failed" }

# Broad: every other test at smoke size (report only unless -StrictStandard).
# Bounded: a hang on WARP must not hold the job until GitHub's six-hour limit.
Write-Host "== D3D12 standard suite (smoke size)"
$standardArgs = $common + @("--suite", "standard", "--duration", "smoke", "--quality", "low",
                            "--out", "$out\standard-d3d12.json", "--log-file", "$out\standard-d3d12.log")
$proc = Start-Process -FilePath $runner.FullName -ArgumentList $standardArgs -NoNewWindow -PassThru
$null = $proc.Handle # keeps the exit code readable after WaitForExit(timeout)
if ($proc.WaitForExit($StandardTimeoutMinutes * 60 * 1000)) {
    $code = $proc.ExitCode
} else {
    $proc.Kill()
    $code = 124
    Write-Host "::warning title=D3D12 standard suite::timed out after $StandardTimeoutMinutes min; last log lines follow"
    if (Test-Path "$out\standard-d3d12.log") { Get-Content "$out\standard-d3d12.log" -Tail 25 | Out-Host }
}
if (Test-Path "$out\standard-d3d12.json") {
    $r = Get-Content "$out\standard-d3d12.json" -Raw | ConvertFrom-Json
    foreach ($t in $r.tests) {
        $line = "{0,-28} {1,-10} {2}" -f $t.id, $t.status, ($(if ($t.verdict) { "$($t.verdict): $($t.verdict_message)" } else { $t.status_message }))
        if ($t.status -ne "completed" -and $t.status -ne "skipped" -or $t.verdict -eq "fail") {
            Write-Host "::warning title=D3D12 $($t.id)::$line"
        } else {
            Write-Host $line
        }
    }
    Write-Host "debug-layer errors: $($r.validation.errors)"
}
if ($StrictStandard -and $code -ne 0) { throw "D3D12 standard suite failed (exit $code)" }
Write-Host "windows smoke OK ($($tests.Count) tests; D3D12 standard suite exit $code)"
