# Stream mode: delay buffer in memory vs on disk (benchmark)

Measured 2026-11-11 on Windows 11, plugin branch `feat/1.0.0-packet-delay` @ afaa14c (v1.0.0), with
`tests/relay_harness.cpp` from this branch (the same relay code plus CPU, I/O and peak-memory
columns). Scripts and raw summaries: `tools/buffer-bench/`.

## Resumen (español)

- **Disco (spill cifrado):** el proceso usa **+11 a +16 MB** de RAM sobre el arranque, con
  cualquier retardo (10, 120 o 600 s) y a 6 u 8 Mbps, y guarda siempre **606 s** de historial
  (440 MB en disco a 6 Mbps, 570-582 MB a 8 Mbps). Escribe al disco el bitrate del stream:
  **44 MiB/min a 6 Mbps (2,8 GB/hora)** y **57-58 MiB/min a 8 Mbps (3,6-3,7 GB/hora)**.
- **Solo RAM:** la memoria crece con lo que se guarda: a 6 Mbps **+50 MB** (retardo 10 s, 60 s
  guardados), **+101 MB** (120 s) y **+451 MB** (600 s); a 8 Mbps **+69 / +131 / +585 MB**.
- **Subida en vivo de 10 a 300 s con el historial lleno:** en disco fue **instantánea** (300 s al
  siguiente segundo, sin cortar la salida). En solo RAM la salida se **paró 233 s** (60 s guardados)
  o **284 s** (10 s guardados) hasta acumular el retardo.
- **CPU:** igual en los dos modos (14-20 s de CPU por minuto mientras envía; la diferencia medida
  disco − RAM es de −0,1 a +1,0 s de CPU por minuto, como mucho un 1,7 % de un núcleo). **0 fallbacks del spill** en 12 ejecuciones en disco.
  Esos 14-20 s salen de usar ffmpeg como OBS y como plataforma (chunks RTMP de 128 bytes en los dos
  sockets del relay); con un publicador como OBS y una plataforma que anuncia 4096 bytes son 1-1,5 s
  por minuto: `docs/STREAM_MODE-es.md`, "CPU".
- **Salida:** en las 22 ejecuciones cada paquete de salida es idéntico byte a byte a uno de la fuente
  (sha256), el DTS de vídeo es estrictamente creciente y la decodificación no da ningún error.
- **Desgaste del SSD (calculado a partir de lo medido):** 100 horas de stream escriben 0,28-0,31 TB a
  6 Mbps y 0,36-0,41 TB a 8 Mbps: **0,14-0,19 %** de un SSD de 220 TBW (Crucial P3 1 TB) y
  **0,03 %** de uno de 1.200 TBW (WD_BLACK SN7100 2 TB).
- No hay HDD ni USB en esta máquina: solo se midieron dos SSD NVMe.

## Machine

| | |
|---|---|
| CPU | AMD Ryzen 7 7700X (8 cores / 16 threads) |
| RAM | 31 GB usable |
| OS | Windows 11 Pro 10.0.26300 |

Drives (`Get-PhysicalDisk`, `Get-Partition`, `Get-Volume`):

| Disk | Model | MediaType | BusType | Size | Volume | Free at test time |
|---|---|---|---|---|---|---|
| 0 | Crucial P3 1 TB (CT1000P3SSD8; QLC, no DRAM) | SSD | NVMe | 932 GB | C: (system, NTFS) | 295 GB |
| 1 | WD_BLACK SN7100 2 TB | SSD | NVMe | 1863 GB | E: (NTFS) | 842 GB |

There is no HDD and no USB drive on this machine, so only the two NVMe SSDs were measured. **Nothing
here says anything about HDDs or USB sticks.**

## What was run

- **Sources** (pre-encoded once, then published with stream copy so every run gets the same
  packets): 800 s, 1920x1080 30 fps x264 CBR (`nal-hrd=cbr`, filler included), `-g 60 -bf 2`,
  preset veryfast, `testsrc2` + temporal noise; AAC 160 kbps 48 kHz stereo. Container bitrate
  **6.17 Mbps** ("6 Mbps": 6000 kbps video) and **8.17 Mbps** ("8 Mbps": 8000 kbps video).
- **One run** (`tools/buffer-bench/run.sh`): an ffmpeg RTMP listener as the platform (writes FLV),
  `relay_harness` (the relay in its own process) with `TMP`/`TEMP` pointed at a folder on the drive
  under test, and `ffmpeg -re -c copy` as OBS. The delay is set at t=0 (the moment OBS's stream
  reaches the relay); the stream runs 800 s; the harness keeps running until the destination has
  sent everything (the delay drains after OBS stops).
- **Modes**
  - *Disk*: spill on (`spill MB` 0), the spill file in `C:\dd-bench-tmp` or `E:\dd-bench-tmp`.
    `keep` 60 (with the spill the relay keeps 600 s regardless).
  - *RAM only, keep 60 s*: spill off (`spill MB` -1), keep 60 (what presets/rules topping out at
    60 s give).
  - *RAM only, keep = delay*: spill off, keep equal to the delay (10 or 120; at 600 s it is the
    same as keep 60).
- **Matrix**: {6, 8 Mbps} x {10, 120, 600 s} x {disk on C:, disk on E:, RAM keep 60, RAM keep =
  delay} = 22 runs.
- **Live raise**: every 6 Mbps / 10 s run raised the delay to **300 s at t=660**, after the
  history had filled in every mode (disk mode holds 606 s from t≈606).
- **Batches**: batch 1 ran the 6 disk-on-C: runs and the 10 RAM-only runs at the same time (16
  relays; the platform recordings went to E:, so nothing but the spill touched C: from the test).
  Batch 2 ran the 6 disk-on-E: runs alone, recordings to C:, so E: saw only the spill. CPU per run
  was the same in both batches (6 Mbps / 10 s: 15.5 s/min with 16 relays running, 15.6 with 6).

### Measured, per run

From the harness's status line, once per second:

- `priv` / `ws`: the process's private bytes and working set; `ppriv` / `pws`: Windows' own peaks
  for the process lifetime (`PeakPagefileUsage`, `PeakWorkingSetSize`). **Start** is the first
  status line (t=0: the relay running, OBS's stream just arrived; it is the harness with FFmpeg
  loaded, about 21 MB). **Steady** is the median over t=620-655 s (history full in every mode,
  before the raise). The relay's own cost is steady minus start.
- `cpu`: user + kernel CPU seconds of the process (`GetProcessTimes`). Reported per minute of
  stream between t=55 and t=655.
- `iow` / `ior`: bytes the process wrote / read through `WriteFile` / `ReadFile`
  (`GetProcessIoCounters`): the spill file plus the harness's own stdout (about 0.02 MiB/min, the
  RAM-only rows). Sockets do not count here.
- `hist`, `hram`, `disk`, `spill`: the relay's own history length and bytes, the RAM it holds for
  them, the bytes in the spill file and whether the spill is active. A spill fallback would show as
  `spill=0` while streaming, plus a log line.
- `behind` (content age the destination is sending, vs the delay), `rs` (resyncs: the destination
  fell behind and jumped back to the delay), `bytes` sent.
- System-wide every 10 s (`tools/buffer-bench/sampler.ps1`): physical disk bytes written/read per
  drive (`Win32_PerfRawData_PerfDisk_PhysicalDisk`), modified page list and standby cache.

Output correctness (`tools/buffer-bench/verify.py`): sha256 of every packet of the recording and of
the source (`ffmpeg -c copy -f framehash -hash sha256`); every output packet must equal a source
packet (same stream, size and hash); video DTS strictly increasing, audio DTS non-decreasing;
splices listed (consecutive output video packets that are not consecutive source packets, with the
jump in source time); full decode with `ffmpeg -v error -f null -` must print nothing.

## Results

"6 Mbps" / "8 Mbps" are the 6.17 / 8.17 Mbps streams above. MB = 2^20 bytes in the tables
(the harness prints MiB with an "MB" label).

### Memory

| Mode | Stream | Delay | History kept | of it in RAM | on disk | Private bytes: start / steady / peak (MB) | Working set: steady / peak (MB) | Relay cost, steady |
|---|---|---|---|---|---|---|---|---|
| Disk, C: Crucial P3 | 6 Mbps | 10 s | 606 s, 445 MB | 11 MB | 440 MB | 21.0 / 32.4 / 36.9 (34.0 before the raise, sampled) | 34.6 / 38.8 | +11 MB |
| Disk, C: Crucial P3 | 6 Mbps | 120 s | 606 s, 445 MB | 13 MB | 440 MB | 21.1 / 35.3 / 37.1 | 37.2 / 39.1 | +14 MB |
| Disk, C: Crucial P3 | 6 Mbps | 600 s | 606 s, 445 MB | 13 MB | 440 MB | 21.1 / 34.3 / 36.5 | 36.3 / 38.1 | +13 MB |
| Disk, C: Crucial P3 | 8 Mbps | 10 s | 606 s, 578 MB | 14 MB | 570 MB | 21.1 / 34.7 / 39.2 | 37.1 / 40.5 | +14 MB |
| Disk, C: Crucial P3 | 8 Mbps | 120 s | 606 s, 584 MB | 16 MB | 576 MB | 21.3 / 37.5 / 42.1 | 39.1 / 43.1 | +16 MB |
| Disk, C: Crucial P3 | 8 Mbps | 600 s | 606 s, 578 MB | 16 MB | 571 MB | 21.4 / 36.2 / 44.4 | 38.3 / 44.8 | +15 MB |
| Disk, E: WD_BLACK SN7100 | 6 Mbps | 10 s | 606 s, 445 MB | 11 MB | 440 MB | 21.2 / 32.8 / 35.7 (33.6 before the raise, sampled) | 34.7 / 37.7 | +12 MB |
| Disk, E: WD_BLACK SN7100 | 6 Mbps | 120 s | 606 s, 445 MB | 13 MB | 440 MB | 21.2 / 34.6 / 36.7 | 36.5 / 38.4 | +13 MB |
| Disk, E: WD_BLACK SN7100 | 6 Mbps | 600 s | 606 s, 445 MB | 12 MB | 440 MB | 21.1 / 33.9 / 36.4 | 35.7 / 37.5 | +13 MB |
| Disk, E: WD_BLACK SN7100 | 8 Mbps | 10 s | 606 s, 589 MB | 13 MB | 582 MB | 21.2 / 34.2 / 36.4 | 36.1 / 38.1 | +13 MB |
| Disk, E: WD_BLACK SN7100 | 8 Mbps | 120 s | 606 s, 589 MB | 15 MB | 582 MB | 21.2 / 36.2 / 38.8 | 38.0 / 40.4 | +15 MB |
| Disk, E: WD_BLACK SN7100 | 8 Mbps | 600 s | 606 s, 589 MB | 15 MB | 582 MB | 21.2 / 36.6 / 38.9 | 38.0 / 40.3 | +15 MB |
| RAM only, keep 60 s | 6 Mbps | 10 s | 66 s, 49 MB | 50 MB | 0 | 20.9 / 71.0 / 173.6 (75.5 before the raise, sampled) | 73.2 / 174.6 | +50 MB |
| RAM only, keep 60 s | 6 Mbps | 120 s | 126 s, 97 MB | 100 MB | 0 | 20.8 / 121.6 / 123.4 | 122.5 / 124.1 | +101 MB |
| RAM only, keep 60 s | 6 Mbps | 600 s | 606 s, 445 MB | 449 MB | 0 | 21.8 / 473.1 / 475.4 | 473.9 / 476.6 | +451 MB |
| RAM only, keep 60 s | 8 Mbps | 10 s | 66 s, 68 MB | 69 MB | 0 | 20.6 / 89.8 / 98.3 | 92.3 / 100.4 | +69 MB |
| RAM only, keep 60 s | 8 Mbps | 120 s | 126 s, 129 MB | 131 MB | 0 | 20.6 / 151.6 / 154.9 | 153.2 / 156.0 | +131 MB |
| RAM only, keep 60 s | 8 Mbps | 600 s | 606 s, 578 MB | 581 MB | 0 | 20.6 / 605.6 / 619.5 | 605.8 / 619.0 | +585 MB |
| RAM only, keep = delay | 6 Mbps | 10 s | 16 s, 12 MB | 13 MB | 0 | 21.2 / 35.6 / 138.7 (37.1 before the raise, sampled) | 36.5 / 138.3 | +14 MB |
| RAM only, keep = delay | 6 Mbps | 120 s | 126 s, 92 MB | 95 MB | 0 | 20.5 / 116.8 / 123.3 | 118.6 / 124.3 | +96 MB |
| RAM only, keep = delay | 8 Mbps | 10 s | 16 s, 16 MB | 17 MB | 0 | 20.8 / 37.2 / 44.6 | 39.2 / 45.9 | +16 MB |
| RAM only, keep = delay | 8 Mbps | 120 s | 126 s, 129 MB | 131 MB | 0 | 20.6 / 151.8 / 155.3 | 153.3 / 156.2 | +131 MB |
| RAM only, keep = delay | 6 / 8 Mbps | 600 s | same runs as RAM only, keep 60 s, 600 s (keep 60 < delay) | | | | | |

The peaks of the three raise runs (6 Mbps / 10 s) are after the raise to 300 s: in RAM only the
history then grows to 300 s (204 s and 154 s of it by the end of the stream, 150 MB and 113 MB);
in disk mode it stays at 606 s with 4-13 MB in RAM.

Not in these numbers: with buffered writes the spill's pages also pass through Windows' file cache
(the standby list), which belongs to the system, is not charged to the process and is given back
under memory pressure. The modified (not yet written) page list stayed between 112 and 411 MB
system-wide during both batches (it includes everything else running), with no build-up.

### CPU, disk I/O, relay lag

CPU and MiB/min are over t=55-655 s. For the 600 s delays that window is mostly before the
destination starts sending (it starts at t=600), so those rows show the cost of ingest + history
only; the read rate there is the first 55 s of sending.

| Mode | Stream | Delay | CPU s / min | Written MiB/min | Read MiB/min | Written total MiB | Read total MiB | Spill fallbacks | Max lag (steady) | Resyncs |
|---|---|---|---|---|---|---|---|---|---|---|
| Disk, C: | 6 Mbps | 10 s | 15.5 | 44.1 | 0.0 | 588 | 330 (after the raise) | 0 | 0.1 s | 0 |
| Disk, C: | 6 Mbps | 120 s | 14.4 | 44.1 | 39.3 | 588 | 584 | 0 | 0.0 s | 0 |
| Disk, C: | 6 Mbps | 600 s | 4.6 | 44.1 | 4.0 | 588 | 584 | 0 | 0.1 s | 0 |
| Disk, C: | 8 Mbps | 10 s | 19.5 | 57.2 | 0.0 | 778 | 0 | 0 | 0.2 s | 0 |
| Disk, C: | 8 Mbps | 120 s | 18.0 | 57.8 | 51.1 | 778 | 774 | 0 | 0.4 s | 1 |
| Disk, C: | 8 Mbps | 600 s | 6.0 | 57.3 | 5.4 | 778 | 774 | 0 | 0.3 s | 0 |
| Disk, E: | 6 Mbps | 10 s | 15.6 | 44.1 | 0.0 | 588 | 324 (after the raise) | 0 | 0.0 s | 0 |
| Disk, E: | 6 Mbps | 120 s | 14.5 | 44.1 | 39.3 | 588 | 584 | 0 | 0.0 s | 0 |
| Disk, E: | 6 Mbps | 600 s | 5.3 | 44.1 | 4.1 | 588 | 584 | 0 | 0.0 s | 0 |
| Disk, E: | 8 Mbps | 10 s | 20.0 | 58.4 | 0.0 | 778 | 0 | 0 | 0.1 s | 0 |
| Disk, E: | 8 Mbps | 120 s | 18.6 | 58.4 | 52.1 | 778 | 774 | 0 | 0.1 s | 0 |
| Disk, E: | 8 Mbps | 600 s | 6.8 | 58.4 | 5.4 | 778 | 774 | 0 | 0.1 s | 0 |
| RAM, keep 60 | 6 Mbps | 10 s | 15.2 | 0.0 | 0.0 | 0 | 0 | n/a | 0.1 s | 0 |
| RAM, keep 60 | 6 Mbps | 120 s | 14.1 | 0.0 | 0.0 | 0 | 0 | n/a | 0.2 s | 0 |
| RAM, keep 60 | 6 Mbps | 600 s | 4.6 | 0.0 | 0.0 | 0 | 0 | n/a | 0.4 s | 0 |
| RAM, keep 60 | 8 Mbps | 10 s | 19.6 | 0.0 | 0.0 | 0 | 0 | n/a | 0.4 s | 0 |
| RAM, keep 60 | 8 Mbps | 120 s | 17.7 | 0.0 | 0.0 | 0 | 0 | n/a | 1.2 s | 0 |
| RAM, keep 60 | 8 Mbps | 600 s | 5.8 | 0.0 | 0.0 | 0 | 0 | n/a | 1.0 s | 0 |
| RAM, keep = delay | 6 Mbps | 10 s | 15.4 | 0.0 | 0.0 | 0 | 0 | n/a | 0.5 s | 0 |
| RAM, keep = delay | 6 Mbps | 120 s | 14.0 | 0.0 | 0.0 | 0 | 0 | n/a | 0.3 s | 0 |
| RAM, keep = delay | 8 Mbps | 10 s | 19.5 | 0.0 | 0.0 | 0 | 0 | n/a | 0.6 s | 0 |
| RAM, keep = delay | 8 Mbps | 120 s | 17.8 | 0.0 | 0.0 | 0 | 0 | n/a | 1.0 s | 1 |

Max lag = the largest |`behind` − delay| at t=620-655 while the destination was live.

What this shows:

- **Writes = the stream's bitrate, whatever the delay.** The spill writes every packet once:
  44.1 MiB/min at 6.17 Mbps (6.17 Mbit/s x 60 s = 44.1 MiB), 57.2-58.4 MiB/min at 8.17 Mbps. With
  the delay at 10 s it writes the same as at 600 s.
- **Reads depend on the delay.** At 10 s the destination sends from RAM (the newest seconds stay
  there): 0 bytes read back. At 120 s and 600 s everything written is read back once (584 / 774
  MiB of 588 / 778). After the raise to 300 s the 10 s runs read back 324-330 MiB.
- **The spill costs little CPU.** Same delay and bitrate, disk minus RAM: -0.1 to +0.9 s of CPU
  per minute while sending, 0 to +1.0 s/min in the 600 s rows (mostly not sending): at most 1.7 %
  of one core, about the run-to-run spread. The CPU here is the relay itself: about 4.6-6.8 s per minute before the destination
  sends and 14-20 s per minute while it sends (23-33 % of one core of a 7700X), in both modes.
  Those absolute figures come from ffmpeg standing in for OBS and for the platform, which makes
  both of the relay's sockets carry 128-byte RTMP chunks; with a publisher that behaves like OBS and
  a platform announcing 4096-byte chunks (Twitch, Kick, Facebook) the relay takes 1-1.5 s per
  minute: `docs/STREAM_MODE.md`, "CPU". The disk-minus-RAM difference above still holds.
- **The two SSDs behaved the same**: same memory (within 1-2 MB), same CPU, no fallback, lag at
  most 0.4 s in steady state.
- **Physical writes match.** E: carried only the spill in batch 2: the drive counter showed
  341-351 MiB/min written over 6-12 minute windows while all six were streaming, against 307.5 MiB/min of spill writes
  (3 x 44.1 + 3 x 58.4) reported by the six processes, a ratio of 1.12 that also includes NTFS
  metadata and whatever else wrote to E: (other work on that drive was not stopped). So the spill
  reaches the SSD; the cache does not absorb it. (C: is the system drive and too busy for the same
  check.)

**Resyncs.** Two runs had one resync each, both 8 Mbps / 120 s, one in disk mode (C:, at t=347) and
one in RAM only (at t=334), both in batch 1 with 16 relays and 16 recorders running at once. In both
the destination's sent bytes stopped for about 3 s, then the relay logged "the upload fell behind;
jumped back to the 120s delay" and the recording skips 3.0 s and 3.5 s of source forward. Nothing
like it in batch 2 (6 relays). It happened in both modes alike; the likeliest cause is the local
ffmpeg recorder not reading for those seconds, but that was not measured.

### Live raise from 10 to 300 s (6 Mbps, history full, t=660)

| Run | Delay reached 300 s | Output stopped (no bytes sent) | Splice in the recording | Recording |
|---|---|---|---|---|
| Disk, C: | at the next status line (t=661, `behind` 300.6 s) | never (longest gap over the whole run: 1 s, at t=348) | back 299.7 s of source, at output 653 s | 0 mismatches, DTS increasing, 0 decode errors |
| Disk, E: | at the next status line (t=661, `behind` 300.0 s) | never (0 s) | back 291.7 s of source, at output 653 s | 0 mismatches, DTS increasing, 0 decode errors |
| RAM only, keep 60 | after 235 s (t=895) | 233 s (t=661-894) | back 55.6 s (all it had), then waited | 0 mismatches, DTS increasing, 0 decode errors |
| RAM only, keep 10 | after 285 s (t=945) | 284 s (t=661-944) | back 5.7 s, then waited | 0 mismatches, DTS increasing, 0 decode errors |

**With the spill the raise was instant**: the relay rewound into the history on disk and kept
sending; the I/O counters show it reading back the older blocks right away (5 MiB in the first
second, then at the stream's bitrate). **RAM only**, the history did not reach 300 s, so the relay
rewound what it had and the platform got nothing for 233-284 s while the delay built up.

Not explained here: the two disk runs rewound 299.7 s and 291.7 s of source for the same 290 s
raise (their recordings differ by the same 8 s: 1101.4 s vs 1093.4 s), although `behind` read
300.0-300.6 s in both from t=661. Every packet was still byte-identical to the source with DTS
increasing and a clean decode. Left for the relay's owners to look at.

### Output correctness, all runs

| Run | Video / audio packets out | Not identical to a source packet | Video DTS not increasing | Decode errors | Splices (output s: source jump s) |
|---|---|---|---|---|---|
| disk C: 6 Mbps 10 s | 33044 / 51628 | 0 | 0 | 0 | 2: -1.7; 653: -299.7 |
| disk C: 6 Mbps 120 s | 24053 / 37581 | 0 | 0 | 0 | 2: -1.7 |
| disk C: 6 Mbps 600 s | 24052 / 37579 | 0 | 0 | 0 | 2: -1.7 |
| disk C: 8 Mbps 10 s | 24040 / 37560 | 0 | 0 | 0 | 1: -1.3 |
| disk C: 8 Mbps 120 s | 23951 / 37421 | 0 | 0 | 0 | 1: -1.3; 220: +3.0 (resync) |
| disk C: 8 Mbps 600 s | 24039 / 37559 | 0 | 0 | 0 | 1: -1.3 |
| disk E: 6 Mbps 10 s | 32804 / 51254 | 0 | 0 | 0 | 2: -1.7; 653: -291.7 |
| disk E: 6 Mbps 120 s | 24052 / 37579 | 0 | 0 | 0 | 2: -1.7 |
| disk E: 6 Mbps 600 s | 24052 / 37579 | 0 | 0 | 0 | 2: -1.7 |
| disk E: 8 Mbps 10 s | 24037 / 37557 | 0 | 0 | 0 | 1: -1.2 |
| disk E: 8 Mbps 120 s | 24038 / 37559 | 0 | 0 | 0 | 1: -1.2 |
| disk E: 8 Mbps 600 s | 24037 / 37556 | 0 | 0 | 0 | 1: -1.2 |
| RAM keep 60, 6 Mbps 10 s | 25722 / 40188 | 0 | 0 | 0 | 2: -1.7; 653: -55.6 |
| RAM keep 60, 6 Mbps 120 s | 24052 / 37580 | 0 | 0 | 0 | 2: -1.7 |
| RAM keep 60, 6 Mbps 600 s | 24052 / 37579 | 0 | 0 | 0 | 2: -1.7 |
| RAM keep 60, 8 Mbps 10 s | 24038 / 37557 | 0 | 0 | 0 | 1: -1.2 |
| RAM keep 60, 8 Mbps 120 s | 24038 / 37557 | 0 | 0 | 0 | 1: -1.2 |
| RAM keep 60, 8 Mbps 600 s | 24043 / 37565 | 0 | 0 | 0 | 1: -1.4 |
| RAM keep 10, 6 Mbps 10 s | 24224 / 37849 | 0 | 0 | 0 | 2: -1.7; 653: -5.7 |
| RAM keep 120, 6 Mbps 120 s | 24040 / 37560 | 0 | 0 | 0 | 1: -1.3 |
| RAM keep 10, 8 Mbps 10 s | 24041 / 37563 | 0 | 0 | 0 | 1: -1.3 |
| RAM keep 120, 8 Mbps 120 s | 23930 / 37388 | 0 | 0 | 0 | 1: -1.1; 210: +3.5 (resync) |

The splice at 1-2 s in every run is the delay being set at t=0, when the history held 1.1-1.7 s:
the relay rewinds what it has and then waits for the rest, as designed. The source has 24000 video
packets (800 s); the recordings have ~24040 (the 1.1-1.7 s rewound at t=0 sent twice).

## SSD wear

**Measured**: what the relay wrote to the spill file (process counters, above): 44.1 MiB/min at
6.17 Mbps and 57.2-58.4 MiB/min at 8.17 Mbps, the same with any delay; and the physical write
counter of E: at 1.12x that (an upper bound, see above).

**Computed** from those (decimal GB/TB, as drive ratings use):

| Stream | Written per hour (process) | per 100 h (process) | per 100 h (x1.12, physical upper bound) |
|---|---|---|---|
| 6 Mbps (6.17) | 2.78 GB | 278 GB | 311 GB |
| 8 Mbps (8.17) | 3.60-3.67 GB | 360-367 GB | 403-412 GB |

Against drive ratings (TBW, terabytes written within the warranty), using the physical upper bound
(3.11 GB/h at 6 Mbps, 4.12 GB/h at 8 Mbps):

| Rating | Example | 100 h at 6 Mbps | 100 h at 8 Mbps | Hours of streaming to reach the rating (6 / 8 Mbps) |
|---|---|---|---|---|
| 150 TBW | | 0.21 % | 0.27 % | 48,000 / 36,000 |
| 220 TBW | Crucial P3 1 TB (this machine's C:) | 0.14 % | 0.19 % | 71,000 / 54,000 |
| 300 TBW | | 0.10 % | 0.14 % | 96,000 / 73,000 |
| 600 TBW | | 0.05 % | 0.07 % | 193,000 / 146,000 |
| 1200 TBW | WD_BLACK SN7100 2 TB (this machine's E:) | 0.03 % | 0.03 % | 385,000 / 292,000 |

Streaming 4 hours every day of the year (1,460 h) writes about 4.5 TB at 6 Mbps and 6.0 TB at
8 Mbps: 2.1 % / 2.7 % of a 220 TBW drive per year.

Ratings: Crucial P3 1 TB 220 TBW and WD_BLACK SN7100 2 TB 1,200 TBW, from the manufacturers' spec
sheets as quoted by Tom's Hardware and retailer listings (checked 2026-11-11).

Not measured: the drive's own NAND writes (write amplification). SMART host/NAND write counters
need administrator rights, which this run did not have. The spill writes whole 1 MiB blocks in
order, which keeps amplification low on SSDs, but the factor was not measured; a factor of 2 would
double every percentage above.

## Exact commands

Drives:

```powershell
Get-PhysicalDisk | Select DeviceId,FriendlyName,MediaType,BusType,Size,HealthStatus
Get-Partition | Where DriveLetter | Select DiskNumber,DriveLetter,Size
Get-Volume | Where DriveLetter | Select DriveLetter,FileSystemLabel,FileSystem,DriveType,SizeRemaining,Size
```

Harness build (this branch's worktree, `.deps` a junction to the main checkout's):

```bash
cmake --preset windows-x64 -DTRIGGLOW_BUILD_TESTS=ON
cmake --build build_x64 --config RelWithDebInfo --target relay_harness
```

Sources (R = 6 and 8):

```bash
ffmpeg -f lavfi -i "testsrc2=size=1920x1080:rate=30,noise=alls=12:allf=t+u" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 800 \
  -c:v libx264 -preset veryfast -b:v ${R}000k -minrate ${R}000k -maxrate ${R}000k -bufsize $((R*2))000k \
  -x264-params nal-hrd=cbr:force-cfr=1 -g 60 -keyint_min 60 -sc_threshold 0 -bf 2 -pix_fmt yuv420p \
  -c:a aac -b:a 160k -ar 48000 -ac 2 -f flv src_${R}M.flv
```

One run (`tools/buffer-bench/run.sh NAME SRC TMPDIR OUTDIR SCRIPT KEEP MAX SPILLMB PORT`):

```bash
ffmpeg -listen 1 -f flv -i rtmp://127.0.0.1:$PORT/live/k -map 0 -c copy -f flv $OUTDIR/$NAME.flv &
TMP="$TMPDIR" TEMP="$TMPDIR" relay_harness rtmp://127.0.0.1:$PORT/live k "$SCRIPT" $KEEP $MAX $SPILLMB > $NAME.log &
# after the harness prints "PUBLISH <url> <key>":
ffmpeg -re -i $SRC -map 0 -c copy -f flv "<url>/<key>"
```

The 22 runs (`tools/buffer-bench/batch1.sh`, `batch2.sh`), e.g.:

```bash
run.sh cd6_10   src_6M.flv 'C:\dd-bench-tmp' /e/dd-bench-out "0:10,660:300" 60 1500 0  19410  # disk on C:, raise
run.sh cd8_600  src_8M.flv 'C:\dd-bench-tmp' /e/dd-bench-out "0:600"        60 1500 0  19415  # disk on C:
run.sh ed6_120  src_6M.flv 'E:\dd-bench-tmp' /c/dd-bench-out "0:120"        60 1500 0  19431  # disk on E:
run.sh r60_6_10 src_6M.flv 'E:\dd-bench-tmp' /e/dd-bench-out "0:10,660:300" 60 1500 -1 19416  # RAM, keep 60, raise
run.sh rd_8_120 src_8M.flv 'E:\dd-bench-tmp' /e/dd-bench-out "0:120"        120 1500 -1 19425 # RAM, keep = delay
```

Analysis:

```bash
python -I summarize.py cd6_10 ... > summary_b1.jsonl        # memory, CPU, I/O, lag per run
python -I verify.py src_6M.flv /e/dd-bench-out/cd6_10.flv    # packet hashes, DTS, splices, decode
python -I raise.py cd6_10 ed6_10 r60_6_10 rd_6_10            # when the raise reached 300 s, send gaps
python -I tables.py                                          # the tables above
powershell -File sampler.ps1 -Out sampler.csv -Stop stop     # physical disk / page lists every 10 s
```

The scripts carry this machine's paths (`E:\dd-bench-work`, the worktree's build); the test
folders (`C:\dd-bench-tmp`, `E:\dd-bench-tmp`, `C:\dd-bench-out`, `E:\dd-bench-out`,
`E:\dd-bench-work`) were created for the test and deleted afterwards.

## Limits

- Two NVMe SSDs only. No HDD, no USB drive, no SATA SSD: the "Disk on HDD" default is not backed by
  a measurement here.
- The relay ran in `relay_harness`, not inside OBS; OBS's own memory and CPU are not included.
- Loopback only: no real platform, no real network.
- One synthetic source per bitrate; real gameplay at the same CBR bitrate writes the same bytes per
  second.
- Windows only.
