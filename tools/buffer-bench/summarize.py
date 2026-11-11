"""Summarize relay_harness logs: python -I summarize.py NAME [NAME ...] -> JSON lines.

Windows (status line t=N):
- base: the first status line (t=0, OBS's stream just reached the relay)
- steady: t in [620, 655] (history full in every mode, before the 660 s raise)
- cpu/io rates: from t=55 to t=655 (10 minutes of stream, history filling and full)
"""
import json
import re
import statistics
import sys

LINE = re.compile(r'^t=\s*(\d+) ')
KV = re.compile(r'(\w+)=([-\d.]+)')


def parse(path):
    rows = {}
    events = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.match(line)
        if m:
            t = int(m.group(1))
            d = {}
            for k, v in KV.findall(line):
                # first occurrence wins except for duplicated names
                if k not in d:
                    try:
                        d[k] = float(v)
                    except ValueError:
                        pass
            hist = re.search(r'hist=([\d.]+)s ([\d.]+)MB', line)
            br = re.search(r'spill=\d ([\d.]+)Mbps', line)
            d['Mbps'] = float(br.group(1)) if br else 0.0
            d['hist_s'] = float(hist.group(1))
            d['hist_mb'] = float(hist.group(2))
            d['spill_note'] = line.rsplit('raise=', 1)[1].split(' ', 1)[1].strip() if 'raise=' in line else ''
            d['ended'] = int(re.search(r'ended=(\d)', line).group(1))
            rows[t] = d
        elif line.startswith(('EVENT', '[warn]', '[error]', 'SET', 'DUMP')) or 'spill' in line.lower():
            events.append(line.strip())
    return rows, events


def summarize(name):
    rows, events = parse(f'logs/{name}.log')
    ts = sorted(rows)
    base = rows[ts[0]]
    steady = [rows[t] for t in ts if 620 <= t <= 655]
    end_stream = max((t for t in ts if not rows[t]['ended']), default=ts[-1])
    last = rows[ts[-1]]
    r55, r655 = rows.get(55), rows.get(655)
    delay = steady[-1]['delay'] if steady else base['delay']
    lag = [abs(rows[t]['behind'] - rows[t]['delay']) for t in ts
           if 620 <= t <= 655 and rows[t]['state'] == 2]
    out = {
        'name': name,
        'base_priv': base['priv'], 'base_ws': base['ws'],
        'steady_priv': round(statistics.median(r['priv'] for r in steady), 1),
        'steady_ws': round(statistics.median(r['ws'] for r in steady), 1),
        'peak_priv': last['ppriv'], 'peak_ws': last['pws'],
        'steady_hist_s': round(statistics.median(r['hist_s'] for r in steady), 1),
        'steady_hist_mb': round(statistics.median(r['hist_mb'] for r in steady), 1),
        'steady_hram_mb': round(statistics.median(r['hram'] for r in steady), 1),
        'steady_disk_mb': round(statistics.median(r['disk'] for r in steady), 1),
        'max_disk_mb': max(rows[t]['disk'] for t in ts),
        'spill_off_lines_while_streaming': sum(1 for t in ts if not rows[t]['ended'] and rows[t]['spill'] == 0),
        'cpu_s_per_min': round((r655['cpu'] - r55['cpu']) / 10.0, 2) if r55 and r655 else None,
        'cpu_total_s': last['cpu'], 'run_s': ts[-1], 'stream_s': end_stream,
        'write_mb_per_min': round((r655['iow'] - r55['iow']) / 10.0 / 1048576, 2) if r55 and r655 else None,
        'read_mb_per_min': round((r655['ior'] - r55['ior']) / 10.0 / 1048576, 2) if r55 and r655 else None,
        'write_mb_total': round(last['iow'] / 1048576, 1), 'read_mb_total': round(last['ior'] / 1048576, 1),
        'other_mb_total': round(last['ioo'] / 1048576, 1),
        'lag_max_s_steady': round(max(lag), 2) if lag else None,
        'resyncs': int(last['rs']), 'reconnects': int(last['rc']),
        'delay': delay, 'bitrate_mbps': round(statistics.median(r['Mbps'] for r in steady), 2) if steady and 'Mbps' in steady[0] else None,
        'raise_s': steady[-1].get('raise') if steady else None,
        'spill_notes': sorted({rows[t]['spill_note'] for t in ts if rows[t]['spill_note']}),
        'events': [e for e in events if not e.startswith('SET')][:12],
    }
    return out


for n in sys.argv[1:]:
    print(json.dumps(summarize(n)))
