### Memory

| Mode | Stream | Delay | History kept | of it in RAM | on disk | Private bytes: start / steady / peak (MB) | Working set: steady / peak (MB) | Over the start, steady |
|---|---|---|---|---|---|---|---|---|
| Disk (spill), C: Crucial P3 1 TB | 6 Mbps | 10 s | 606 s, 445 MB | 11 MB | 440 MB | 21.0 / 32.4 / 36.9 (before the raise: 34.0 sampled) | 34.6 / 38.8 | +11 MB |
| Disk (spill), C: Crucial P3 1 TB | 6 Mbps | 120 s | 606 s, 445 MB | 13 MB | 440 MB | 21.1 / 35.3 / 37.1 | 37.2 / 39.1 | +14 MB |
| Disk (spill), C: Crucial P3 1 TB | 6 Mbps | 600 s | 606 s, 445 MB | 13 MB | 440 MB | 21.1 / 34.3 / 36.5 | 36.3 / 38.1 | +13 MB |
| Disk (spill), C: Crucial P3 1 TB | 8 Mbps | 10 s | 606 s, 578 MB | 14 MB | 570 MB | 21.1 / 34.7 / 39.2 | 37.1 / 40.5 | +14 MB |
| Disk (spill), C: Crucial P3 1 TB | 8 Mbps | 120 s | 606 s, 584 MB | 16 MB | 576 MB | 21.3 / 37.5 / 42.1 | 39.1 / 43.1 | +16 MB |
| Disk (spill), C: Crucial P3 1 TB | 8 Mbps | 600 s | 606 s, 578 MB | 16 MB | 571 MB | 21.4 / 36.2 / 44.4 | 38.3 / 44.8 | +15 MB |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 6 Mbps | 10 s | 606 s, 445 MB | 11 MB | 440 MB | 21.2 / 32.8 / 35.7 (before the raise: 33.6 sampled) | 34.7 / 37.7 | +12 MB |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 6 Mbps | 120 s | 606 s, 445 MB | 13 MB | 440 MB | 21.2 / 34.6 / 36.7 | 36.5 / 38.4 | +13 MB |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 6 Mbps | 600 s | 606 s, 445 MB | 12 MB | 440 MB | 21.1 / 33.9 / 36.4 | 35.7 / 37.5 | +13 MB |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 8 Mbps | 10 s | 606 s, 589 MB | 13 MB | 582 MB | 21.2 / 34.2 / 36.4 | 36.1 / 38.1 | +13 MB |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 8 Mbps | 120 s | 606 s, 589 MB | 15 MB | 582 MB | 21.2 / 36.2 / 38.8 | 38.0 / 40.4 | +15 MB |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 8 Mbps | 600 s | 606 s, 589 MB | 15 MB | 582 MB | 21.2 / 36.6 / 38.9 | 38.0 / 40.3 | +15 MB |
| RAM only, keep 60 s | 6 Mbps | 10 s | 66 s, 49 MB | 50 MB | 0 MB | 20.9 / 71.0 / 173.6 (before the raise: 75.5 sampled) | 73.2 / 174.6 | +50 MB |
| RAM only, keep 60 s | 6 Mbps | 120 s | 126 s, 97 MB | 100 MB | 0 MB | 20.8 / 121.6 / 123.4 | 122.5 / 124.1 | +101 MB |
| RAM only, keep 60 s | 6 Mbps | 600 s | 606 s, 445 MB | 449 MB | 0 MB | 21.8 / 473.1 / 475.4 | 473.9 / 476.6 | +451 MB |
| RAM only, keep 60 s | 8 Mbps | 10 s | 66 s, 68 MB | 69 MB | 0 MB | 20.6 / 89.8 / 98.3 | 92.3 / 100.4 | +69 MB |
| RAM only, keep 60 s | 8 Mbps | 120 s | 126 s, 129 MB | 131 MB | 0 MB | 20.6 / 151.6 / 154.9 | 153.2 / 156.0 | +131 MB |
| RAM only, keep 60 s | 8 Mbps | 600 s | 606 s, 578 MB | 581 MB | 0 MB | 20.6 / 605.6 / 619.5 | 605.8 / 619.0 | +585 MB |
| RAM only, keep = delay | 6 Mbps | 10 s | 16 s, 12 MB | 13 MB | 0 MB | 21.2 / 35.6 / 138.7 (before the raise: 37.1 sampled) | 36.5 / 138.3 | +14 MB |
| RAM only, keep = delay | 6 Mbps | 120 s | 126 s, 92 MB | 95 MB | 0 MB | 20.5 / 116.8 / 123.3 | 118.6 / 124.3 | +96 MB |
| RAM only, keep = delay | 8 Mbps | 10 s | 16 s, 16 MB | 17 MB | 0 MB | 20.8 / 37.2 / 44.6 | 39.2 / 45.9 | +16 MB |
| RAM only, keep = delay | 8 Mbps | 120 s | 126 s, 129 MB | 131 MB | 0 MB | 20.6 / 151.8 / 155.3 | 153.3 / 156.2 | +131 MB |

### CPU, disk I/O, lag

| Mode | Stream | Delay | CPU s per min (t 55-655) | Written MiB/min | Read MiB/min | Written total MiB | Read total MiB | Spill fallbacks | Max lag (steady) | Resyncs |
|---|---|---|---|---|---|---|---|---|---|---|
| Disk (spill), C: Crucial P3 1 TB | 6 Mbps | 10 s | 15.5 | 44.1 | 0.0 | 588 | 330 | 0 | 0.1 s | 0 |
| Disk (spill), C: Crucial P3 1 TB | 6 Mbps | 120 s | 14.4 | 44.1 | 39.3 | 588 | 584 | 0 | 0.0 s | 0 |
| Disk (spill), C: Crucial P3 1 TB | 6 Mbps | 600 s | 4.6 | 44.1 | 4.0 | 588 | 584 | 0 | 0.1 s | 0 |
| Disk (spill), C: Crucial P3 1 TB | 8 Mbps | 10 s | 19.5 | 57.2 | 0.0 | 778 | 0 | 0 | 0.2 s | 0 |
| Disk (spill), C: Crucial P3 1 TB | 8 Mbps | 120 s | 18.0 | 57.8 | 51.1 | 778 | 774 | 0 | 0.4 s | 1 |
| Disk (spill), C: Crucial P3 1 TB | 8 Mbps | 600 s | 6.0 | 57.3 | 5.4 | 778 | 774 | 0 | 0.3 s | 0 |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 6 Mbps | 10 s | 15.6 | 44.1 | 0.0 | 588 | 324 | 0 | 0.0 s | 0 |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 6 Mbps | 120 s | 14.5 | 44.1 | 39.3 | 588 | 584 | 0 | 0.0 s | 0 |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 6 Mbps | 600 s | 5.3 | 44.1 | 4.1 | 588 | 584 | 0 | 0.0 s | 0 |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 8 Mbps | 10 s | 20.0 | 58.4 | 0.0 | 778 | 0 | 0 | 0.1 s | 0 |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 8 Mbps | 120 s | 18.6 | 58.4 | 52.1 | 778 | 774 | 0 | 0.1 s | 0 |
| Disk (spill), E: WD_BLACK SN7100 2 TB | 8 Mbps | 600 s | 6.8 | 58.4 | 5.4 | 778 | 774 | 0 | 0.1 s | 0 |
| RAM only, keep 60 s | 6 Mbps | 10 s | 15.2 | 0.0 | 0.0 | 0 | 0 | n/a | 0.1 s | 0 |
| RAM only, keep 60 s | 6 Mbps | 120 s | 14.1 | 0.0 | 0.0 | 0 | 0 | n/a | 0.2 s | 0 |
| RAM only, keep 60 s | 6 Mbps | 600 s | 4.6 | 0.0 | 0.0 | 0 | 0 | n/a | 0.4 s | 0 |
| RAM only, keep 60 s | 8 Mbps | 10 s | 19.6 | 0.0 | 0.0 | 0 | 0 | n/a | 0.4 s | 0 |
| RAM only, keep 60 s | 8 Mbps | 120 s | 17.7 | 0.0 | 0.0 | 0 | 0 | n/a | 1.2 s | 0 |
| RAM only, keep 60 s | 8 Mbps | 600 s | 5.8 | 0.0 | 0.0 | 0 | 0 | n/a | 1.0 s | 0 |
| RAM only, keep = delay | 6 Mbps | 10 s | 15.4 | 0.0 | 0.0 | 0 | 0 | n/a | 0.5 s | 0 |
| RAM only, keep = delay | 6 Mbps | 120 s | 14.0 | 0.0 | 0.0 | 0 | 0 | n/a | 0.3 s | 0 |
| RAM only, keep = delay | 8 Mbps | 10 s | 19.5 | 0.0 | 0.0 | 0 | 0 | n/a | 0.6 s | 0 |
| RAM only, keep = delay | 8 Mbps | 120 s | 17.8 | 0.0 | 0.0 | 0 | 0 | n/a | 1.0 s | 1 |

### Output correctness

| Run | Video / audio packets out | Not byte-identical to a source packet | Video DTS not increasing | Decode errors | Splices (output s: source jump s) |
|---|---|---|---|---|---|
| cd6_10 | 33044 / 51628 | 0 | 0 | 0 | 2: -1.7; 653: -299.7 |
| cd6_120 | 24053 / 37581 | 0 | 0 | 0 | 2: -1.7 |
| cd6_600 | 24052 / 37579 | 0 | 0 | 0 | 2: -1.7 |
| cd8_10 | 24040 / 37560 | 0 | 0 | 0 | 1: -1.3 |
| cd8_120 | 23951 / 37421 | 0 | 0 | 0 | 1: -1.3; 220: +3.0 |
| cd8_600 | 24039 / 37559 | 0 | 0 | 0 | 1: -1.3 |
| ed6_10 | 32804 / 51254 | 0 | 0 | 0 | 2: -1.7; 653: -291.7 |
| ed6_120 | 24052 / 37579 | 0 | 0 | 0 | 2: -1.7 |
| ed6_600 | 24052 / 37579 | 0 | 0 | 0 | 2: -1.7 |
| ed8_10 | 24037 / 37557 | 0 | 0 | 0 | 1: -1.2 |
| ed8_120 | 24038 / 37559 | 0 | 0 | 0 | 1: -1.2 |
| ed8_600 | 24037 / 37556 | 0 | 0 | 0 | 1: -1.2 |
| r60_6_10 | 25722 / 40188 | 0 | 0 | 0 | 2: -1.7; 653: -55.6 |
| r60_6_120 | 24052 / 37580 | 0 | 0 | 0 | 2: -1.7 |
| r60_6_600 | 24052 / 37579 | 0 | 0 | 0 | 2: -1.7 |
| r60_8_10 | 24038 / 37557 | 0 | 0 | 0 | 1: -1.2 |
| r60_8_120 | 24038 / 37557 | 0 | 0 | 0 | 1: -1.2 |
| r60_8_600 | 24043 / 37565 | 0 | 0 | 0 | 1: -1.4 |
| rd_6_10 | 24224 / 37849 | 0 | 0 | 0 | 2: -1.7; 653: -5.7 |
| rd_6_120 | 24040 / 37560 | 0 | 0 | 0 | 1: -1.3 |
| rd_8_10 | 24041 / 37563 | 0 | 0 | 0 | 1: -1.3 |
| rd_8_120 | 23930 / 37388 | 0 | 0 | 0 | 1: -1.1; 210: +3.5 |
