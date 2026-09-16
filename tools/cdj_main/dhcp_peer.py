"""Single-client isolated DHCP fixture, RFC 2131/2132; not a LAN server.

Supports initial SELECTING only. One reserved /24 address, infinite lease
avoids pretending host time is guest lease time. No relay, renewal, option
overload, routing, DNS, or physical network access. Unsupported input is silent.
"""
import ipaddress
import struct

from tools.cdj_main.ethernet_peer import MAC, checksum


def options(data):
    result = {}
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 255:
            return result
        if code == 0:
            continue
        if i >= len(data):
            return None
        n = data[i]
        i += 1
        if i + n > len(data) or code in result or code == 52:
            return None
        result[code] = data[i:i+n]
        i += n
    return None


class DhcpPeer:
    def __init__(self, server, lease):
        server, lease = ipaddress.IPv4Address(server), ipaddress.IPv4Address(lease)
        network = ipaddress.IPv4Network(f'{server}/24', strict=False)
        if (server.is_multicast or server.is_unspecified or lease not in network
                or server == lease or server in (network.network_address, network.broadcast_address)
                or lease in (network.network_address, network.broadcast_address)):
            raise ValueError('server and lease must be distinct unicast hosts in one /24')
        self.server, self.lease = server.packed, lease.packed
        self.client = None
        self.xid = None
        self.events = []

    def respond(self, frame):
        if not 282 <= len(frame) <= 1514 or frame[12:14] != b'\x08\x00':
            return None
        if frame[:6] not in (MAC, b'\xff'*6):
            return None
        src = frame[6:12]
        ip = frame[14:]
        size = int.from_bytes(ip[2:4], 'big')
        if (ip[0] != 0x45 or ip[9] != 17 or not 268 <= size <= len(ip)
                or checksum(ip[:20]) or int.from_bytes(ip[6:8], 'big') & 0xbfff
                or ip[8] == 0 or ip[12:16] != bytes(4)
                or ip[16:20] not in (self.server, b'\xff'*4)):
            return None
        udp = ip[20:size]
        if udp[:4] != bytes.fromhex('00440043') or int.from_bytes(udp[4:6], 'big') != len(udp):
            return None
        pseudo = ip[12:20] + b'\x00\x11' + udp[4:6]
        if udp[6:8] != bytes(2) and checksum(pseudo + udp):
            return None
        boot = udp[8:]
        if (boot[:4] != b'\x01\x01\x06\x00' or boot[12:28] != bytes(16)
                or boot[28:34] != src or src[0] & 1 or src == bytes(6)
                or boot[236:240] != bytes.fromhex('63825363')
                or int.from_bytes(boot[10:12], 'big') & 0x7fff):
            return None
        opts = options(boot[240:])
        if opts is None or opts.get(53) not in (b'\x01', b'\x03'):
            return None
        identity = (src, opts.get(61))
        if 61 in opts and len(opts[61]) < 2:
            return None
        if self.client is not None and identity != self.client:
            return None
        xid = boot[4:8]
        if opts[53] == b'\x01':
            if 54 in opts or (50 in opts and len(opts[50]) != 4):
                return None
            kind = 2
        else:
            if (identity != self.client or xid != self.xid
                    or opts.get(54) != self.server or opts.get(50) != self.lease):
                return None
            kind = 5
        self.client, self.xid = identity, xid
        reply = bytearray(240)
        reply[:3] = b'\x02\x01\x06'
        reply[4:8] = xid
        reply[10:12] = boot[10:12]
        reply[16:20] = self.lease
        reply[28:44] = boot[28:44]
        reply[236:240] = boot[236:240]
        reply += bytes([53, 1, kind, 54, 4]) + self.server
        reply += bytes.fromhex('3304ffffffff0104ffffff00')
        if 61 in opts:
            reply += bytes([61, len(opts[61])]) + opts[61]
        reply += b'\xff'
        reply = reply.ljust(300, b'\0')
        broadcast = bool(boot[10] & 0x80)
        destination = b'\xff'*4 if broadcast else self.lease
        segment = bytearray(struct.pack('!HHHH', 67, 68, len(reply)+8, 0) + reply)
        pseudo = self.server + destination + b'\x00\x11' + segment[4:6]
        struct.pack_into('!H', segment, 6, checksum(pseudo + segment) or 0xffff)
        header = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20+len(segment),
                                      0, 0, 64, 17, 0, self.server, destination))
        struct.pack_into('!H', header, 10, checksum(header))
        self.events.append(dict(request='discover' if kind == 2 else 'request',
                                reply='offer' if kind == 2 else 'ack', xid=xid.hex()))
        return (b'\xff'*6 if broadcast else src) + MAC + b'\x08\x00' + header + segment
