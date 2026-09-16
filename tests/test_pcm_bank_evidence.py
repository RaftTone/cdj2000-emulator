import hashlib
import json
import struct
import wave
import pytest
from tools.cdj_dsp.pcm_bank_evidence import BASE, STRIDE, report

@pytest.mark.parametrize('missing_write', [False, True])
def test_bank1_metadata_and_write_coverage(tmp_path, missing_write):
    samples=[x for i in range(8*588) for x in (i,-i)]
    pcm=struct.pack('<%dh'%len(samples),*samples)
    wav=tmp_path/'source.wav'
    with wave.open(str(wav),'wb') as w:
        w.setparams((2,2,44100,0,'NONE','not compressed'));w.writeframes(pcm)
    raw=bytearray(STRIDE*2)
    raw[STRIDE:STRIDE+9408]=pcm[4*2352:8*2352]
    for i in range(4): struct.pack_into('<4I',raw,STRIDE+0x3c00+i*16,4+i,1,0,0)
    block_samples=samples[4*1176:5*1176]
    rows=[dict(event='pcm_observation',pc=0xc003c698,b4=1,a6=0,b6=2,packets=20),
          dict(event='pcm_observation',pc=0xc003c398,a4=BASE+STRIDE,a6=1176,
               raw_banks=dict(address=BASE,hex=raw.hex())),
          dict(event='pcm_observation',pc=0xc0049e6c,return_observation=True,packets=30)]
    for channel in (0,1):
        rows[-1][f'output_plane{channel}']=dict(hex=struct.pack('<588f',
            *(s/32768 for s in block_samples[channel::2])).hex())
    # Explicitly incomplete observer tail is reported but cannot add a verified call.
    rows.append(dict(event='pcm_observation',pc=0xc003c698,b4=1,a6=1,b6=2,packets=40))
    trace=tmp_path/'trace.jsonl';trace.write_text(''.join(json.dumps(e)+'\n' for e in rows))
    writes=[]
    for offset in list(range(0,9408,4))+list(range(0x3c00,0x3c40,4)):
        writes.append(dict(event='hpi_host_data_fixed_write',packets=10,
                           sequence=len(writes)+1,address=BASE+STRIDE+offset))
    if missing_write: writes.pop()
    events=tmp_path/'events.jsonl';events.write_text(''.join(json.dumps(e)+'\n' for e in writes))
    (tmp_path/'gate.json').write_text(json.dumps(dict(passed=True,repeat_matches=True,
        final_state_and_memory_match=True,trace_sha256=hashlib.sha256(trace.read_bytes()).hexdigest(),
        verified_connected_stops=1)))
    (tmp_path/'manifest.json').write_text(json.dumps(dict(
        event_transcript_sha256=hashlib.sha256(events.read_bytes()).hexdigest())))
    result=report(tmp_path,wav,events)
    assert result['passed'] is not missing_write
    assert result['complete_calls']==1 and result['incomplete_observer_tail']
    assert result['calls'][0]['metadata']==[4,1,0,0]
    assert result['calls'][0]['output_matches']==[True,True]
    assert result['epochs'][0]['payload_bytes']==9408
    events.write_text(events.read_text()+'\n')
    with pytest.raises(ValueError,match='provenance'): report(tmp_path,wav,events)
