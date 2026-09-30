"""Inspect exact null-sink render deadlines, retaining rewinds and uncertainty."""
import json
from pathlib import Path
import numpy as np


def segments(directory):
    d=Path(directory)
    if (d/'sink.jsonl').stat().st_size > 16_000_000 or (d/'sink.f32').stat().st_size > 400_000_000:
        raise ValueError('sink trace exceeds capture bound')
    events=[json.loads(line) for line in (d/'sink.jsonl').read_text().splitlines()]
    if len(events) < 3 or not (d/'sink.f32').stat().st_size:
        raise ValueError('empty sink trace')
    if (d/'sink.f32').stat().st_size % 8:
        raise ValueError('partial stereo frame in trace PCM')
    if events[-1].get('ev')!='end' or events[-1].get('dropped'):
        raise ValueError('sink trace incomplete or overflowed')
    if events[0] != {'ev':'start','pulse_version':'16.1','rate':48000,'channels':2,'format':'float32le'}:
        raise ValueError('unexpected trace format')
    raw=np.memmap(d/'sink.f32',dtype='<f4',mode='r')
    result=[]
    def truncate(t):
        while result and result[-1]['time']>=t:
            result.pop()
        if result:
            last=result[-1]
            count=max(0,min(len(last['audio']),round((t-last['time'])*48000)))
            last['audio']=last['audio'][:count]
    uncertainty=[]
    file_offset=0
    clock_offset=None
    for event in events[1:-1]:
        if not 0 <= event['bracket_ns'] <= 50000:
            raise ValueError('sink clock observation bracket exceeds 50 us')
        if event['file_offset'] != file_offset:
            raise ValueError('non-contiguous trace file offset')
        if event['bytes'] < 0 or event['bytes'] % 8:
            raise ValueError('invalid trace frame size')
        offset=event['real_ns']-event['mono_ns']
        if clock_offset is None:
            clock_offset=offset
        elif abs(offset-clock_offset)>100_000:
            raise ValueError('realtime/monotonic clock mapping changed by more than 100 us')
        uncertainty.append(event['bracket_ns'])
        mono=event['t0_mono_ns']/1e9
        if event['ev']=='rewind':
            truncate(mono)
            continue
        if event['ev']!='render' or event['bytes']%8:
            raise ValueError('invalid render event')
        truncate(mono)
        first=event['file_offset']//4
        data=raw[first:first+event['bytes']//4]
        if len(data)*4 != event['bytes']:
            raise ValueError('truncated trace PCM')
        if not np.all(np.isfinite(data)):
            raise ValueError('non-finite trace PCM')
        file_offset += event['bytes']
        result.append({'time':mono,'wall_offset':(event['real_ns']-event['mono_ns'])/1e9,
                       'audio':data.reshape(-1,2).mean(axis=1)})
    if file_offset != raw.nbytes:
        raise ValueError('unaccounted trace PCM')
    return result, max(uncertainty,default=0)/1000


def pulse_times(directory):
    chunks, uncertainty=segments(directory)
    starts=[]
    previous=False
    end=None
    for chunk in chunks:
        audio=chunk['audio'];t=chunk['time']
        if not len(audio):continue
        if end is not None and abs(t-end)>.0001:previous=False
        on=audio>.1
        indices=np.flatnonzero(on & ~np.concatenate(([previous],on[:-1])))
        starts.extend(float(t+i/48000+chunk['wall_offset']) for i in indices)
        previous=bool(on[-1]);end=t+len(audio)/48000
    return starts,uncertainty


if __name__=='__main__':
    import argparse,sys
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'analysis'))
    from latency import presentation_map
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args()
    observed,uncertainty=pulse_times(args.directory)
    tap=json.loads((args.directory/'tap.json').read_text())
    failure=None
    try: mapping,clock=presentation_map(tap['clocks'])
    except ValueError as error:
        failure=str(error)
        clocks=[c for c in tap['clocks'] if c.get('contextTime',0)>0 and c.get('performanceTime',0)>0]
        x=[c['contextTime'] for c in clocks];y=[(c['timeOrigin']+c['performanceTime'])/1000 for c in clocks]
        mapping=lambda t:float(np.interp(t,x,y));clock={'diagnostic_only':True}
    if len(observed)!=len(tap['starts']):raise ValueError(f'pulses trace {len(observed)}, tap {len(tap["starts"])}')
    rows=[{'gold_wall':gold,'tap_wall':mapping(frame/tap['expected']['rate']),
           'error_ms':(mapping(frame/tap['expected']['rate'])-gold)*1000}
          for gold,frame in zip(observed,tap['starts'])]
    maximum=max(abs(r['error_ms']) for r in rows)
    result={'gate':'null-sink-trace','ok':failure is None and maximum<=1,
            'clock_error':failure,'observation_bracket_max_us':uncertainty,
            'max_error_ms':maximum,'median_error_ms':float(np.median([r['error_ms'] for r in rows])),
            'clock':clock,'pulses':rows}
    print(json.dumps(result,indent=2))
    raise SystemExit(0 if result['ok'] else 1)
