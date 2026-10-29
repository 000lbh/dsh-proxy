#!/usr/bin/env python3
"""Minimal WebSocket client used by tests/test_proxy.sh."""

import base64
import os
import socket
import sys


def recv_exact(sock, length):
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise RuntimeError("socket closed")
        data += chunk
    return data


def main():
    if len(sys.argv) != 4:
        print("usage: ws_check.py HOST PORT PATH", file=sys.stderr)
        return 2
    host = sys.argv[1]
    port = int(sys.argv[2])
    path = sys.argv[3]
    sock = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(
        (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "\r\n"
        ).encode()
    )
    response = b""
    while b"\r\n\r\n" not in response:
        response += sock.recv(4096)
    if b"101 Switching Protocols" not in response.split(b"\r\n", 1)[0]:
        print(response.decode("latin-1"), file=sys.stderr)
        return 1

    payload = b"hello"
    mask = os.urandom(4)
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + masked)

    header = recv_exact(sock, 2)
    length = header[1] & 0x7F
    if length == 126:
        length = int.from_bytes(recv_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(recv_exact(sock, 8), "big")
    echoed = recv_exact(sock, length)
    if echoed != b"echo:hello":
        print(f"unexpected websocket payload: {echoed!r}", file=sys.stderr)
        return 1
    sock.sendall(bytes([0x88, 0x80]) + os.urandom(4))
    sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
