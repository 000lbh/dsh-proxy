#!/usr/bin/env python3
"""A tiny fake `dsh web` used by tests/test_proxy.sh.

It prints the same token URL line as dsh, can ask an install/update question on
stdin, and serves just enough HTTP/WebSocket behavior to test the proxy.
"""

import argparse
import base64
import hashlib
import json
import os
import secrets
import socket
import socketserver
import sys
import time
import urllib.parse

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
COOKIE = "dsh-auth-mock=testsecret"


def recv_until(sock, marker):
    data = b""
    while marker not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        if len(data) > 65536:
            break
    return data


class BufferedReader:
    def __init__(self, sock, initial=b""):
        self.sock = sock
        self.buffer = initial

    def read(self, length):
        while len(self.buffer) < length:
            chunk = self.sock.recv(max(4096, length - len(self.buffer)))
            if not chunk:
                break
            self.buffer += chunk
        data = self.buffer[:length]
        self.buffer = self.buffer[length:]
        return data

    def readline(self):
        while b"\r\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            self.buffer += chunk
        line, separator, rest = self.buffer.partition(b"\r\n")
        if not separator:
            data = self.buffer
            self.buffer = b""
            return data
        self.buffer = rest
        return line + separator


def read_chunked(reader):
    body = b""
    while True:
        line = reader.readline()
        if not line:
            return body
        size = int(line.split(b";", 1)[0].strip(), 16)
        if size == 0:
            while True:
                trailer = reader.readline()
                if not trailer or trailer == b"\r\n":
                    return body
        body += reader.read(size)
        reader.read(2)


def ws_send(sock, payload, opcode=0x1):
    header = bytes([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header += bytes([length])
    elif length < 65536:
        header += bytes([126]) + length.to_bytes(2, "big")
    else:
        header += bytes([127]) + length.to_bytes(8, "big")
    sock.sendall(header + payload)


def ws_recv(reader):
    header = reader.read(2)
    if len(header) < 2:
        return None, b""
    opcode = header[0] & 0x0F
    masked = bool(header[1] & 0x80)
    length = header[1] & 0x7F
    if length == 126:
        length = int.from_bytes(reader.read(2), "big")
    elif length == 127:
        length = int.from_bytes(reader.read(8), "big")
    mask = reader.read(4) if masked else b""
    payload = reader.read(length)
    if masked and len(mask) == 4:
        payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    return opcode, payload


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        raw = recv_until(self.request, b"\r\n\r\n")
        if not raw:
            return
        try:
            head, _, rest = raw.partition(b"\r\n\r\n")
            lines = head.decode("latin-1").split("\r\n")
            method, target, _version = lines[0].split(" ", 2)
        except Exception:
            return
        self.reader = BufferedReader(self.request, rest)
        headers = {}
        for line in lines[1:]:
            if ":" not in line:
                continue
            name, value = line.split(":", 1)
            name = name.strip().lower()
            value = value.strip()
            headers[name] = value if name not in headers else headers[name] + ", " + value
        parsed = urllib.parse.urlsplit(target)
        path = parsed.path or "/"
        query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)

        if headers.get("upgrade", "").lower() == "websocket":
            key = headers.get("sec-websocket-key", "")
            accept = base64.b64encode(
                hashlib.sha1((key + WS_GUID).encode()).digest()
            ).decode()
            self.request.sendall(
                (
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
                ).encode()
            )
            ws_reader = BufferedReader(self.request, rest)
            while True:
                try:
                    opcode, payload = ws_recv(ws_reader)
                except Exception:
                    return
                if opcode is None or opcode == 0x8:
                    return
                if opcode == 0x9:
                    ws_send(self.request, payload, opcode=0xA)
                elif opcode in (0x1, 0x2):
                    ws_send(self.request, b"echo:" + payload, opcode=opcode)
            return

        if path == "/" and "token" in query:
            if query["token"][0] == self.server.token:
                self.request.sendall(
                    (
                        "HTTP/1.1 303 See Other\r\n"
                        f"Set-Cookie: {COOKIE}; Path=/; HttpOnly\r\n"
                        "Location: ./\r\n"
                        "Connection: close\r\n"
                        "Content-Length: 0\r\n\r\n"
                    ).encode()
                )
            else:
                self.send_text(401, "bad token")
            return

        if COOKIE not in headers.get("cookie", ""):
            self.send_text(401, "unauthorized")
            return

        if path == "/":
            if self.server.expire_once and not self.server.expired:
                self.server.expired = True
                self.send_text(401, "expired")
                return
            self.send_text(
                200,
                "<!doctype html><html><body><h1>mock dsh</h1></body></html>",
                content_type="text/html; charset=utf-8",
            )
            return
        if path == "/api/echo":
            payload = {
                "method": method,
                "target": target,
                "path": path,
                "query": parsed.query,
                "headers": headers,
            }
            self.send_json(200, payload)
            return
        if path == "/api/echo-body":
            if "chunked" in headers.get("transfer-encoding", "").lower():
                body = read_chunked(self.reader)
            else:
                length = int(headers.get("content-length", "0"))
                body = self.reader.read(length)
            self.send_json(
                200,
                {
                    "length": len(body),
                    "sha256": hashlib.sha256(body).hexdigest(),
                    "text": body.decode("utf-8", "replace"),
                },
            )
            return
        if path == "/redirect":
            self.request.sendall(
                (
                    "HTTP/1.1 302 Found\r\n"
                    f"Location: http://127.0.0.1:{self.server.port}/destination\r\n"
                    "Connection: close\r\n"
                    "Content-Length: 0\r\n\r\n"
                ).encode()
            )
            return
        if path == "/sse":
            self.request.sendall(
                (
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Connection: close\r\n\r\n"
                ).encode()
            )
            for index in range(3):
                self.request.sendall(f"data: tick-{index}\n\n".encode())
                time.sleep(0.1)
            return
        self.send_text(404, "not found")

    def send_text(self, status, text, content_type="text/plain; charset=utf-8"):
        body = text.encode()
        reason = {
            200: "OK",
            302: "Found",
            401: "Unauthorized",
            404: "Not Found",
        }.get(status, "OK")
        self.request.sendall(
            (
                f"HTTP/1.1 {status} {reason}\r\n"
                f"Content-Type: {content_type}\r\n"
                f"Content-Length: {len(body)}\r\n"
                "Connection: close\r\n\r\n"
            ).encode()
            + body
        )

    def send_json(self, status, payload):
        self.send_text(status, json.dumps(payload), "application/json")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--token", default=None)
    parser.add_argument("--prompt", action="store_true")
    parser.add_argument("--delay-token", type=float, default=0.0)
    parser.add_argument("--public-url", action="store_true")
    parser.add_argument("--expire-once", action="store_true")
    parser.add_argument("--port-file", default=None)
    args = parser.parse_args()

    if args.prompt:
        print(
            "Need to install the following packages:\n"
            "  mock-dsh@1.0.0\n"
            "Ok to proceed? (y) ",
            end="",
            flush=True,
        )
        answer = sys.stdin.readline().strip().lower()
        if answer not in ("", "y", "yes"):
            print(f"mock: prompt declined ({answer!r})", flush=True)
            return 1
        print("mock: prompt answered with y", flush=True)

    token = args.token or secrets.token_urlsafe(32)
    if args.delay_token > 0:
        time.sleep(args.delay_token)
    with Server(("127.0.0.1", args.port), Handler) as server:
        actual_port = server.server_address[1]
        server.port = actual_port
        server.token = token
        server.expire_once = args.expire_once
        server.expired = False
        if args.port_file:
            with open(args.port_file, "w", encoding="utf-8") as port_file:
                port_file.write(str(actual_port))
        if args.public_url:
            url = f"https://public.example/ui/?token={token}"
        else:
            url = f"http://127.0.0.1:{actual_port}/?token={token}"
        print(f"dsh web: {url}", flush=True)
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
