# arqsim: ARQ performance lab over UDP with a modulation-aware channel

`arqsim` is a C++17 command-line tool for experimental performance analysis of reliable data transfer.
It compares **Stop-and-Wait**, **Go-Back-N** and **Selective Repeat** ARQ running over real UDP sockets.
A channel emulator sits between sender and receiver and imposes:

* packet loss, either independent (Bernoulli) or bursty (Gilbert–Elliott), set separately for DATA and ACK traffic
* a finite link rate `R` (serialisation `L/R` plus FIFO queueing), propagation delay `RTT/2` and optional jitter
* a **physical layer**: every frame is modulated with **BPSK, QPSK, 16-QAM or 64-QAM**, passed through an AWGN channel at a chosen SNR and demodulated. The resulting bit errors are caught by a per-packet CRC-32.
* **Adaptive modulation (AMC)**, which picks the scheme with the highest expected goodput for the current SNR

Everything is written in C++: protocols, sockets, impairments, CRC/SHA-256, statistics, CSV output and the SVG figures.
No scripts are used, as the proposal requires.

> Course project: *Experimental Performance Analysis of Stop-and-Wait and Go-Back-N ARQ over UDP under Controlled Packet Loss and Delay*, extended with additional modulation and protocol modules.

---

## What was added beyond the proposal

| Area | Proposal | Final project |
|------|----------|---------------|
| Protocols | SW, GBN | SW, GBN, **Selective Repeat**, optional **NAK** fast retransmission |
| Channel | Bernoulli loss, delay | + **link rate R** (makes eq. 1/2 testable), **Gilbert–Elliott burst loss**, jitter |
| Physical layer | – | **BPSK / QPSK / 16-QAM / 64-QAM over AWGN**, bit-level simulation, Gray mapping, **AMC** |
| Error detection | file checksum | **CRC-32 per packet** + **SHA-256** per file |
| Timeout | fixed 2×RTT | fixed 2×RTT **and adaptive RFC 6298** (Jacobson/Karels, Karn, back-off) |
| Statistics | 5 runs | mean, std, **95 % Student-t confidence intervals** |
| Validation | – | **measured vs theoretical utilisation**, Monte-Carlo **BER vs closed-form BER**, unit + end-to-end tests |
| Output | CSV | CSV + **SVG figures generated in C++** + terminal tables/ASCII bars |

---

## Build

Requirements: a C++17 compiler (GCC ≥ 9, Clang ≥ 10 or MSVC 2019+) and CMake ≥ 3.16.

```bash
cmake -S . -B build -G Ninja        # or omit -G to use the default generator
cmake --build build
ctest --test-dir build              # unit + end-to-end tests
```

On Windows with MSYS2 UCRT64 you need the packages `mingw-w64-ucrt-x86_64-gcc`, `-cmake` and `-ninja`.
The produced `build/arqsim.exe` is statically linked.

## Run everything with one command (Windows)

```powershell
.\run            # build, tests, every demo, then ALL experiment presets (~8-10 min)
.\run -Quick     # same, but only the short 'quick' sweep (~1 min)
.\run -NoSweep   # build, tests and demos only
```

`run.ps1` (or `run.cmd` from cmd.exe) builds the project, runs the tests, and runs one demo per feature:
SW/GBN/SR, the utilisation check against theory, fixed vs adaptive RTO, burst loss, NAK, modulation and AMC, the BER table, a packet-level trace and the three-process UDP mode with SHA-256 comparison.
It then runs the sweeps and prints every table and ASCII bar chart.

At the end it prints a **Conclusion**, computed from the CSV files of that run (nothing is hard-coded):

* one finding per study: theory check, SR vs GBN vs SW, fixed vs adaptive RTO, NAK, burst loss, modulation cliffs, integrity
* a **verdict** that picks the best combination from the data and says why, plus when the choice does not matter:

```
  BEST OVERALL: SR + adaptive RTO (RFC 6298) + adaptive modulation (AMC)
  because
    SR has the highest throughput at 4 of 5 loss levels, including 3 of 4 lossy ones.
      at 20 % loss it is 1.6x faster than GBN and needs 3.4x fewer retransmissions.
    Adaptive RTO needs no more retransmissions than the fixed one in 8 of 8 cases;
    AMC is within 10 % of the best fixed modulation at 7 of 8 usable SNR points;
    when it does NOT matter:
      without loss GBN and SR are equal, and GBN has the simpler receiver;
      NAKs add at most 5 % to SR, so they are optional there (they matter for GBN).
```

With `-Quick` the verdict is a one-line protocol comparison.
Output goes to `out/`, which is git-ignored, so the committed `results/` are left untouched.

> Parallel sweeps give every job its own block of UDP ports. On Windows, Hyper-V/WinNAT reserves port ranges that change after each reboot (`netsh int ipv4 show excludedportrange protocol=udp`), so the sweep first checks each block and skips any that cannot be bound.

## Quick start

```bash
arqsim help                                   # all commands and options
arqsim run --proto gbn --win 8 --loss 0.05 --rtt 50 --reps 5
arqsim run --proto sr  --win 8 --mod 16qam --snr 20 --rto adaptive
arqsim run --proto sr  --mod auto --snr 13 --nak --verbose     # packet-level trace
arqsim amc                                    # which modulation AMC picks per SNR
arqsim ber --snr 0:30:2 --svg ber.svg         # BER theory vs Monte Carlo
arqsim presets                                # list built-in experiment sets
arqsim sweep --preset quick --out results/quick
```

Example output (`arqsim run --proto sw --rtt 10 --reps 6 --rto adaptive`):

```
  sw N=1 | loss 0.000 (bernoulli) | RTT 10 ms | RTO adaptive | ideal PHY
  file 50000 bytes in 50 packets, link rate 2000 kbit/s

  rep  integrity    time(ms)  thrpt(kb/s) data_tx   retx    tmo   effic.    util   util_th
  1    PASS            730.8        547.3      50      0      0    1.000   0.278     0.289
  ...
  throughput       mean     550.37  std     2.93  95% CI +-    3.08 kbit/s
```

### Three separate processes (the proposal's architecture)

```bash
arqsim channel  --loss 0.1 --rtt 30 --mod qpsk --snr 12             # UDP 50010 / 50011
arqsim receiver --proto sr --win 8 --nak --out received.bin         # UDP 50020
arqsim sender   --proto sr --win 8 --nak --rto adaptive --rtt 30 --input file.bin   # UDP 50000
```

The sender and the receiver both print the SHA-256 of the file, so you can check that the two copies are identical.

## Experiments

| Preset | Question | Runs |
|--------|----------|------|
| `proposal`   | The proposal's matrix: SW vs GBN (N = 4, 8, 16), loss 0–20 %, RTT 10–200 ms, 5 runs each | 400 |
| `protocols`  | SW vs GBN vs SR under increasing loss | 75 |
| `modulation` | Throughput of GBN/SR vs SNR for BPSK, QPSK, 16-QAM, 64-QAM and AMC | 270 |
| `rto`        | Fixed 2×RTT vs adaptive RFC 6298 RTO | 80 |
| `burst`      | Independent vs bursty (Gilbert–Elliott) loss | 120 |
| `nak`        | Does NAK-triggered retransmission help? | 80 |

```bash
arqsim sweep --preset all --out results        # every study, ~8 min (8 parallel jobs)
arqsim sweep --proto sw,gbn,sr --loss 0,0.05,0.1 --rtt 20,100 --win 4,16 --reps 3 --out results/custom
arqsim report results/custom/raw.csv           # re-aggregate
arqsim plot results/proposal/summary.csv --x rtt_ms --y throughput_kbps \
            --series protocol,window --where loss=0.05 --out fig.svg
```

Each sweep writes:

* `raw.csv`: one row per run, including seed, integrity, timings, counters and SHA-256
* `summary.csv`: per configuration, the mean, standard deviation and 95 % CI of every metric
* `*.svg`: the figures for the report

The results committed in [`results/`](results/) and discussed in [`docs/RESULTS.md`](docs/RESULTS.md) were produced with exactly these commands.

## Key findings (1 025 runs, details in [`docs/RESULTS.md`](docs/RESULTS.md))

* **Theory is reproduced.** Measured Stop-and-Wait utilisation is within 2 % of `L/R / (RTT + L/R)`.
  GBN saturates once `N·L/R ≥ RTT + L/R`, and the remaining gap is explained by the finite file.
* **Window vs loss.** At RTT 200 ms, GBN (N=16) is 12.5× faster than SW. Under 20 % loss, though, it sends 4.1 copies of every useful packet.
* **Selective Repeat** retransmits only the lost packets (4.5× fewer than GBN). It is up to 64 % faster than GBN under heavy loss and up to 2.3× faster at the edge of an SNR cliff.
* **A fixed RTO of 2×RTT breaks when queueing (`N·L/R`) exceeds the RTT.** The adaptive RFC 6298 RTO removes 88–96 % of the retransmissions there.
* **SR timers must follow the pipe.** Re-arming them on cumulative ACK progress removed all spurious timeouts on slow links (BPSK: 560 → 939 kbit/s).
* **NAKs** speed up GBN by up to 71 % but barely help SR.
* **Burst loss** cuts SW throughput by up to 83 %, yet *more than doubles* GBN throughput at the same average loss rate.
* **Modulation.** Every scheme has a ~3 dB SNR cliff below which ARQ cannot deliver at all. AMC tracks the best scheme from BPSK (939 kbit/s) up to 64-QAM (4 544 kbit/s).
* **Integrity.** Every completed transfer was bit-exact: 0 corrupted files out of 935 completed runs. The 90 aborted runs are all modulation/SNR pairs below the cliff.

## Documentation

* [`docs/REPORT.pdf`](docs/REPORT.pdf): final report (theory, design, methodology, results, discussion)
* [`docs/DESIGN.md`](docs/DESIGN.md): architecture, packet format, channel/PHY model, protocol rules, metric definitions
* [`docs/RESULTS.md`](docs/RESULTS.md): figures and discussion of the experiments

## Project layout

```
src/core        packet format, CRC-32, SHA-256, clocks, trace
src/net         UDP sockets (Winsock2 / BSD)
src/phy         modulation, AWGN, BER theory, AMC
src/channel     loss models + channel emulator
src/arq         SW / GBN / SR sender, receiver, RTO estimators
src/experiment  runner, sweeps/presets, statistics, CSV, SVG charts
src/cli         arqsim command-line interface
tests           unit + end-to-end tests
```

## References

1. J. F. Kurose and K. W. Ross, *Computer Networking: A Top-Down Approach*, 8th ed., Pearson, 2021.
2. A. S. Tanenbaum, N. Feamster, D. J. Wetherall, *Computer Networks*, 6th ed., Pearson, 2021.
3. S. Lin, D. J. Costello Jr., M. J. Miller, "Automatic-repeat-request error-control schemes," *IEEE Commun. Mag.*, 22(12), 1984.
4. J. Postel, "User Datagram Protocol," RFC 768, 1980.
5. D. Bertsekas and R. Gallager, *Data Networks*, 2nd ed., Prentice Hall, 1992.
6. V. Paxson, M. Allman, J. Chu, M. Sargent, "Computing TCP's Retransmission Timer," RFC 6298, 2011.
7. J. G. Proakis and M. Salehi, *Digital Communications*, 5th ed., McGraw-Hill, 2008.
8. E. N. Gilbert, "Capacity of a burst-noise channel," *Bell Syst. Tech. J.*, 39(5), 1960.
9. P. Karn and C. Partridge, "Improving round-trip time estimates in reliable transport protocols," *ACM SIGCOMM*, 1987.
