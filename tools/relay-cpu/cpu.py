"""CPU of relay_harness runs: python -I cpu.py LOG [LOG ...] -> one JSON line each.

Per phase of the stream, CPU seconds per minute of stream from the harness's
"cpu=" (Windows' process times, sampled at the clock tick) and "cyc=" (exact
CPU cycles, in millions per second):
- steady: OBS sending and the destination sending (t = delay + 10 to the end
  of the stream - 5)
- ingest_only: before the delay has passed (delays of 30 s or more)
- drain_only: after OBS stopped, while the delay is sent out
"""
import json
import os
import re
import sys

LINE = re.compile(r'^t=\s*(\d+) in=\d ended=(\d) delay=(\d+).*cpu=([\d.]+)s(?: cyc=(\d+)M)?')


def main():
    for path in sys.argv[1:]:
        rows = {}
        for line in open(path, encoding='utf-8', errors='replace'):
            m = LINE.match(line)
            if m:
                rows[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), float(m.group(4)),
                                         int(m.group(5)) if m.group(5) else None)
        ts = sorted(rows)
        end = max(t for t in ts if not rows[t][0])
        first_delay = rows[ts[0]][1]

        def rate(a, b):
            if a not in rows or b not in rows or b <= a:
                return None
            out = {'cpu_s_per_min': round((rows[b][2] - rows[a][2]) / (b - a) * 60, 2)}
            if rows[a][3] is not None:
                out['mcycles_per_s'] = round((rows[b][3] - rows[a][3]) / (b - a))
            return out

        res = {'run': os.path.basename(path), 'stream_s': end, 'cpu_total_s': rows[ts[-1]][2]}
        start = max(first_delay, 0) + 10 if first_delay < 30 else first_delay + 10
        res['steady'] = rate(start if first_delay >= 30 else 30, end - 5)
        if first_delay >= 30:
            res['ingest_only'] = rate(15, first_delay - 5)
            res['drain_only'] = rate(end + 10, min(ts[-1] - 5, end + first_delay - 5))
        print(json.dumps(res))


main()
