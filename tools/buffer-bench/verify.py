"""Output correctness for one relay_harness run.

  python -I verify.py SRC.flv OUT.flv

- every output packet (video and audio) is byte-identical to a packet of the
  source (sha256 of the packet data, via ffmpeg's framehash muxer, stream copy)
- video DTS strictly increasing, audio DTS non-decreasing
- the largest step between consecutive DTS per stream, and how many steps
  are over 200 ms (a hole in the output's timeline)
- splices: where consecutive output video packets are not consecutive source
  packets (a live delay change or a resync), with the jump in source seconds
- clean decode: ffmpeg -v error decoding the whole output prints nothing
Prints one JSON line.
"""
import json
import os
import subprocess
import sys


def packets(path):
    cache = path + '.framehash'
    if not path.endswith('.flv') or not os.path.exists(cache) or os.path.getmtime(cache) < os.path.getmtime(path):
        out = subprocess.run(
            ['ffmpeg', '-v', 'error', '-i', path, '-map', '0', '-c', 'copy', '-f', 'framehash', '-hash', 'sha256', '-'],
            capture_output=True, text=True, check=True).stdout
        if 'src_' in os.path.basename(path):
            with open(cache, 'w') as f:
                f.write(out)
    else:
        with open(cache) as f:
            out = f.read()
    rows = []
    for line in out.splitlines():
        if not line or line.startswith('#'):
            continue
        parts = [p.strip() for p in line.split(',')]
        # stream, dts, pts, duration, size, hash
        rows.append((int(parts[0]), int(parts[1]), int(parts[4]), parts[5]))
    return rows


def main():
    src, out = sys.argv[1], sys.argv[2]
    s = packets(src)
    o = packets(out)
    # FLV: stream 0 video, 1 audio (both files written the same way), ms timebase.
    src_index = {}
    src_all = set()
    for i, (st, dts, size, h) in enumerate(s):
        src_all.add((st, size, h))
        if st == 0:
            src_index.setdefault(h, (i, dts))
    unmatched = {0: 0, 1: 0}
    counts = {0: 0, 1: 0}
    last = {0: None, 1: None}
    dts_bad = {0: 0, 1: 0}
    max_step = {0: 0, 1: 0}
    gaps = {0: 0, 1: 0}
    splices = []
    prev_src = None
    prev_src_dts = None
    for st, dts, size, h in o:
        counts[st] = counts.get(st, 0) + 1
        if (st, size, h) not in src_all:
            unmatched[st] = unmatched.get(st, 0) + 1
        if last.get(st) is not None:
            if (st == 0 and dts <= last[st]) or (st != 0 and dts < last[st]):
                dts_bad[st] = dts_bad.get(st, 0) + 1
            step = dts - last[st]
            max_step[st] = max(max_step.get(st, 0), step)
            if step > 200:
                gaps[st] = gaps.get(st, 0) + 1
        last[st] = dts
        if st == 0 and h in src_index:
            idx, sdts = src_index[h]
            if prev_src is not None:
                # next video packet of the source
                j = prev_src + 1
                while j < len(s) and s[j][0] != 0:
                    j += 1
                if idx != j:
                    splices.append({'out_s': round(dts / 1000.0, 2), 'jump_s': round((sdts - prev_src_dts) / 1000.0, 2)})
            prev_src, prev_src_dts = idx, sdts
    dec = subprocess.run(['ffmpeg', '-v', 'error', '-i', out, '-f', 'null', '-'], capture_output=True, text=True)
    dec_lines = [l for l in dec.stderr.splitlines() if l.strip()]
    res = {
        'out': os.path.basename(out),
        'video_pkts': counts.get(0, 0), 'audio_pkts': counts.get(1, 0),
        'video_unmatched': unmatched.get(0, 0), 'audio_unmatched': unmatched.get(1, 0),
        'video_dts_not_increasing': dts_bad.get(0, 0), 'audio_dts_decreasing': dts_bad.get(1, 0),
        'video_max_step_ms': max_step.get(0, 0), 'audio_max_step_ms': max_step.get(1, 0),
        'video_gaps_over_200ms': gaps.get(0, 0), 'audio_gaps_over_200ms': gaps.get(1, 0),
        'out_video_s': round((last[0] or 0) / 1000.0, 1),
        'splices': splices,
        'decode_errors': len(dec_lines), 'decode_first': dec_lines[:3],
    }
    print(json.dumps(res))


main()
