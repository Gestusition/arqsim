# Design notes

## Process / socket topology

```
                 DATA                         DATA
 ┌──────────┐  ────────►  ┌──────────────────┐  ────────►  ┌────────────┐
 │  Sender  │ :50000      │ Channel emulator │ :50010      │  Receiver  │ :50020
 │ SW/GBN/SR│  ◄────────  │  loss · R · RTT  │  ◄────────  │ SW/GBN/SR  │
 └──────────┘    ACK/NAK  │  PHY (mod+AWGN)  │ :50011      └────────────┘
                          └──────────────────┘   ACK/NAK
```

* `arqsim run` / `arqsim sweep` start the three components as threads inside
  one process. They still communicate **only through UDP datagrams on
  127.0.0.1**, exactly like the standalone `arqsim sender|channel|receiver`
  processes.
* Ports follow the proposal (50000 / 50010 / 50011 / 50020) and can be changed
  with `--sender-port`, `--chan-data-port`, `--chan-ack-port`, `--recv-port`.

## Source layout

| Directory          | Responsibility |
|--------------------|----------------|
| `src/core`         | Packet wire format, CRC-32, SHA-256, clocks, event trace |
| `src/net`          | Portable UDP socket (Winsock2 / BSD) |
| `src/phy`          | BPSK, QPSK, 16-QAM, 64-QAM modem, AWGN, BER theory, AMC |
| `src/channel`      | Loss models (Bernoulli, Gilbert-Elliott) and the channel emulator |
| `src/arq`          | Abstract `ArqSender` + `StopAndWait`, `GoBackN`, `SelectiveRepeat`; receiver; RTO estimators |
| `src/experiment`   | Single-run orchestration, sweeps/presets, statistics, CSV, SVG charts |
| `src/cli`          | `arqsim` command-line interface |
| `tests`            | Unit + end-to-end tests (`ctest`) |

## Packet format

| Offset | Size | Field   | Notes |
|-------:|-----:|---------|-------|
| 0      | 1    | type    | 1 DATA, 2 ACK, 3 NAK, 4 FIN, 5 FINACK |
| 1      | 1    | flags   | reserved |
| 2      | 2    | session | random per run; stale datagrams from earlier runs are ignored |
| 4      | 4    | seq     | DATA: sequence number. ACK: SW/GBN = next expected (cumulative), SR = acknowledged seq |
| 8      | 2    | len     | payload length |
| 10     | 4    | crc32   | over header (crc zeroed) + payload |
| 14     | len  | payload | file bytes |

Sequence numbers are 32-bit and never wrap within an experiment, so the
SR window constraint `N ≤ 2^(k-1)` is satisfied trivially.

## Channel model (per direction, FIFO)

1. **Loss** – Bernoulli(p) or Gilbert–Elliott with stationary loss `p` and
   mean burst length `B`  (`P(B→G)=1/B`, `P(G→B)=p/(B(1−p))`).
2. **Serialisation** – the link is busy for `L/R`; packets queue behind each
   other (this is what makes the utilisation formulas testable on localhost).
3. **Physical layer** – the frame is modulated, Gaussian noise with variance
   `N0/2 = 1/(2·Es/N0)` per dimension is added, the symbols are hard-decided.
   Resulting bit errors make the CRC fail at the receiver.
4. **Propagation** – `RTT/2` (+ optional uniform jitter).

The emulator uses `select()` for coarse waits and spins for the last 16 ms
before a scheduled delivery, because Winsock rounds `select()` timeouts to the
OS tick (≈15.6 ms) which would otherwise add random delay to every packet.

## Physical layer

* Constellations are Gray-coded and normalised to `Es = 1`
  (square M-QAM is scaled by `sqrt(2(M−1)/3)`).
* `--snr` is **Es/N0** (per symbol). Theoretical bit-error probabilities:

| Scheme | BER |
|--------|-----|
| BPSK   | `Q(√(2Es/N0))` |
| QPSK   | `Q(√(Es/N0))` |
| M-QAM  | `(4/log2M)(1−1/√M) Q(√(3Es/((M−1)N0)))` |

* Frame error rate `FER = 1 − (1 − BER)^bits`.
* Link rate `R = symbolRate × log2(M)` – higher order modulation is faster but
  needs more SNR. **AMC** (`--mod auto`) picks the scheme that maximises
  `log2(M) × (1 − FER)`.

## ARQ details

| | Stop-and-Wait | Go-Back-N | Selective Repeat |
|-|-|-|-|
| Window | 1 | N | N (sender and receiver) |
| ACK | cumulative (next expected) | cumulative | individual |
| Timer | one | one, on oldest unACKed (restarted on every new ACK) | one per packet |
| On timeout | resend the packet | resend all outstanding from base | resend that packet only |
| On NAK (`--nak`) | resend | go back from base (once per base) | resend that packet |
| Receiver buffer | none | none (out-of-order discarded) | out-of-order buffered |

### RTO

* `fixed`: `RTO = k × RTT` (k = 2, the proposal's policy).
* `adaptive`: RFC 6298 — initial 1 s, `SRTT`, `RTTVAR`,
  `RTO = SRTT + max(G, 4·RTTVAR)` with `G = 5 ms`, Karn's algorithm and
  exponential back-off.

Observation built into the design: a fixed `2×RTT` RTO ignores queueing
delay `N·L/R`. When `N·L/R ≳ RTT`, SR's per-packet timers (started when the
packet is handed to the link) fire spuriously; GBN is less affected because it
restarts its single timer on every new ACK. The `rto` preset quantifies this.

## Metrics

| Metric | Definition |
|--------|-----------|
| transfer time | first DATA sent → last DATA acknowledged (FIN excluded) |
| throughput | useful file bits / transfer time |
| efficiency | useful payload bytes / all DATA payload bytes sent (incl. retx) |
| utilisation (measured) | `packets·L/R / transfer time` |
| utilisation (theory) | `min(1, N·(L/R)/(RTT + L/R))` (proposal eq. 1 and 2) |
| integrity | completed **and** size equal **and** SHA-256 equal |
| 95 % CI | `t(0.975, n−1) · s / √n` over repetitions |

Repetition *r* of every configuration uses the same seed, so all protocols
face statistically identical channel randomness.
