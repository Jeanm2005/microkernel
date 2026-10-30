#!/usr/bin/env python3
"""Ping the guest over QEMU's length-prefixed Ethernet stream (no TAP/root)."""
import argparse
import re
import socket
import struct
import subprocess
import time
from pathlib import Path

HOST = bytes.fromhex('525400abcdef')
GUEST = bytes.fromhex('525400123456')
HOST_IP = bytes([10, 0, 2, 2])
GUEST_IP = bytes([10, 0, 2, 15])
COUNT = 260  # wraps both queue ring positions and reuses every RX descriptor


def checksum(data):
    if len(data) & 1:
        data += b'\0'
    total = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def ethernet(kind, body, destination=GUEST):
    return (destination + HOST + struct.pack('!H', kind) + body).ljust(60, b'\0')


def ping(seq, payload, destination=GUEST_IP, flags=0):
    icmp = struct.pack('!BBHHH', 8, 0, 0, 0x1234, seq) + payload
    icmp = icmp[:2] + struct.pack('!H', checksum(icmp)) + icmp[4:]
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(icmp), seq, flags,
                     64, 1, 0, HOST_IP, destination)
    ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
    return ethernet(0x800, ip + icmp)


def read_exact(sock, count):
    result = bytearray()
    while len(result) < count:
        part = sock.recv(count - len(result))
        if not part:
            raise AssertionError('QEMU network stream closed unexpectedly')
        result.extend(part)
    return bytes(result)


def receive(sock):
    size, = struct.unpack('!I', read_exact(sock, 4))
    assert 14 <= size <= 65536, f'invalid Ethernet stream length {size}'
    return read_exact(sock, size)


def send(sock, frame):
    sock.sendall(struct.pack('!I', len(frame)) + frame)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--iso', required=True)
    parser.add_argument('--disk', required=True)
    parser.add_argument('--log', required=True)
    args = parser.parse_args()
    host, guest = socket.socketpair()
    host.settimeout(3)
    path = Path(args.log)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('wb') as log:
        command = [args.qemu, '-M', 'q35', '-m', '256M', '-cpu', 'qemu64', '-smp', '1',
                   '-no-reboot', '-display', 'none', '-serial', 'stdio',
                   '-drive', f'file={args.disk},if=none,id=disk0,format=raw',
                   '-device', 'virtio-blk-pci,drive=disk0,disable-legacy=off',
                   '-netdev', f'socket,id=wire,fd={guest.fileno()}',
                   '-device', 'virtio-net-pci,netdev=wire,disable-modern=on,mac=52:54:00:12:34:56',
                   '-cdrom', args.iso, '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, pass_fds=(guest.fileno(),))
        guest.close()
        try:
            deadline = time.monotonic() + 15
            while b'[net] ready: 10.0.2.15' not in path.read_bytes():
                if proc.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError('guest never became network-ready')
                time.sleep(0.02)
            arp = struct.pack('!HHBBH6s4s6s4s', 1, 0x800, 6, 4, 1,
                              HOST, HOST_IP, bytes(6), GUEST_IP)
            send(host, ethernet(0x806, arp, b'\xff' * 6))
            response = receive(host)
            assert response[:14] == HOST + GUEST + b'\x08\x06'
            expected = struct.pack('!HHBBH6s4s6s4s', 1, 0x800, 6, 4, 2,
                                   GUEST, GUEST_IP, HOST, HOST_IP)
            assert response[14:42] == expected, 'invalid ARP reply'
            print('PASS net: ARP resolves 10.0.2.15')
            # All should be dropped without a reply. Checksums are valid
            # except where deliberately corrupted. Follow with a valid
            # echo: any unexpected reply makes its validation fail.
            invalid = [
                GUEST + HOST + b'\x08',  # truncated Ethernet
                ethernet(0x806, arp[:20]),  # truncated ARP (padding is zeros)
                ethernet(0x806, arp[:-4] + HOST_IP, b'\xff' * 6),
                ping(600, b'bad destination', HOST_IP),
                ping(601, b'fragment', flags=0x2000),
                ping(602, b'fragment offset', flags=1),
            ]
            bad = bytearray(ping(603, b'bad IP checksum')); bad[24] ^= 1
            invalid.append(bytes(bad))
            bad = bytearray(ping(604, b'bad ICMP checksum')); bad[36] ^= 1
            invalid.append(bytes(bad))
            bad = bytearray(ping(605, b'impossible total length'))
            bad[16:18] = b'\xff\xff'; bad[24:26] = b'\0\0'
            bad[24:26] = struct.pack('!H', checksum(bytes(bad[14:34])))
            invalid.append(bytes(bad))
            for packet in invalid:
                send(host, packet)
            for seq in range(COUNT):
                length = [0, 1, 31, 1472][seq % 4]
                payload = bytes((i + seq) & 255 for i in range(length))
                send(host, ping(seq, payload))
                response = receive(host)
                assert response[:14] == HOST + GUEST + b'\x08\x00'
                ip = response[14:34]
                assert ip[0] == 0x45 and checksum(ip) == 0 and ip[9] == 1
                assert ip[12:20] == GUEST_IP + HOST_IP
                size, = struct.unpack('!H', ip[2:4])
                assert size == 28 + len(payload)
                icmp = response[34:14 + size]
                assert checksum(icmp) == 0 and icmp[:2] == b'\0\0'
                assert struct.unpack('!HH', icmp[4:8]) == (0x1234, seq)
                assert icmp[8:] == payload, f'payload mismatch on echo {seq}'
            assert proc.wait(timeout=5) == 1, 'guest did not exit successfully'
            text = path.read_text()
            assert '[root] net exited with code 0' in text
            assert 'selftest: network ping completed; no frames leaked' in text
            match = re.search(r'ping ok: 260 echo replies, (\d+) frames, (\d+) dropped', text)
            assert match and int(match[2]) >= len(invalid), 'malformed frames not rejected'
            assert 'FAILED' not in text and 'PANIC' not in text
            print(f'PASS net: {COUNT} validated echoes (empty, odd, MTU-sized payloads; ring reuse)')
            print(f'PASS net: {len(invalid)} malformed/unsupported frames rejected; no frames leaked')
        except BaseException:
            print(path.read_text(errors='replace'))
            raise
        finally:
            host.close()
            if proc.poll() is None:
                proc.kill()
            proc.wait()


if __name__ == '__main__':
    main()
