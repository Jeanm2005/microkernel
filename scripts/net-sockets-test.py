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
    parser.add_argument("--guest-client", action="store_true")
    parser.add_argument("--restart", action="store_true")
    args = parser.parse_args()
    tcp_guest, udp_guest = (9001, 9000) if args.guest_client else (7, 7)
    tcp_port, udp_port = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    path = Path(args.log)
    with path.open('wb') as log:
        cmd = [args.qemu, '-M', 'q35', '-m', '256M', '-cpu', 'qemu64', '-smp', '1',
               '-no-reboot', '-display', 'none', '-serial', 'stdio',
               '-drive', f'file={args.disk},if=none,id=disk0,format=raw',
               '-device', 'virtio-blk-pci,drive=disk0,disable-legacy=off',
               '-netdev', f'user,id=net0,hostfwd=tcp:127.0.0.1:{tcp_port}-:{tcp_guest},'
                          f'hostfwd=udp:127.0.0.1:{udp_port}-:{udp_guest}',
               '-device', 'virtio-net-pci,netdev=net0,disable-modern=on,mac=52:54:00:12:34:56',
               '-cdrom', args.iso, '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        clients = []
        try:
            end = time.monotonic() + 15
            ready = b'[netsock] ready:' if args.guest_client else b'[net] ready:'
            while ready not in path.read_bytes():
                assert proc.poll() is None and time.monotonic() < end, 'guest not ready'
                time.sleep(0.02)
            if args.guest_client:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.settimeout(3); sock.sendto(b'guest UDP IPC', ('127.0.0.1', udp_port))
                    assert sock.recv(2048) == b'guest UDP IPC'
                with socket.create_connection(('127.0.0.1', tcp_port), timeout=3) as sock:
                    sock.settimeout(5)
                    payload = bytes(i & 255 for i in range(1025))
                    sock.sendall(payload); sock.shutdown(socket.SHUT_WR)
                    answer = bytearray()
                    while True:
                        chunk = sock.recv(2048)
                        if not chunk: break
                        answer.extend(chunk)
                    assert answer == payload, (len(answer), len(payload))
                assert proc.wait(timeout=5) == 1
                text = path.read_text()
                assert 'selftest: guest sockets and network recovery ok; no frames leaked' in text
                assert '[netsock] done, 0 failure(s)' in text
                if args.restart:
                    assert 'old socket handles cannot alias the restarted server' in text
                    assert 'network restarted once with revoked old grants' in text
                    assert 'SUPERVISOR STOP' in text
                print('PASS guest: UDP/TCP socket IPC, badge isolation, clean shutdown and no frame leaks')
                if args.restart: print('PASS guest: timed call cancellation, blocked-server stop, grant revocation and reconnect')
                return
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
