<#
.SYNOPSIS
  One command that builds arqsim, runs the tests, demonstrates every feature
  and runs every experiment, printing all results in detail.

.EXAMPLE
  .\run            # everything (full sweep of all presets, ~8-10 min)
  .\run -Quick     # same, but the short 'quick' sweep instead of all presets
  .\run -NoSweep   # build + tests + demos only (~1 min)

  Results go to out\ (git-ignored); the committed results\ folder is not touched.
#>
param(
    [switch]$Quick,
    [switch]$NoSweep,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$build = Join-Path $root 'build'
$out   = Join-Path $root 'out'
$exe   = Join-Path $build 'arqsim.exe'
$tests = Join-Path $build 'arqsim_tests.exe'
$t0    = Get-Date
$step  = 0
$failures = New-Object System.Collections.Generic.List[string]

function Write-Step([string]$title) {
    $script:step++
    $line = '=' * 100
    Write-Host ''
    Write-Host $line -ForegroundColor Cyan
    Write-Host (" [{0}] {1}" -f $script:step, $title) -ForegroundColor Cyan
    Write-Host $line -ForegroundColor Cyan
}

function Invoke-Arq([string]$label, [string[]]$arguments) {
    Write-Host ''
    Write-Host ("  > arqsim " + ($arguments -join ' ')) -ForegroundColor Yellow
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  !! '$label' exited with code $LASTEXITCODE" -ForegroundColor Red
        $script:failures.Add($label)
    }
}

# Make the MSYS2 UCRT64 toolchain visible if it is installed but not on PATH.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -and (Test-Path 'C:\msys64\ucrt64\bin')) {
    $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
}

# ------------------------------------------------------------------ build
Write-Step 'Build (CMake + Ninja, Release)'
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    $gen = if (Get-Command ninja -ErrorAction SilentlyContinue) { @('-G', 'Ninja') } else { @() }
    cmake -S $root -B $build @gen -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
}
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& $exe version

# ------------------------------------------------------------------ tests
Write-Step 'Unit + end-to-end tests'
& $tests
if ($LASTEXITCODE -ne 0) { $failures.Add('tests') } else { Write-Host '  all tests passed' -ForegroundColor Green }

# ------------------------------------------------------------------ demos
New-Item -ItemType Directory -Force -Path $out | Out-Null

Write-Step 'Single runs: Stop-and-Wait vs Go-Back-N vs Selective Repeat (loss 5 %, RTT 50 ms, 5 reps)'
Invoke-Arq 'run sw'  @('run', '--proto', 'sw',  '--loss', '0.05', '--rtt', '50', '--reps', '5')
Invoke-Arq 'run gbn' @('run', '--proto', 'gbn', '--win', '8', '--loss', '0.05', '--rtt', '50', '--reps', '5')
Invoke-Arq 'run sr'  @('run', '--proto', 'sr',  '--win', '8', '--loss', '0.05', '--rtt', '50', '--reps', '5')

Write-Step 'Theory check: measured vs theoretical utilisation (SW, no loss, adaptive RTO)'
Invoke-Arq 'run util' @('run', '--proto', 'sw', '--rtt', '10', '--reps', '6', '--rto', 'adaptive')

Write-Step 'Fixed 2xRTT vs adaptive RFC 6298 RTO (GBN N=16, RTT 20 ms)'
Invoke-Arq 'run rto fixed'    @('run', '--proto', 'gbn', '--win', '16', '--rtt', '20', '--loss', '0.02', '--reps', '3', '--rto', 'fixed')
Invoke-Arq 'run rto adaptive' @('run', '--proto', 'gbn', '--win', '16', '--rtt', '20', '--loss', '0.02', '--reps', '3', '--rto', 'adaptive')

Write-Step 'Burst loss (Gilbert-Elliott) and NAK fast retransmission'
Invoke-Arq 'run burst' @('run', '--proto', 'gbn', '--win', '8', '--loss', '0.05', '--loss-model', 'gilbert', '--rtt', '50', '--reps', '3')
Invoke-Arq 'run nak'   @('run', '--proto', 'gbn', '--win', '8', '--loss', '0.05', '--rtt', '50', '--nak', '--reps', '3')

Write-Step 'Physical layer: modulation over AWGN and adaptive modulation (AMC)'
Invoke-Arq 'run 16qam' @('run', '--proto', 'sr', '--win', '8', '--mod', '16qam', '--snr', '20', '--rto', 'adaptive', '--reps', '3')
Invoke-Arq 'run amc'   @('run', '--proto', 'sr', '--win', '8', '--mod', 'auto', '--snr', '13', '--nak', '--reps', '3')
Invoke-Arq 'amc table' @('amc')
Invoke-Arq 'ber'       @('ber', '--snr', '0:30:2', '--csv', (Join-Path $out 'ber.csv'), '--svg', (Join-Path $out 'ber.svg'))

Write-Step 'Packet-level trace (SR N=4, 10 % loss, 16-QAM @ 20 dB, NAK, 10 kB file)'
$traceLog = Join-Path $out 'trace.log'
Write-Host '  > arqsim run --proto sr --win 4 --loss 0.1 --mod 16qam --snr 20 --nak --file-kb 10 --verbose' -ForegroundColor Yellow
# Write to a file first: cutting a live pipeline short would leave the process running and holding its UDP ports.
& $exe run --proto sr --win 4 --loss 0.1 --mod 16qam --snr 20 --nak --file-kb 10 --verbose *> $traceLog
$trace = Get-Content $traceLog
$trace | Select-Object -First 80 | ForEach-Object { "  $_" }
if ($trace.Count -gt 80) {
    Write-Host ("  ... {0} more lines; full trace in {1}" -f ($trace.Count - 80), $traceLog) -ForegroundColor DarkGray
    $trace | Select-Object -Last 12 | ForEach-Object { "  $_" }
}

Write-Step 'Three separate processes over UDP: channel -> receiver <- sender (proposal architecture)'
$rxFile = Join-Path $out 'received.bin'
$chLog  = Join-Path $out 'channel.log'
$rxLog  = Join-Path $out 'receiver.log'
Write-Host '  channel : arqsim channel --loss 0.1 --rtt 30 --mod qpsk --snr 12   (UDP 50010/50011, background)'
Write-Host '  receiver: arqsim receiver --proto sr --win 8 --nak                 (UDP 50020, background)'
$ch = Start-Process -FilePath $exe -ArgumentList 'channel', '--loss', '0.1', '--rtt', '30', '--mod', 'qpsk', '--snr', '12' `
        -RedirectStandardOutput $chLog -NoNewWindow -PassThru
$rx = Start-Process -FilePath $exe -ArgumentList 'receiver', '--proto', 'sr', '--win', '8', '--nak', '--idle', '30', '--out', "`"$rxFile`"" `
        -RedirectStandardOutput $rxLog -NoNewWindow -PassThru
Start-Sleep -Milliseconds 700
Write-Host '  > arqsim sender --proto sr --win 8 --nak --rto adaptive --rtt 30' -ForegroundColor Yellow
& $exe sender --proto sr --win 8 --nak --rto adaptive --rtt 30
if ($LASTEXITCODE -ne 0) { $failures.Add('3-process sender') }
if (-not $rx.WaitForExit(15000)) { Stop-Process -Id $rx.Id -Force; $failures.Add('3-process receiver') }
Stop-Process -Id $ch.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 200
Write-Host '  --- receiver output ---'; Get-Content $rxLog | ForEach-Object { "  $_" }
Write-Host '  (compare the sender and receiver sha256 lines: identical = bit-exact transfer)' -ForegroundColor DarkGray

# ------------------------------------------------------------------ sweeps
if (-not $NoSweep) {
    Invoke-Arq 'presets' @('presets')
    $jobArgs = if ($Jobs -gt 0) { @('--jobs', "$Jobs") } else { @() }
    if ($Quick) {
        Write-Step 'Experiment sweep: preset quick'
        Invoke-Arq 'sweep quick' (@('sweep', '--preset', 'quick', '--out', (Join-Path $out 'quick')) + $jobArgs)
    } else {
        Write-Step 'Experiment sweep: ALL presets (proposal, protocols, modulation, rto, burst, nak)'
        Invoke-Arq 'sweep all' (@('sweep', '--preset', 'all', '--out', (Join-Path $out 'results')) + $jobArgs)
    }
}

# ------------------------------------------------------------------ summary
Write-Step 'Summary'
$elapsed = (Get-Date) - $t0
Write-Host ("  total time      {0:mm\:ss}" -f $elapsed)
Write-Host  "  output folder   $out"
Get-ChildItem $out -Recurse -File -Include *.csv, *.svg | ForEach-Object {
    Write-Host ("    {0}" -f $_.FullName.Substring($out.Length + 1))
}
if ($failures.Count -eq 0) {
    Write-Host '  status          ALL STEPS SUCCEEDED' -ForegroundColor Green
} else {
    Write-Host ('  status          FAILED: ' + ($failures -join ', ')) -ForegroundColor Red
    exit 1
}
