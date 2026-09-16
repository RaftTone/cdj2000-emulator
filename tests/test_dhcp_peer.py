import struct
import pytest
from tools.cdj_main.dhcp_peer import DhcpPeer, options
from tools.cdj_main.ethernet_peer import MAC, checksum

SERVER = bytes([192,168,42,1])
LEASE = bytes([192,168,42,2])
CLIENT = bytes.fromhex('000000000001')

def packet(kind=1, extra=b'', xid=1, broadcast=True, client=CLIENT):
    boot = bytearray(240)
    boot[:4] = bytes([1,1,6,0])
    boot[4:8] = xid.to_bytes(4, 'big')
    boot[10] = 128 if broadcast else 0
    boot[28:34] = client
    boot[236:240] = bytes.fromhex('63825363')
    boot += bytes([53,1,kind,61,7,1]) + client + extra + b'\xff'
    udp = bytearray(struct.pack('!HHHH',68,67,len(boot)+8,0) + boot)
    pseudo = bytes(4) + b'\xff'*4 + b'\x00\x11' + udp[4:6]
    struct.pack_into('!H',udp,6,checksum(pseudo+udp) or 65535)
    ip = bytearray(struct.pack('!BBHHHBBH4s4s',69,0,len(udp)+20,0,0,64,17,0,bytes(4),b'\xff'*4))
    struct.pack_into('!H',ip,10,checksum(ip))
    return b'\xff'*6 + client + b'\x08\x00' + ip + udp

@pytest.mark.parametrize('broadcast',[True,False])
def test_offer_ack(broadcast):
    peer = DhcpPeer('192.168.42.1','192.168.42.2')
    for kind, expected in [(1,2),(3,5)]:
        extra = bytes([54,4])+SERVER+bytes([50,4])+LEASE if kind==3 else b''
        response = peer.respond(packet(kind,extra,broadcast=broadcast))
        assert response[:6] == (b'\xff'*6 if broadcast else CLIENT)
        assert response[6:12] == MAC
        assert checksum(response[14:34]) == 0
        assert checksum(response[26:34]+b'\x00\x11'+response[38:40]+response[34:]) == 0
        assert response[34:38] == bytes.fromhex('00430044')
        assert response[58:62] == LEASE
        opts = options(response[282:])
        assert opts[53] == bytes([expected])
        assert opts[51] == b'\xff'*4
        assert opts[1] == bytes.fromhex('ffffff00')
        assert 3 not in opts and 6 not in opts
    assert [e['reply'] for e in peer.events] == ['offer','ack']

def test_request_must_match_offer():
    peer = DhcpPeer('192.168.42.1','192.168.42.2')
    extra = bytes([54,4])+SERVER+bytes([50,4])+LEASE
    assert peer.respond(packet(3,extra)) is None
    assert peer.respond(packet())
    assert peer.respond(packet(3,extra,xid=2)) is None
    assert peer.respond(packet(3,extra[:-1]+b'\x03')) is None
    assert peer.respond(packet(client=bytes.fromhex('020000000003'))) is None
    assert peer.respond(packet(3,extra))

@pytest.mark.parametrize('data',[b'',b'\x35',b'\x35\x02\x01',b'\x35\x01\x01\x35\x01\x01\xff',b'\x34\x01\x01\xff'])
def test_bad_options(data):
    assert options(data) is None

@pytest.mark.parametrize('offset',[14,24,34,38,40,42,70,278])
def test_bad_frame(offset):
    frame=bytearray(packet()); frame[offset]^=1
    assert DhcpPeer('192.168.42.1','192.168.42.2').respond(frame) is None

@pytest.mark.parametrize('lease',['192.168.42.1','192.168.42.0','192.168.42.255','192.168.43.2'])
def test_bad_lease(lease):
    with pytest.raises(ValueError): DhcpPeer('192.168.42.1',lease)
