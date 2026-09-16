"""Read-only directional packet inventory; ports are observations, not protocol proof."""
import argparse
from collections import Counter
import hashlib
import ipaddress
import json
from pathlib import Path
import struct

from tools.cdj_main.ethernet_peer import checksum


def classify(frame):
    if len(frame) < 14:
        raise ValueError('short Ethernet header')
    result = {'ethertype': frame[12:14].hex()}
    if frame[12:14] != b'\x08\x00':
        return result
    ip = frame[14:]
    if len(ip) < 20 or ip[0] >> 4 != 4:
        raise ValueError('short or invalid IPv4 header')
    h = (ip[0] & 15) * 4
    size = int.from_bytes(ip[2:4], 'big')
    if not 20 <= h <= size <= len(ip) or checksum(ip[:h]):
        raise ValueError('invalid IPv4 length/checksum')
    result.update(source=str(ipaddress.IPv4Address(ip[12:16])),
                  destination=str(ipaddress.IPv4Address(ip[16:20])), protocol=ip[9])
    if int.from_bytes(ip[6:8], 'big') & 0x3fff:
        result['fragmented'] = True
        return result
    if ip[9] == 17:
        udp = ip[h:size]
        if len(udp) < 8:
            raise ValueError('short UDP header')
        source, destination, length, check = struct.unpack('!HHHH', udp[:8])
        if length != len(udp):
            raise ValueError('invalid UDP length')
        pseudo = ip[12:20] + struct.pack('!BBH', 0, 17, length)
        if check and checksum(pseudo + udp):
            raise ValueError('invalid UDP checksum')
        result.update(source_port=source, destination_port=destination)
    return result


def inventory(path):
    digest = hashlib.sha256()
    counts = Counter()
    errors = []
    records = 0
    with Path(path).open('rb') as stream:
        for number, line in enumerate(stream, 1):
            digest.update(line)
            records += 1
            try:
                record = json.loads(line)
                direction = record['direction']
                if direction not in ('guest-to-peer', 'peer-to-guest'):
                    raise ValueError('invalid direction')
                fields = classify(bytes.fromhex(record['hex']))
                fields['direction'] = direction
                counts[json.dumps(fields, sort_keys=True)] += 1
            except (ValueError, KeyError, TypeError) as error:
                errors.append({'line': number, 'error': str(error)})
    return {'capture_sha256': digest.hexdigest(), 'records': records,
            'flows': [dict(json.loads(key), count=count)
                      for key, count in sorted(counts.items())],
            'invalid_records': errors,
            'limitations': 'Port inventory only; no PTP lock, subscriptions, audio, '
                           'guest timing or firmware reception inferred.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    report = inventory(args.capture)
    print(json.dumps(report, indent=2))
    return bool(report['invalid_records'])


if __name__ == '__main__':
    raise SystemExit(main())
