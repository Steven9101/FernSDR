"""Compare tap presentation timestamps with independent PulseAudio monitor time."""
import argparse
import json
from pathlib import Path
import sys
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'analysis'))
from latency import presentation_map


def analyse(directory, injected_offset_ms=0):
    d = Path(directory)
    tap = json.loads((d/'tap.json').read_text())
    rows = [json.loads(line) for line in (d/'monitor.jsonl').read_text().splitlines()]
    if any(row['ev'] == 'hole' for row in rows):
        raise ValueError('independent monitor lost samples')
    header = rows[0]
    timing = [r for r in rows if r['ev']=='timing' and r['write_bytes'] >= 0 and r['synchronized']
              and not r['write_corrupt'] and not r['read_corrupt'] and not r['suspended']]
    if len(timing) < 20:
        raise ValueError('too few valid monitor snapshots')
    if any(r['read_bytes'] != r['captured_frames'] * header['bytes_per_frame'] for r in timing):
        raise ValueError('monitor recording index does not match captured file')
    rate = header['rate']
    times = np.array([r['snapshot_us']/1e6 for r in timing])
    positions = np.array([r['write_bytes']/header['bytes_per_frame'] for r in timing])
    # Record-side clock equation in libpulse 16.1 stream.c, without transport
    # delay because timestamp belongs to the server snapshot, not its arrival.
    offsets = times - positions/rate - np.array([(r['source_us']-r['sink_us'])/1e6 for r in timing])
    audio = np.fromfile(d/'monitor.f32', dtype='<f4')
    on = audio > .1
    starts = np.flatnonzero(on & ~np.concatenate(([False], on[:-1])))
    # Resampling ringing can cross the threshold more than once at an edge.
    starts = starts[np.concatenate(([True], np.diff(starts) > rate*.1))]
    if len(starts) != len(tap['starts']):
        raise ValueError(f"pulse count: monitor={len(starts)}, tap={len(tap['starts'])}")
    to_wall, browser_clock = presentation_map(tap['clocks'])
    results=[]
    for observed, frame in zip(starts, tap['starts']):
        # Nearest server write-index snapshot is independent of the browser.
        k=int(np.argmin(abs(positions-observed)))
        gold=float(observed/rate+offsets[k])
        measured=float(to_wall(frame/tap['expected']['rate'])+injected_offset_ms/1000)
        results.append({'monitor_frame':int(observed),'tap_frame':frame,'gold_wall':gold,
                        'tap_wall':measured,'error_ms':(measured-gold)*1000,
                        'transport_us':timing[k]['transport_us']})
    error=np.array([r['error_ms'] for r in results])
    return {'gate':'independent-virtual-output','ok':bool(np.max(abs(error)) <= 1),
            'physical_output_calibrated':False,'max_absolute_error_ms':float(np.max(abs(error))),
            'median_error_ms':float(np.median(error)), 'browser_clock':browser_clock,
            'monitor_offset_span_ms':float(np.ptp(offsets)*1000),'pulses':results}


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--inject-offset-ms',type=float,default=0)
    args=parser.parse_args()
    result=analyse(args.directory,args.inject_offset_ms)
    print(json.dumps(result,indent=2))
    raise SystemExit(0 if result['ok'] else 1)
