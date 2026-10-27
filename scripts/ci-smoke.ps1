# Windows CI smoke test: the runner starts and lists tests and suites; then, on Direct3D 12 with
# WARP (CI runners have no GPU), the smoke suite and every scene run offscreen with the debug layer,
# and must complete with zero debug-layer errors; so must the Direct3D 11 smoke and legacy runs. The standard suite also runs at smoke size, one
# bounded process per test; its outcome is reported (annotations) without failing the job until every
# test is ported.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [switch]$StrictStandard,
    [int]$StandardTimeoutMinutes = 30,
    [int]$PerTestTimeoutMinutes = 4
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

# Broad: every standard-suite test at smoke size, each in its own bounded process, so that a hang on
# WARP shows up as one timed-out test (with its log) instead of hiding the tests after it. Report
# only unless -StrictStandard.
Write-Host "== D3D12 standard suite (smoke size, one process per test)"
$stdDir = Join-Path $out "standard-d3d12"
New-Item -ItemType Directory -Force -Path $stdDir | Out-Null
$stdCommon = @("--api", "d3d12", "--offscreen", "--resolution", "320x180", "--validation", "--fail-on-validation",
               "--duration", "smoke", "--quality", "low", "--log-level", "debug")
$standard = @($tests | Where-Object { $_.suites -contains "standard" } | ForEach-Object { $_.id })
$deadline = (Get-Date).AddMinutes($StandardTimeoutMinutes)
$problems = 0
$code = 0
foreach ($id in $standard) {
    $remaining = [int](($deadline - (Get-Date)).TotalSeconds)
    if ($remaining -le 0) {
        Write-Host "::warning title=D3D12 standard suite::time budget of $StandardTimeoutMinutes min used up before $id"
        $problems++
        break
    }
    $json = Join-Path $stdDir "$id.json"
    $log = Join-Path $stdDir "$id.log"
    $testArgs = $stdCommon + @("--test", $id, "--out", $json, "--log-file", $log)
    $started = Get-Date
    $proc = Start-Process -FilePath $runner.FullName -ArgumentList $testArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $stdDir "$id.out") -RedirectStandardError (Join-Path $stdDir "$id.err")
    $null = $proc.Handle # keeps the exit code readable after WaitForExit(timeout)
    $limit = [Math]::Min($PerTestTimeoutMinutes * 60, $remaining)
    if ($proc.WaitForExit($limit * 1000)) {
        $exit = $proc.ExitCode
    } else {
        $proc.Kill()
        $exit = 124
    }
    $secs = [int]((Get-Date) - $started).TotalSeconds
    $line = "{0,-26} exit {1,-4} {2,4}s" -f $id, $exit, $secs
    $bad = $exit -ne 0
    $warn = $false
    if (Test-Path $json) {
        $r = Get-Content $json -Raw | ConvertFrom-Json
        $t = $r.tests | Select-Object -First 1
        if ($t) {
            $msg = if ($t.verdict) { "$($t.verdict): $($t.verdict_message)" } else { $t.status_message }
            $line += " {0,-10} {1}" -f $t.status, $msg
            if ($t.status -ne "completed" -and $t.status -ne "skipped" -or $t.verdict -eq "fail") { $bad = $true }
            if ($t.verdict -eq "warn") { $warn = $true }
        }
        if ($r.validation.errors -gt 0) { $line += " [debug-layer errors: $($r.validation.errors)]"; $bad = $true }
    }
    if ($bad) {
        $problems++
        $code = if ($exit -ne 0) { $exit } else { 1 }
        Write-Host "::warning title=D3D12 $($id)::$line"
        if ($exit -eq 124) { Write-Host "timed out after $limit s; last log lines:" }
        if (Test-Path $log) { Get-Content $log -Tail 30 | Out-Host }
    } else {
        Write-Host $line
        if ($warn -and (Test-Path $log)) { Get-Content $log -Tail 12 | Out-Host } # a warning verdict explains itself in the log
    }
}
Write-Host "D3D12 standard suite: $($standard.Count) tests, $problems with problems"

# Upscaler plugins on D3D12 (report only: XeSS on WARP may be refused or slow). Bounded like the suite.
Write-Host "== Upscaler plugins (D3D12, report only)"
$pluginArgs = $stdCommon + @("--test", "feature.upscale", "--upscaler-plugins", "--out", "$out\plugins-d3d12.json",
                             "--log-file", "$out\plugins-d3d12.log")
$proc = Start-Process -FilePath $runner.FullName -ArgumentList $pluginArgs -NoNewWindow -PassThru `
    -RedirectStandardOutput "$out\plugins-d3d12.out" -RedirectStandardError "$out\plugins-d3d12.err"
$null = $proc.Handle
if (-not $proc.WaitForExit(10 * 60 * 1000)) { $proc.Kill(); Write-Host "::warning title=Upscaler plugins::timed out" }
if (Test-Path "$out\plugins-d3d12.json") {
    $r = Get-Content "$out\plugins-d3d12.json" -Raw | ConvertFrom-Json
    $t = $r.tests | Select-Object -First 1
    Write-Host "feature.upscale: $($t.status) $($t.verdict) $($t.verdict_message)"
    foreach ($p in $t.details.plugins) { Write-Host ("  {0}: {1} {2}" -f $p.file, $p.status, ($p | ConvertTo-Json -Compress)) }
} elseif (Test-Path "$out\plugins-d3d12.log") {
    Get-Content "$out\plugins-d3d12.log" -Tail 20 | Out-Host
}
if ($StrictStandard -and $problems -gt 0) { throw "D3D12 standard suite failed ($problems tests)" }

# Direct3D 11 on WARP: the smoke suite, the legacy scenes checked against the Direct3D 12 frames
# above (same rasterizer, so they must match), and the legacy suite. Must pass; runs after the
# report-only D3D12 suite so that one failure does not hide the other's report.
$d3d11 = $devices.apis | Where-Object { $_.api -eq "d3d11" }
if (-not $d3d11 -or -not $d3d11.adapters -or $d3d11.adapters.Count -lt 1) {
    throw "no Direct3D 11 adapter (WARP) found: $(Get-Content (Join-Path $out 'devices.json') -Raw)"
}
Write-Host "== D3D11 capabilities (WARP)"
& $runner.FullName --api d3d11 --caps | Out-Host
& $runner.FullName --api d3d11 --check-support | Out-Host
$common11 = @("--api", "d3d11", "--offscreen", "--resolution", "320x180", "--validation", "--fail-on-validation",
              "--log-level", "info")
# Each step reports its failure and the next one still runs; the job fails at the end.
$failures = @()
function Step([string]$what, [scriptblock]$body) {
    try { & $body } catch {
        Write-Host "::error title=$($what)::$($_.Exception.Message)"
        $script:failures += "$($what): $($_.Exception.Message)"
    }
}
Step "D3D11 smoke suite" {
    Run "D3D11 smoke suite" ($common11 + @("--suite", "smoke", "--out", "$out\smoke-d3d11.json",
                                            "--log-file", "$out\smoke-d3d11.log")) | Out-Null
    python "$root/scripts/check-results.py" "$out\smoke-d3d11.json" --require-completed
    if ($LASTEXITCODE -ne 0) { throw "D3D11 smoke results check failed" }
}
Step "D3D11 scenes" {
    python "$root/scripts/update-golden.py" "$out\scenes-d3d12.json" "$out\golden-d3d12" --tolerance 8
    if ($LASTEXITCODE -ne 0) { throw "could not write the D3D12 reference frames" }
    $legacyScenes = "scene.lattice,scene.courtyard,scene.courtyard.lite,scene.atrium,scene.atrium.lite"
    Run "D3D11 scenes (against the D3D12 frames)" ($common11 + @("--test", $legacyScenes, "--duration", "smoke",
            "--quality", "low", "--dump-frames", "$out\scene-frames-d3d11", "--golden", "$out\golden-d3d12",
            "--out", "$out\scenes-d3d11.json", "--log-file", "$out\scenes-d3d11.log")) @(0, 2, 7) | Out-Null
    python "$root/scripts/check-results.py" "$out\scenes-d3d11.json" --require-completed --require-golden-pass
    if ($LASTEXITCODE -ne 0) { throw "D3D11 scene results check failed" }
}
Step "D3D11 legacy suite" {
    Run "D3D11 legacy suite" ($common11 + @("--suite", "legacy", "--duration", "smoke", "--quality", "low",
            "--out", "$out\legacy-d3d11.json", "--log-file", "$out\legacy-d3d11.log")) @(0, 2, 7) | Out-Null
    $legacy = Get-Content "$out\legacy-d3d11.json" -Raw | ConvertFrom-Json
    foreach ($t in $legacy.tests) {
        "{0,-26} {1,-10} {2}" -f $t.id, $t.status, $(if ($t.verdict) { "$($t.verdict): $($t.verdict_message)" } else { $t.status_message }) | Out-Host
    }
    python "$root/scripts/check-results.py" "$out\legacy-d3d11.json" --require-completed
    if ($LASTEXITCODE -ne 0) { throw "D3D11 legacy suite results check failed" }
    $failed = @($legacy.tests | Where-Object { $_.verdict -eq "fail" } | ForEach-Object { $_.id })
    if ($failed.Count) { throw "failed verdicts: $($failed -join ', ')" }
}
if ($failures.Count) { throw "Direct3D 11: $($failures -join '; ')" }
Write-Host "windows smoke OK ($($tests.Count) tests; D3D12 standard suite: $problems with problems)"
