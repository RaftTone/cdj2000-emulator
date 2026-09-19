"""Audit bounded stereo S16 PCM bank observations against WAV and MAIN writes.

Observed contract only: two banks, four 588-frame blocks, metadata at +3c00.
No inference of concurrent hardware ownership or downstream release.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import wave

BASE = 0x118381e0
STRIDE = 0x3cc0

def digest(path):
    # hashlib.file_digest is Python 3.11+; BUILD.md promises 3.10.
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()

def report(run, wav, events):
    gate = json.loads((run/'gate.json').read_text())
    manifest = json.loads((run/'manifest.json').read_text())
    if (not gate.get('passed') or not gate.get('repeat_matches')
            or not gate.get('final_state_and_memory_match')
            or digest(run/'trace.jsonl') != gate['trace_sha256']
            or digest(events) != manifest['event_transcript_sha256']):
        raise ValueError('trace/event provenance mismatch or unverified replay')
    with wave.open(str(wav)) as w:
        if (w.getnchannels(),w.getsampwidth(),w.getframerate()) != (2,2,44100):
            raise ValueError('requires stereo S16 44100 Hz WAV')
        pcm = w.readframes(w.getnframes())
    calls=[]; pending=None; observations=0
    for line in (run/'trace.jsonl').open():
        e=json.loads(line)
        if e['event']!='pcm_observation': continue
        observations+=1
        if e['pc']==0xc003c698:
            if pending is not None: raise ValueError('nested observed call')
            if e['b4'] not in (0,1) or e['a6'] not in range(4) or e['b6']!=2:
                raise ValueError('unsupported PCM bank/block/class')
            pending=e
        elif e['pc']==0xc003c398:
            if pending is None or e['a6']!=1176: raise ValueError('unexpected kernel')
            pending['kernel']=e
        elif e.get('return_observation'):
            if pending is None or 'kernel' not in pending: raise ValueError('unpaired return')
            k=pending['kernel']; bank=pending['b4']; block=pending['a6']
            if k['raw_banks']['address']!=BASE: raise ValueError('raw base changed')
            raw=bytes.fromhex(k['raw_banks']['hex'])
            start=bank*STRIDE+block*2352
            if k['a4']!=BASE+start: raise ValueError('unexpected input stride')
            metadata=struct.unpack_from('<4I',raw,bank*STRIDE+0x3c00+block*16)
            ordinal=metadata[0]
            source=pcm[ordinal*2352:(ordinal+1)*2352]
            if len(source)!=2352: raise ValueError('metadata exceeds source')
            samples=struct.unpack('<1176h',source)
            matches=[bytes.fromhex(e[f'output_plane{c}']['hex'])==
                     struct.pack('<588f',*(s/32768 for s in samples[c::2])) for c in range(2)]
            calls.append(dict(bank=bank,block=block,metadata=list(metadata),
                input_address=hex(k['a4']),input_matches=raw[start:start+2352]==source,
                output_matches=matches,entry_packet=pending['packets'],return_packet=e['packets']))
            pending=None
    if not calls: raise ValueError('no complete calls')
    writes=[]
    for line in events.open():
        e=json.loads(line); a=e.get('address',0)
        if 'host_data' in e['event'] and 'write' in e['event'] and BASE<=a<BASE+2*STRIDE:
            writes.append(e)
    epochs=[]; last_end={0:0,1:0}
    for call in calls:
        bank=call['bank']
        if call['block']==0:
            lower=last_end[bank]; upper=call['entry_packet']
            selected=[e for e in writes if lower<e['packets']<upper
                      and BASE+bank*STRIDE<=e['address']<BASE+(bank+1)*STRIDE]
            # Only the most recent payload/metadata writes are relevant on the first use.
            by_address={e['address']:e for e in selected}
            payload=[e for a,e in by_address.items() if a<BASE+bank*STRIDE+9408]
            meta=[e for a,e in by_address.items() if BASE+bank*STRIDE+0x3c00<=a<BASE+bank*STRIDE+0x3c40]
            epochs.append(dict(bank=bank,first_metadata_ordinal=call['metadata'][0],
                previous_same_bank_return_packet=lower,entry_packet=upper,
                payload_bytes=len(payload)*4,metadata_bytes=len(meta)*4,
                first_write_sequence=min((e['sequence'] for e in payload+meta),default=None),
                last_write_sequence=max((e['sequence'] for e in payload+meta),default=None)))
        last_end[bank]=call['return_packet']
        call['host_writes_during_call']=sum(call['entry_packet']<=e['packets']<=call['return_packet']
            and BASE+bank*STRIDE<=e['address']<BASE+(bank+1)*STRIDE for e in writes)
    return dict(scope='verified recorded scheduling only; no concurrent hardware lifetime proof',
        trace_sha256=gate['trace_sha256'],event_sha256=digest(events),wav_sha256=digest(wav),
        verified_connected_stops=gate['verified_connected_stops'],observations=observations,
        incomplete_observer_tail=pending is not None,complete_calls=len(calls),
        frames=len(calls)*588,calls=calls,epochs=epochs,
        passed=all(c['input_matches'] and all(c['output_matches']) and not c['host_writes_during_call'] for c in calls)
               and all(e['payload_bytes']==9408 and e['metadata_bytes']==64 for e in epochs))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('run',type=Path);p.add_argument('wav',type=Path);p.add_argument('events',type=Path)
    a=p.parse_args();r=report(a.run,a.wav,a.events)
    print(json.dumps(r,indent=2));raise SystemExit(0 if r['passed'] else 1)
