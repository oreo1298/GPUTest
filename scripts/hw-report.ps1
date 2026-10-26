<#
.SYNOPSIS
  Hardware report: runs a validation suite on every GPU and bundles results JSON,
  logs, system information and rendered frames ("screenshots") into one zip.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File hw-report.ps1
  powershell -ExecutionPolicy Bypass -File hw-report.ps1 -Mode standard -Api d3d12 -Gpu 0
  Then commit the zip to hardware-reports/ or attach it to the session.
#>
param(
    [string]$Runner = "",
    [ValidateSet("quick", "standard", "stress")][string]$Mode = "quick",
    [string]$Stress = "20m",
    [string]$Api = "",
    [string]$Gpu = "",
    [string]$OutDir = (Get-Location).Path
)
$ErrorActionPreference = "Continue"

if (-not $Runner) {
    $candidates = @(
        (Join-Path $PSScriptRoot "*-run.exe"),
        (Join-Path $PSScriptRoot "..\*-run.exe"),
        (Join-Path $PSScriptRoot "..\build\*\bin\*-run.exe")
    )
    foreach ($c in $candidates) {
        $found = Get-ChildItem -Path $c -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $Runner = $found.FullName; break }
    }
}
if (-not $Runner -or -not (Test-Path $Runner)) {
    Write-Error "Cannot find the benchmark runner (*-run.exe). Pass -Runner PATH."
    exit 1
}
Write-Host "runner: $Runner"

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$name = "hwreport-$($env:COMPUTERNAME)-$stamp"
$work = Join-Path ([System.IO.Path]::GetTempPath()) $name
foreach ($d in "system", "runs", "frames", "logs") { New-Item -ItemType Directory -Force -Path (Join-Path $work $d) | Out-Null }
$sys = Join-Path $work "system"
$logs = Join-Path $work "logs"

Get-CimInstance Win32_OperatingSystem | Format-List * | Out-File (Join-Path $sys "os.txt")
Get-CimInstance Win32_Processor | Format-List * | Out-File (Join-Path $sys "cpu.txt")
Get-CimInstance Win32_PhysicalMemory | Format-List * | Out-File (Join-Path $sys "memory.txt")
Get-CimInstance Win32_VideoController | Format-List * | Out-File (Join-Path $sys "gpus.txt")
Get-CimInstance Win32_BaseBoard | Format-List * | Out-File (Join-Path $sys "board.txt")
Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Format-List * | Out-File (Join-Path $sys "pnp-display.txt")
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) { nvidia-smi -q | Out-File (Join-Path $sys "nvidia-smi.txt") }
if (Get-Command vulkaninfo -ErrorAction SilentlyContinue) { vulkaninfo --summary 2>&1 | Out-File (Join-Path $sys "vulkaninfo.txt") }
Write-Host "running dxdiag (takes ~20 s)..."
Start-Process -FilePath "dxdiag.exe" -ArgumentList "/t", (Join-Path $sys "dxdiag.txt") -Wait -ErrorAction SilentlyContinue
Get-WinEvent -FilterHashtable @{LogName = "System"; ProviderName = "Display"} -MaxEvents 100 -ErrorAction SilentlyContinue |
    Format-List TimeCreated, Id, Message | Out-File (Join-Path $sys "display-events.txt")   # TDR events (ID 4101)

& $Runner --version *> (Join-Path $sys "runner-version.txt")
& $Runner --list-devices --json 2> (Join-Path $logs "list-devices.log") | Out-File -Encoding utf8 (Join-Path $sys "devices.json")
& $Runner --list-tests --json 2> $null | Out-File -Encoding utf8 (Join-Path $sys "tests.json")

$devices = Get-Content (Join-Path $sys "devices.json") -Raw | ConvertFrom-Json
$pairs = @()
foreach ($a in $devices.apis) {
    if ($Api -and $a.api -ne $Api) { continue }
    $hw = @($a.adapters | Where-Object { -not $_.software })
    if ($hw.Count -eq 0) { $hw = @($a.adapters) }
    foreach ($g in $hw) {
        if ($Gpu -and "$($g.index)" -ne $Gpu) { continue }
        $pairs += [pscustomobject]@{ Api = $a.api; Gpu = $g.index }
    }
}
if ($pairs.Count -eq 0) { Write-Warning "no GPUs found by the runner; see logs\list-devices.log" }

foreach ($p in $pairs) {
    $tag = "$($p.Api)-gpu$($p.Gpu)"
    Write-Host "== $tag"
    & $Runner --api $p.Api --gpu $p.Gpu --sysinfo 2> (Join-Path $logs "sysinfo-$tag.log") | Out-File -Encoding utf8 (Join-Path $sys "sysinfo-$tag.json")
    & $Runner --api $p.Api --gpu $p.Gpu --caps --json 2> (Join-Path $logs "caps-$tag.log") | Out-File -Encoding utf8 (Join-Path $sys "caps-$tag.json")
    & $Runner --api $p.Api --gpu $p.Gpu --check-support *> (Join-Path $sys "support-$tag.txt")
    & $Runner --api $p.Api --gpu $p.Gpu --suite smoke --validation --resolution 1280x720 `
        --dump-frames (Join-Path $work "frames\$tag-smoke") --out (Join-Path $work "runs\$tag-smoke.json") `
        --log-file (Join-Path $logs "$tag-smoke.log") --progress jsonl *> (Join-Path $logs "$tag-smoke.jsonl")
    "$tag smoke exit code $LASTEXITCODE" | Tee-Object -Append -FilePath (Join-Path $logs "exit-codes.txt")
    switch ($Mode) {
        "quick" { $extra = @("--suite", "quick") }
        "standard" { $extra = @("--suite", "standard") }
        "stress" { $extra = @("--suite", "quick", "--loop", $Stress) }
    }
    & $Runner --api $p.Api --gpu $p.Gpu @extra --dump-frames (Join-Path $work "frames\$tag-$Mode") `
        --out (Join-Path $work "runs\$tag-$Mode.json") --log-file (Join-Path $logs "$tag-$Mode.log") `
        --progress jsonl *> (Join-Path $logs "$tag-$Mode.jsonl")
    "$tag $Mode exit code $LASTEXITCODE" | Tee-Object -Append -FilePath (Join-Path $logs "exit-codes.txt")
}

$zip = Join-Path $OutDir "$name.zip"
Compress-Archive -Path $work -DestinationPath $zip -Force
Write-Host ""
Write-Host "Report written to: $zip"
Write-Host "Commit it to hardware-reports/ (or attach it to the session)."
