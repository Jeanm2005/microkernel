#!/usr/bin/env python3
"""Exercise the persistent guest with ordinary host TCP and UDP sockets."""
import argparse
import socket
import subprocess
import time
from pathlib import Path


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--iso', required=True)
    parser.add_argument('--disk', required=True)
    parser.add_argument('--log', required=True)
    args = parser.parse_args()
    tcp_port, udp_port = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    path = Path(args.log)
    with path.open('wb') as log:
        cmd = [args.qemu, '-M', 'q35', '-m', '256M', '-cpu', 'qemu64', '-smp', '1',
               '-no-reboot', '-display', 'none', '-serial', 'stdio',
               '-drive', f'file={args.disk},if=none,id=disk0,format=raw',
               '-device', 'virtio-blk-pci,drive=disk0,disable-legacy=off',
               '-netdev', f'user,id=net0,hostfwd=tcp:127.0.0.1:{tcp_port}-:7,'
                          f'hostfwd=udp:127.0.0.1:{udp_port}-:7',
               '-device', 'virtio-net-pci,netdev=net0,disable-modern=on,mac=52:54:00:12:34:56',
               '-cdrom', args.iso]
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        clients = []
        try:
            end = time.monotonic() + 15
            while b'[net] ready:' not in path.read_bytes():
                assert proc.poll() is None and time.monotonic() < end, 'guest not ready'
                time.sleep(0.02)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.settimeout(3)
                for length in (0, 1, 31, 1472):
                    payload = bytes(i & 255 for i in range(length))
                    sock.sendto(payload, ('127.0.0.1', udp_port))
                    answer, _ = sock.recvfrom(2048)
                    assert answer == payload
            print('PASS sockets: UDP echo through native host sockets')
            for _ in range(3):
                sock = socket.create_connection(('127.0.0.1', tcp_port), timeout=5)
                sock.settimeout(5)
                clients.append(sock)
            for index, sock in enumerate(clients):
                payload = bytes((i + index) & 255 for i in range(32768 + index))
                sock.sendall(payload)
                result = bytearray()
                while len(result) < len(payload):
                    chunk = sock.recv(8192)
                    assert chunk, 'premature TCP EOF'
                    result.extend(chunk)
                assert result == payload
                sock.shutdown(socket.SHUT_WR)
                assert sock.recv(1) == b'', 'FIN not delivered'
                sock.close()
            clients.clear()
            # Keep serving beyond the former ten-second demo lifetime.
            time.sleep(11)
            with socket.create_connection(('127.0.0.1', tcp_port), timeout=3) as sock:
                sock.settimeout(3)
                sock.sendall(b'still serving\n')
                assert sock.recv(1024) == b'still serving\n'
            assert proc.poll() is None and b'FAILED' not in path.read_bytes()
            print('PASS sockets: TCP binary streams, concurrent sessions, half-close, persistent service')
        except BaseException:
            print(path.read_text(errors='replace'))
            raise
        finally:
            for sock in clients:
                sock.close()
            proc.kill()
            proc.wait()


if __name__ == '__main__':
    main()
