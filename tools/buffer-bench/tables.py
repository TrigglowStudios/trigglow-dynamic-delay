"""Markdown tables from logs/summary_*.jsonl and logs/verify_*.jsonl."""
import glob
import json

S = {}
for f in glob.glob('logs/summary_*.jsonl'):
    for l in open(f):
        d = json.loads(l)
        S[d['name']] = d
V = {}
for f in glob.glob('logs/verify_*.jsonl'):
    for l in open(f):
        d = json.loads(l)
        V[d['out'][:-4]] = d

ORDER = []
for mode, prefix, label in [('disk', 'cd', 'Disk (spill), C: Crucial P3 1 TB'),
                            ('disk', 'ed', 'Disk (spill), E: WD_BLACK SN7100 2 TB'),
                            ('ram60', 'r60_', 'RAM only, keep 60 s'),
                            ('ramd', 'rd_', 'RAM only, keep = delay')]:
    for br in (6, 8):
        for dl in (10, 120, 600):
            if prefix in ('cd', 'ed'):
                n = f'{prefix}{br}_{dl}'
            else:
                n = f'{prefix}{br}_{dl}'
            if n in S:
                ORDER.append((label, br, dl, n))


def pre_raise_peak(name):
    import re
    best = 0.0
    bestws = 0.0
    for line in open(f'logs/{name}.log'):
        m = re.match(r't=\s*(\d+) .*priv=([\d.]+)MB ws=([\d.]+)MB', line)
        if m and int(m.group(1)) < 660:
            best = max(best, float(m.group(2)))
            bestws = max(bestws, float(m.group(3)))
    return best, bestws


print('### Memory\n')
print('| Mode | Stream | Delay | History kept | of it in RAM | on disk | Private bytes: start / steady / peak (MB) | Working set: steady / peak (MB) | Over the start, steady |')
print('|---|---|---|---|---|---|---|---|---|')
for label, br, dl, n in ORDER:
    d = S[n]
    pp, pw = pre_raise_peak(n)
    note = f' (before the raise: {pp:.1f} sampled)' if n.endswith('6_10') else ''
    print(f"| {label} | {br} Mbps | {dl} s | {d['steady_hist_s']:.0f} s, {d['steady_hist_mb']:.0f} MB | {d['steady_hram_mb']:.0f} MB | {d['steady_disk_mb']:.0f} MB | "
          f"{d['base_priv']:.1f} / {d['steady_priv']:.1f} / {d['peak_priv']:.1f}{note} | {d['steady_ws']:.1f} / {d['peak_ws']:.1f} | +{d['steady_priv'] - d['base_priv']:.0f} MB |")

print('\n### CPU, disk I/O, lag\n')
print('| Mode | Stream | Delay | CPU s per min (t 55-655) | Written MiB/min | Read MiB/min | Written total MiB | Read total MiB | Spill fallbacks | Max lag (steady) | Resyncs |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for label, br, dl, n in ORDER:
    d = S[n]
    fb = 'n/a' if d['name'].startswith('r') else ('0' if d['spill_off_lines_while_streaming'] == 0 and not d['spill_notes'] else str(d['spill_notes']))
    print(f"| {label} | {br} Mbps | {dl} s | {d['cpu_s_per_min']:.1f} | {d['write_mb_per_min']:.1f} | {d['read_mb_per_min']:.1f} | "
          f"{d['write_mb_total']:.0f} | {d['read_mb_total']:.0f} | {fb} | {d['lag_max_s_steady']:.1f} s | {d['resyncs']} |")

print('\n### Output correctness\n')
print('| Run | Video / audio packets out | Not byte-identical to a source packet | Video DTS not increasing | Decode errors | Splices (output s: source jump s) |')
print('|---|---|---|---|---|---|')
for label, br, dl, n in ORDER:
    v = V.get(n)
    if not v:
        print(f'| {n} | not verified | | | | |')
        continue
    sp = '; '.join(f"{s['out_s']:.0f}: {s['jump_s']:+.1f}" for s in v['splices'])
    print(f"| {n} | {v['video_pkts']} / {v['audio_pkts']} | {v['video_unmatched'] + v['audio_unmatched']} | {v['video_dts_not_increasing']} | {v['decode_errors']} | {sp} |")
