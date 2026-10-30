"""Packet-level transport tests; shared by the QEMU Ethernet harness."""
import struct


def run(sock, send, receive, ethernet, checksum, host_ip, guest_ip):
    def wire(proto, segment):
        ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(segment), 0, 0,
                         64, proto, 0, host_ip, guest_ip)
        ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
        return ethernet(0x800, ip + segment)

    def signed(proto, segment, offset):
        pseudo = host_ip + guest_ip + struct.pack('!BBH', 0, proto, len(segment))
        value = checksum(pseudo + segment)
        return segment[:offset] + struct.pack('!H', value or 65535) + segment[offset + 2:]

    def unpack(proto):
        frame = receive(sock)
        assert frame[12:14] == b'\x08\x00'
        ip = frame[14:34]
        assert ip[0] == 0x45 and ip[9] == proto and checksum(ip) == 0
        assert ip[12:20] == guest_ip + host_ip
        total, = struct.unpack('!H', ip[2:4])
        segment = frame[34:14 + total]
        pseudo = guest_ip + host_ip + struct.pack('!BBH', 0, proto, len(segment))
        assert checksum(pseudo + segment) == 0
        return segment

    def udp(payload, zero=False):
        segment = struct.pack('!HHHH', 30000, 7, 8 + len(payload), 0) + payload
        return wire(17, segment if zero else signed(17, segment, 6))

    for length in (0, 1, 31, 1472):
        payload = bytes(i & 255 for i in range(length))
        send(sock, udp(payload))
        result = unpack(17)
        assert struct.unpack('!HHH', result[:6]) == (7, 30000, 8 + length)
        assert result[8:] == payload and result[6:8] != b'\0\0'
    send(sock, udp(b'IPv4 permits an omitted UDP checksum', zero=True))
    assert unpack(17)[8:] == b'IPv4 permits an omitted UDP checksum'
    # Invalid transport checksums/lengths must not elicit replies.
    bad = bytearray(udp(b'bad checksum')); bad[40] ^= 1
    send(sock, bytes(bad))
    segment = struct.pack('!HHHH', 30000, 7, 9, 0) + b'wrong length'
    send(sock, wire(17, signed(17, segment, 6)))
    send(sock, udp(b'after malformed UDP'))
    assert unpack(17)[8:] == b'after malformed UDP'
    print('PASS transport: UDP lengths, binary payloads, omitted checksum and malformed rejection')

    def tcp(seq, ack=0, flags=2, payload=b'', port=31000, destination=7):
        segment = struct.pack('!HHIIBBHHH', port, destination, seq & 0xffffffff,
                              ack & 0xffffffff, 5 << 4, flags, 4096, 0, 0) + payload
        return wire(6, signed(6, segment, 16))

    def recv_tcp():
        t = unpack(6)
        header = (t[12] >> 4) * 4
        assert header >= 20 and header <= len(t)
        seq, ack = struct.unpack('!II', t[4:12])
        return seq, ack, t[13], t[header:], struct.unpack('!H', t[14:16])[0]

    bad = bytearray(tcp(100)); bad[50] ^= 1
    send(sock, bytes(bad))
    invalid_header = struct.pack('!HHIIBBHHH', 31000, 7, 100, 0, 4 << 4, 2, 4096, 0, 0)
    send(sock, wire(6, signed(6, invalid_header, 16)))
    invalid_options = struct.pack('!HHIIBBHHH', 31000, 7, 100, 0, 6 << 4, 2, 4096, 0, 0)
    invalid_options += b'\x02\x00\x00\x00'
    send(sock, wire(6, signed(6, invalid_options, 16)))
    send(sock, tcp(100, destination=9999))
    _, ack, flags, _, _ = recv_tcp()
    assert flags == 20 and ack == 101, 'closed TCP port must reset'
    host_seq = 0xfffffff0
    send(sock, tcp(host_seq))
    syn = recv_tcp()
    assert syn[1] == host_seq + 1 and syn[2] == 18
    assert recv_tcp() == syn, 'lost SYN-ACK was not retransmitted'
    guest_seq = syn[0] + 1
    host_seq += 1
    send(sock, tcp(host_seq, guest_seq, 16))
    assert recv_tcp()[4] > 0
    payload = bytes(range(33))  # crosses the 32-bit receive sequence wrap
    send(sock, tcp(host_seq, guest_seq, 24, payload))
    echoed = recv_tcp()
    next_host = (host_seq + len(payload)) & 0xffffffff
    assert echoed[:4] == (guest_seq, next_host, 24, payload)
    assert recv_tcp() == echoed, 'lost echo was not retransmitted'
    # A duplicate must acknowledge progress without echoing the bytes twice.
    send(sock, tcp(host_seq, guest_seq, 24, payload))
    duplicate = recv_tcp()
    assert duplicate[1] == next_host and not duplicate[3]
    next_guest = (guest_seq + len(payload)) & 0xffffffff
    send(sock, tcp(next_host + 10, next_guest, 24, b'out of order'))
    assert recv_tcp()[1] == next_host
    send(sock, tcp(next_host, next_guest + 50, 16))
    assert recv_tcp()[1] == next_host
    send(sock, tcp(next_host + 1, 0, 4))
    assert recv_tcp()[1] == next_host, 'inexact RST destroyed a live connection'
    send(sock, tcp(next_host, next_guest, 16))
    assert recv_tcp()[4] > 0
    send(sock, tcp(next_host, next_guest, 17))
    finish = recv_tcp()
    assert finish[2] == 17 and finish[1] == (next_host + 1) & 0xffffffff
    assert recv_tcp() == finish, 'lost FIN was not retransmitted'
    send(sock, tcp(next_host + 1, finish[0] + 1, 16))
    print('PASS transport: TCP handshake, retransmission, duplicate/order checks, sequence wrap and FIN')
    # Exhaust the fixed table, then prove RST releases the slots.
    for i in range(4):
        send(sock, tcp(1000 + i, port=32000 + i))
        assert recv_tcp()[2] == 18
    send(sock, tcp(2000, port=33000))  # fifth SYN is dropped
    for i in range(4):
        send(sock, tcp(1001 + i, flags=4, port=32000 + i))
    send(sock, tcp(2000, port=33000))
    result = recv_tcp()
    assert result[1] == 2001 and result[2] == 18
    send(sock, tcp(2001, flags=4, port=33000))
    print('PASS transport: bounded TCP connection table and slot reclamation')
