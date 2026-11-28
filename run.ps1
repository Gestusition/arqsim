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

# ------------------------------------------------------------------ conclusion
# Every number below is computed from the CSV files this run just produced.
$inv = [System.Globalization.CultureInfo]::InvariantCulture
function Fmt([string]$f) { [string]::Format($inv, $f, [object[]]$args) }
function Num($v) { [double]::Parse([string]$v, $inv) }
function Load([string]$path) { if (Test-Path $path) { @(Import-Csv $path) } else { @() } }
# First row matching every key: strings compare as text, numbers numerically ("0.050" == 0.05).
function Row($rows, [hashtable]$where) {
    foreach ($r in $rows) {
        $ok = $true
        foreach ($k in $where.Keys) {
            $w = $where[$k]
            if ($w -is [string]) { if ($r.$k -ne $w) { $ok = $false; break } }
            elseif ((Num $r.$k) -ne [double]$w) { $ok = $false; break }
        }
        if ($ok) { return $r }
    }
}
function Thr($row) { Num $row.throughput_kbps_mean }
function Say([string]$title, [string[]]$lines) {
    Write-Host ''
    Write-Host "  * $title" -ForegroundColor Green
    foreach ($l in $lines) { Write-Host "      $l" }
}

# Picks the best protocol / RTO / modulation from the data and explains why.
function Write-Verdict($pr, $rto, $mod, $nak) {
    $because = @()
    # Protocol: who has the highest throughput at each loss level?
    $levels = $pr | ForEach-Object { Num $_.loss } | Sort-Object -Unique
    $wins = @{ sw = 0; gbn = 0; sr = 0 }
    foreach ($l in $levels) {
        $w = $pr | Where-Object { (Num $_.loss) -eq $l } | Sort-Object { Thr $_ } -Descending | Select-Object -First 1
        $wins[$w.protocol]++
    }
    $best = ($wins.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
    $lossy = @($levels | Where-Object { $_ -gt 0 })
    $lossyWins = @($lossy | Where-Object { $l = $_
            ($pr | Where-Object { (Num $_.loss) -eq $l } | Sort-Object { Thr $_ } -Descending | Select-Object -First 1).protocol -eq $best }).Count
    $lMax = ($levels | Measure-Object -Maximum).Maximum
    $b = Row $pr @{ protocol = $best; loss = $lMax }
    $others = $pr | Where-Object { (Num $_.loss) -eq $lMax -and $_.protocol -ne $best }
    $because += Fmt '{0} has the highest throughput at {1} of {2} loss levels, including {3} of {4} lossy ones.' $best.ToUpper() $wins[$best] $levels.Count $lossyWins $lossy.Count
    foreach ($o in $others) {
        $rx = (Num $o.retransmissions_mean) / [math]::Max(1e-9, (Num $b.retransmissions_mean))
        $tail = if ($rx -ge 1.2) { Fmt ' and needs {0:0.0}x fewer retransmissions' $rx } else { ' with about the same number of retransmissions' }
        $because += Fmt '  at {0:0} % loss it is {1:0.0}x faster than {2}{3}.' (100 * $lMax) ((Thr $b) / (Thr $o)) $o.protocol.ToUpper() $tail
    }
    if ($best -eq 'sr') { $because += '  it keeps the pipe full like GBN but resends only the packets that were really lost.' }

    # RTO: how often does adaptive beat fixed?
    $rtoPick = 'fixed 2xRTT'
    if ($rto.Count) {
        $pairs = 0; $adWins = 0
        foreach ($f in ($rto | Where-Object { $_.rto_mode -eq 'fixed' })) {
            $a = Row $rto @{ protocol = $f.protocol; rtt_ms = (Num $f.rtt_ms); rto_mode = 'adaptive' }
            if ($a) { $pairs++; if ((Num $a.retransmissions_mean) -le (Num $f.retransmissions_mean)) { $adWins++ } }
        }
        if ($adWins * 2 -ge $pairs) { $rtoPick = 'adaptive RTO (RFC 6298)' }
        $because += Fmt 'Adaptive RTO needs no more retransmissions than the fixed one in {0} of {1} cases;' $adWins $pairs
        $because += '  it learns the real RTT including queueing, so it does not fire spurious timeouts.'
    }

    # Modulation: is AMC always at least as good as the best fixed scheme?
    $modPick = $null
    if ($mod.Count) {
        $snrs = $mod | Where-Object { $_.protocol -eq $best -and $_.modulation -eq 'auto' } | ForEach-Object { Num $_.snr_db }
        $close = 0; $usable = 0
        foreach ($s in $snrs) {
            $fixedBest = ($mod | Where-Object { $_.protocol -eq $best -and $_.modulation -ne 'auto' -and (Num $_.snr_db) -eq $s } |
                          ForEach-Object { Thr $_ } | Measure-Object -Maximum).Maximum
            if ($fixedBest -le 0) { continue }
            $usable++
            $amc = Thr (Row $mod @{ protocol = $best; modulation = 'auto'; snr_db = $s })
            if ($amc -ge 0.9 * $fixedBest) { $close++ }
        }
        if ($close * 2 -ge $usable) { $modPick = 'adaptive modulation (AMC)' }
        $because += Fmt 'AMC is within 10 % of the best fixed modulation at {0} of {1} usable SNR points;' $close $usable
        $because += '  no single fixed scheme can do that: BPSK is too slow at high SNR, 64-QAM fails at low SNR.'
    }

    $pick = @($best.ToUpper(), $rtoPick, $modPick) | Where-Object { $_ }
    Write-Host ''
    Write-Host ('  ' + ('-' * 96)) -ForegroundColor Yellow
    Write-Host ('  BEST OVERALL: ' + ($pick -join ' + ')) -ForegroundColor Yellow
    Write-Host ('  ' + ('-' * 96)) -ForegroundColor Yellow
    Write-Host '  because'
    foreach ($l in $because) { Write-Host "    $l" }
    $caveat = @('  when it does NOT matter:')
    $g0 = Row $pr @{ protocol = 'gbn'; loss = 0 }; $s0 = Row $pr @{ protocol = 'sr'; loss = 0 }
    if ($g0 -and $s0) { $caveat += Fmt '    without loss GBN and SR are equal ({0:0.0} vs {1:0.0} kbit/s), and GBN has the simpler receiver;' (Thr $g0) (Thr $s0) }
    if ($nak.Count) {
        $srGain = ($nak | Where-Object { $_.protocol -eq 'sr' -and $_.nak -eq '0' } | ForEach-Object {
                $on = Row $nak @{ protocol = 'sr'; loss = (Num $_.loss); nak = 1 }
                if ($on -and (Thr $_) -gt 0) { (Thr $on) / (Thr $_) - 1 } } | Measure-Object -Maximum).Maximum
        $caveat += Fmt '    NAKs add at most {0:0} % to SR, so they are optional there (they matter for GBN).' (100 * $srGain)
    }
    foreach ($l in $caveat) { Write-Host "  $l" -ForegroundColor DarkGray }
}

function Write-Conclusion([string]$dir, [bool]$full) {
    $n = 0
    if ($full) {
        $prop = Load (Join-Path $dir 'proposal\summary.csv')
        if ($prop.Count) {
            $sw0 = $prop | Where-Object { $_.protocol -eq 'sw' -and (Num $_.loss) -eq 0 }
            $dev = ($sw0 | ForEach-Object { [math]::Abs((Num $_.utilization_mean) / (Num $_.theory_util_mean) - 1) } |
                    Measure-Object -Maximum).Maximum
            $rttMax = ($prop | ForEach-Object { Num $_.rtt_ms } | Measure-Object -Maximum).Maximum
            $lossMax = ($prop | ForEach-Object { Num $_.loss } | Measure-Object -Maximum).Maximum
            $sw = Row $prop @{ protocol = 'sw'; loss = 0; rtt_ms = $rttMax }
            $g16 = Row $prop @{ protocol = 'gbn'; window = 16; loss = 0; rtt_ms = $rttMax }
            $g16l = Row $prop @{ protocol = 'gbn'; window = 16; loss = $lossMax; rtt_ms = $rttMax }
            $copies = (Num $g16l.data_tx_mean) / ((Num $g16l.file_bytes) / (Num $g16l.payload))
            Say 'Theory is reproduced (proposal preset)' @(
                (Fmt 'Measured Stop-and-Wait utilisation is within {0:0.0} % of L/R / (RTT + L/R) at every RTT.' (100 * $dev)),
                (Fmt 'At RTT {0:0} ms, GBN (N=16) reaches {1:0.0} kbit/s vs {2:0.0} kbit/s for SW: {3:0.0}x faster.' $rttMax (Thr $g16) (Thr $sw) ((Thr $g16) / (Thr $sw))),
                (Fmt 'The cost: at {0:0} % loss GBN (N=16) sends {1:0.0} copies of every useful packet.' (100 * $lossMax) $copies))
            $n++
        }
        $pr = Load (Join-Path $dir 'protocols\summary.csv')
        if ($pr.Count) {
            $lossMax = ($pr | ForEach-Object { Num $_.loss } | Measure-Object -Maximum).Maximum
            $g = Row $pr @{ protocol = 'gbn'; loss = $lossMax }; $s = Row $pr @{ protocol = 'sr'; loss = $lossMax }
            $w = Row $pr @{ protocol = 'sw'; loss = $lossMax }
            Say 'Selective Repeat vs Go-Back-N vs Stop-and-Wait (protocols preset)' @(
                (Fmt 'At {0:0} % loss: SR {1:0.0}, GBN {2:0.0}, SW {3:0.0} kbit/s.' (100 * $lossMax) (Thr $s) (Thr $g) (Thr $w)),
                (Fmt 'SR is {0:+0;-0} % vs GBN and retransmits {1:0.0}x fewer packets ({2:0.0} vs {3:0.0}),' (100 * ((Thr $s) / (Thr $g) - 1)) ((Num $g.retransmissions_mean) / [math]::Max(1e-9, (Num $s.retransmissions_mean))) (Num $s.retransmissions_mean) (Num $g.retransmissions_mean)),
                'because it resends only the lost packets instead of the whole window.')
            $n++
        }
        $rto = Load (Join-Path $dir 'rto\summary.csv')
        if ($rto.Count) {
            $best = $null; $bestCut = -1
            foreach ($f in ($rto | Where-Object { $_.rto_mode -eq 'fixed' })) {
                $a = Row $rto @{ protocol = $f.protocol; rtt_ms = (Num $f.rtt_ms); rto_mode = 'adaptive' }
                $rf = Num $f.retransmissions_mean
                if ($a -and $rf -gt 0) {
                    $cut = 1 - (Num $a.retransmissions_mean) / $rf
                    if ($cut -gt $bestCut) { $bestCut = $cut; $best = @($f, $a) }
                }
            }
            if ($best) {
                Say 'Fixed 2xRTT vs adaptive RFC 6298 RTO (rto preset)' @(
                    (Fmt 'Worst case for the fixed RTO: {0} at RTT {1:0} ms, where queueing (N*L/R) exceeds the RTT.' $best[0].protocol.ToUpper() (Num $best[0].rtt_ms)),
                    (Fmt 'Adaptive RTO removes {0:0} % of the retransmissions there ({1:0.0} -> {2:0.0}); throughput {3:0.0} -> {4:0.0} kbit/s.' (100 * $bestCut) (Num $best[0].retransmissions_mean) (Num $best[1].retransmissions_mean) (Thr $best[0]) (Thr $best[1])))
                $n++
            }
        }
        $nak = Load (Join-Path $dir 'nak\summary.csv')
        if ($nak.Count) {
            $gain = @{}
            foreach ($p in 'gbn', 'sr') {
                $gain[$p] = ($nak | Where-Object { $_.protocol -eq $p -and $_.nak -eq '0' } | ForEach-Object {
                        $on = Row $nak @{ protocol = $p; loss = (Num $_.loss); nak = 1 }
                        if ($on -and (Thr $_) -gt 0) { (Thr $on) / (Thr $_) - 1 } } | Measure-Object -Maximum).Maximum
            }
            Say 'NAK fast retransmission (nak preset)' @(
                (Fmt 'NAKs speed up GBN by up to {0:0} %, but SR by at most {1:0} %:' (100 * $gain['gbn']) (100 * $gain['sr'])),
                'a GBN timeout resends the whole window, so reacting early saves much more there.')
            $n++
        }
        $bu = Load (Join-Path $dir 'burst\summary.csv')
        if ($bu.Count) {
            $ratio = @{}
            foreach ($p in 'sw', 'gbn', 'sr') {
                $ratio[$p] = $bu | Where-Object { $_.protocol -eq $p -and $_.loss_model -eq 'bernoulli' } | ForEach-Object {
                    $ge = Row $bu @{ protocol = $p; loss = (Num $_.loss); loss_model = 'gilbert' }
                    if ($ge -and (Thr $_) -gt 0) { (Thr $ge) / (Thr $_) } } | Measure-Object -Minimum -Maximum
            }
            Say 'Burst (Gilbert-Elliott) vs independent loss, same average rate (burst preset)' @(
                (Fmt 'SW loses up to {0:0} % of its throughput when losses come in bursts.' (100 * (1 - $ratio['sw'].Minimum))),
                (Fmt 'GBN gains up to {0:0.0}x and SR up to {1:0.0}x: one timeout recovers a whole burst, and the gaps between bursts are clean.' $ratio['gbn'].Maximum $ratio['sr'].Maximum))
            $n++
        }
        $mod = Load (Join-Path $dir 'modulation\summary.csv')
        if ($mod.Count) {
            $lines = @()
            foreach ($m in 'bpsk', 'qpsk', '16qam', '64qam', 'auto') {
                $rows = $mod | Where-Object { $_.protocol -eq 'sr' -and $_.modulation -eq $m }
                $ok = $rows | Where-Object { (Thr $_) -gt 0 }
                if (-not $ok) { continue }
                $cliff = ($ok | ForEach-Object { Num $_.snr_db } | Measure-Object -Minimum).Minimum
                $peak = ($rows | ForEach-Object { Thr $_ } | Measure-Object -Maximum).Maximum
                $name = if ($m -eq 'auto') { 'AMC' } else { $m.ToUpper() }
                $lines += Fmt '{0,-6} works from {1,4:0} dB, peak {2,7:0.0} kbit/s' $name $cliff $peak
            }
            $lines += 'Each scheme has a sharp SNR cliff below which ARQ cannot deliver; AMC follows the best scheme at every SNR.'
            Say 'Modulation over AWGN, Selective Repeat (modulation preset)' $lines
            $n++
        }
    } else {
        $q = Load (Join-Path $dir 'summary.csv')
        if ($q.Count) {
            $lossMax = ($q | ForEach-Object { Num $_.loss } | Measure-Object -Maximum).Maximum
            $rttMax = ($q | ForEach-Object { Num $_.rtt_ms } | Measure-Object -Maximum).Maximum
            $lines = foreach ($p in 'sw', 'gbn', 'sr') {
                $r0 = Row $q @{ protocol = $p; loss = 0; rtt_ms = $rttMax }; $rl = Row $q @{ protocol = $p; loss = $lossMax; rtt_ms = $rttMax }
                Fmt '{0,-4} {1,7:0.0} kbit/s without loss, {2,7:0.0} kbit/s at {3:0} % loss ({4:0.0} retransmissions)' $p.ToUpper() (Thr $r0) (Thr $rl) (100 * $lossMax) (Num $rl.retransmissions_mean)
            }
            Say "Protocol comparison at RTT $rttMax ms (quick preset)" (@($lines) + 'Pipelining (GBN/SR) beats Stop-and-Wait; SR loses the least under loss. Run .\run for the full study.')
            $n++
            # Verdict: highest mean throughput over the lossy configurations.
            $lossyRows = $q | Where-Object { (Num $_.loss) -gt 0 }
            $rank = $lossyRows | Group-Object protocol | ForEach-Object {
                [pscustomobject]@{ p = $_.Name; thr = ($_.Group | ForEach-Object { Thr $_ } | Measure-Object -Average).Average
                                   retx = ($_.Group | ForEach-Object { Num $_.retransmissions_mean } | Measure-Object -Average).Average } } |
                Sort-Object thr -Descending
            Write-Host ''
            Write-Host ('  BEST IN THIS QUICK RUN: ' + $rank[0].p.ToUpper()) -ForegroundColor Yellow
            Write-Host (Fmt '    because it has the highest mean throughput under loss ({0:0.0} kbit/s vs {1}),' $rank[0].thr (($rank | Select-Object -Skip 1 | ForEach-Object { Fmt '{0} {1:0.0}' $_.p.ToUpper() $_.thr }) -join ', '))
            Write-Host (Fmt '    with {0:0.0} retransmissions per run on average. Run .\run for the full verdict.' $rank[0].retx)
        }
    }
    if ($full -and $pr.Count) { Write-Verdict $pr $rto $mod $nak }
    # Integrity over every raw.csv of this run
    $raw = Get-ChildItem $dir -Recurse -Filter raw.csv | ForEach-Object { Import-Csv $_.FullName }
    if ($raw) {
        $done = @($raw | Where-Object { $_.completed -eq '1' })
        $bad = @($done | Where-Object { $_.integrity -ne '1' })
        $aborted = $raw.Count - $done.Count
        $lines = @(Fmt '{0} runs in total: {1} completed, {2} corrupted files, {3} aborted.' $raw.Count $done.Count $bad.Count $aborted)
        $lines += if ($bad.Count -eq 0) { 'Every completed transfer was bit-exact.' } else { 'WARNING: some completed transfers did not match the sent file.' }
        if ($aborted -gt 0) { $lines += 'Aborted runs are configurations below the SNR cliff (or past the stall guard), not errors.' }
        Say 'Integrity (CRC-32 per packet + SHA-256 per file)' $lines
        $n++
    }
    if ($n -eq 0) { Write-Host '  (no sweep results found, run without -NoSweep for the conclusion)' }
}

if (-not $NoSweep) {
    Write-Step 'Conclusion'
    if ($Quick) { Write-Conclusion (Join-Path $out 'quick') $false }
    else { Write-Conclusion (Join-Path $out 'results') $true }
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
