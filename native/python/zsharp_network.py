"""Internal asynchronous networking worker; not a user-importable Z# package.

Uses the runtime's bundled CPython standard library and certificate verification.
Threads perform I/O only; the owning ZVM task polls and invokes Z# callbacks.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import threading
import urllib.error
import urllib.parse
import urllib.request
from collections import deque

LIMIT = 1024 * 1024
contexts = {}
lock = threading.RLock()


def export(function):
    function.__zsharp_export__ = True
    return function


class Context:
    def __init__(self):
        self.requests = {}
        self.sockets = {}
        self.events = deque()
        self.closed = False
        self.serial = 0
        self.failure = None

    def emit(self, target, *args):
        with lock:
            if not self.closed and target:
                if len(self.events) >= 256:
                    self.failure = "Network callback queue exceeded 256 events"
                    self.shutdown()
                    return
                self.events.append({"target": target, "args": args})

    def shutdown(self):
        self.closed = True
        self.events.clear()
        for request in self.requests.values():
            request["active"] = False
        for entry in self.sockets.values():
            entry.close()


def url_parts(url, schemes):
    if not isinstance(url, str):
        raise TypeError("URL must be text")
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in schemes or not parts.hostname or parts.username or parts.password:
        raise ValueError("Unsupported URL or credentials in URL")
    if parts.fragment:
        raise ValueError("Network URLs cannot contain fragments")
    return parts


def http_request(context, options):
    url_parts(options["url"], ("http", "https"))
    context.serial += 1
    name = options.get("name", f"Request{context.serial}")
    if not isinstance(name, str):
        raise TypeError("HTTP request name must be text")
    if context.requests.get(name, {}).get("active"):
        raise ValueError("HTTP request name is already active")
    if sum(r["active"] for r in context.requests.values()) >= 32:
        raise ValueError("At most 32 active HTTP requests")
    timeout = options.get("timeout", 30000)
    if not isinstance(timeout, (int, float)) or not 1 <= timeout <= 300000:
        raise ValueError("HTTP timeout must be 1..300000 ms")
    headers = options.get("headers", {})
    if isinstance(headers, str):
        headers = json.loads(headers)
    if not isinstance(headers, dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in headers.items()):
        raise ValueError("HTTP headers must be a JSON object of text values")
    method = options.get("method", "GET").upper()
    if method not in ("GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"):
        raise ValueError("Unsupported HTTP method")
    body = options.get("body")
    if body is not None and not isinstance(body, str):
        raise TypeError("HTTP body must be text")
    if body and method in ("GET", "HEAD"):
        raise ValueError("GET/HEAD cannot have a body")
    request = {"active": True, "error": options["onError"], "headers": "{}"}
    context.requests[name] = request
    while len(context.requests) > 128:
        old = next((key for key, item in context.requests.items() if not item["active"]), None)
        if old is None:
            break
        del context.requests[old]

    def work():
        try:
            data = body.encode("utf-8") if body else None
            if data and len(data) > LIMIT:
                raise ValueError("HTTP body exceeds 1 MiB")
            req = urllib.request.Request(options["url"], data=data, headers=headers, method=method)
            try:
                response = urllib.request.urlopen(req, timeout=timeout / 1000)
            except urllib.error.HTTPError as exc:
                response = exc  # 4xx/5xx are responses, not transport failures.
            with response:
                payload = response.read(LIMIT + 1)
                if len(payload) > LIMIT:
                    raise ValueError("HTTP response exceeds 1 MiB")
                text = payload.decode("utf-8", errors="replace")
                with lock:
                    if request["active"] and not context.closed:
                        request["active"] = False
                        request["headers"] = json.dumps(dict(response.headers))
                        context.emit(options["onResponse"], response.status, text)
        except Exception as exc:
            with lock:
                if request["active"] and not context.closed:
                    request["active"] = False
                    context.emit(options["onError"], str(exc))

    threading.Thread(target=work, daemon=True, name="ZVM HTTP").start()
    while len(context.requests) > 128:
        old = next((key for key, item in context.requests.items() if not item["active"]), None)
        if old is None:
            break
        del context.requests[old]


class Websocket:
    def __init__(self, context, url):
        self.parts = url_parts(url, ("ws", "wss"))
        self.context = context
        self.callbacks = {}
        self.state = "connecting"
        self.socket = None
        self.stopped = threading.Event()
        self.send_lock = threading.Lock()
        self.sending = deque()

    def close(self):
        self.state = "closing"
        self.stopped.set()
        sock = self.socket
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

    def exact(self, count):
        result = bytearray()
        while len(result) < count:
            if self.stopped.is_set() or self.context.closed:
                raise EOFError("Socket closed")
            try:
                part = self.socket.recv(count - len(result))
            except socket.timeout:
                self.flush()
                continue
            if not part:
                raise EOFError("Socket disconnected")
            result.extend(part)
        return bytes(result)

    def frame(self, opcode, payload):
        mask = os.urandom(4)
        size = len(payload)
        header = bytes((0x80 | opcode, 0x80 | size)) if size < 126 else (
            bytes((0x80 | opcode, 0x80 | 126)) + struct.pack("!H", size) if size <= 65535 else
            bytes((0x80 | opcode, 0x80 | 127)) + struct.pack("!Q", size))
        with self.send_lock:
            self.socket.sendall(header + mask + bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload)))

    def flush(self):
        while True:
            with lock:
                if not self.sending:
                    return
                message = self.sending.popleft()
            self.frame(1, message)

    def run(self):
        code, reason = 1006, "Connection lost"
        try:
            parts = self.parts
            sock = socket.create_connection((parts.hostname, parts.port or (443 if parts.scheme == "wss" else 80)), timeout=10)
            if parts.scheme == "wss":
                sock = ssl.create_default_context().wrap_socket(sock, server_hostname=parts.hostname)
            self.socket = sock
            if self.stopped.is_set():
                return
            key = base64.b64encode(os.urandom(16)).decode("ascii")
            path = urllib.parse.urlunsplit(("", "", parts.path or "/", parts.query, ""))
            host = parts.hostname
            if ":" in host:
                host = f"[{host}]"
            if parts.port:
                host += f":{parts.port}"
            request = f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
            sock.sendall(request.encode("ascii"))
            response = bytearray()
            while not response.endswith(b"\r\n\r\n"):
                part = sock.recv(1)
                if not part or len(response) > 16384:
                    raise ValueError("Invalid WebSocket handshake")
                response.extend(part)
            lines = response.decode("iso-8859-1").split("\r\n")
            headers = dict(line.split(":", 1) for line in lines[1:] if ":" in line)
            headers = {k.lower(): v.strip() for k, v in headers.items()}
            accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()).decode("ascii")
            if not lines[0].startswith("HTTP/1.1 101 ") or headers.get("sec-websocket-accept") != accept or headers.get("upgrade", "").lower() != "websocket" or "upgrade" not in [part.strip().lower() for part in headers.get("connection", "").split(",")]:
                raise ValueError("WebSocket handshake rejected")
            sock.settimeout(0.05)
            with lock:
                self.state = "open"
                self.context.emit(self.callbacks.get("connected"))
            fragments, fragment_kind = bytearray(), None
            while not self.stopped.is_set() and not self.context.closed:
                self.flush()
                first, second = self.exact(2)
                opcode, final = first & 15, bool(first & 128)
                if first & 0x70 or second & 0x80:
                    raise ValueError("Invalid WebSocket frame flags")
                size = second & 127
                if size == 126:
                    size = struct.unpack("!H", self.exact(2))[0]
                elif size == 127:
                    size = struct.unpack("!Q", self.exact(8))[0]
                if size > LIMIT or len(fragments) + size > LIMIT:
                    raise ValueError("WebSocket message exceeds 1 MiB")
                if opcode >= 8 and (not final or size > 125):
                    raise ValueError("Invalid WebSocket control frame")
                payload = self.exact(size)
                if opcode == 8:
                    if len(payload) == 1:
                        raise ValueError("Invalid WebSocket close frame")
                    code = struct.unpack("!H", payload[:2])[0] if payload else 1000
                    reason = payload[2:].decode("utf-8") if payload else ""
                    self.frame(8, payload)
                    break
                if opcode == 9:
                    self.frame(10, payload)
                    continue
                if opcode == 10:
                    continue
                if opcode in (1, 2):
                    if fragment_kind is not None:
                        raise ValueError("Unexpected new WebSocket message")
                    fragment_kind = opcode
                elif opcode != 0 or fragment_kind is None:
                    raise ValueError("Invalid WebSocket opcode")
                fragments.extend(payload)
                if final:
                    if fragment_kind != 1:
                        self.context.emit(self.callbacks.get("error"), "Binary WebSocket messages are not supported")
                    else:
                        self.context.emit(self.callbacks.get("callback"), fragments.decode("utf-8"))
                    fragments.clear()
                    fragment_kind = None
        except Exception as exc:
            if not self.stopped.is_set() and not self.context.closed:
                self.context.emit(self.callbacks.get("error"), str(exc))
        finally:
            if self.stopped.is_set():
                code, reason = 1000, ""
            if self.socket:
                self.socket.close()
            self.state = "closed"
            self.context.emit(self.callbacks.get("closed"), code, reason)


@export
def command(scope, operation, payload):
    args = json.loads(payload)
    with lock:
        context = contexts.setdefault(scope, Context())
        if operation == "shutdown":
            context.shutdown()
            contexts.pop(scope, None)
            return "null"
        if operation == "poll":
            if context.failure:
                raise RuntimeError(context.failure)
            events = [context.events.popleft() for _ in range(min(len(context.events), 16))]
            active = bool(context.events) or any(r["active"] for r in context.requests.values()) or any(s.state != "closed" for s in context.sockets.values())
            return json.dumps({"active": active, "events": events})
        if operation == "HTTP.request":
            http_request(context, dict(zip(args[::2], args[1::2])))
            return "null"
        if operation.startswith("HTTP."):
            request = context.requests.get(args[0])
            if request is None:
                raise ValueError("Unknown HTTP request")
            if operation == "HTTP.responseHeaders":
                return json.dumps(request["headers"])
            if operation == "HTTP.requestCancel":
                if request["active"]:
                    request["active"] = False
                    context.emit(request["error"], "Request cancelled")
                return "null"
        method = operation.removeprefix("Websocket.")
        if method == "connect":
            url, name = args
            if not isinstance(name, str):
                raise TypeError("Socket name must be text")
            if name in context.sockets and context.sockets[name].state != "closed":
                raise ValueError("Socket name is already in use")
            if sum(s.state != "closed" for s in context.sockets.values()) >= 32:
                raise ValueError("At most 32 sockets")
            entry = Websocket(context, url)
            context.sockets[name] = entry
            threading.Thread(target=entry.run, daemon=True, name="ZVM WebSocket").start()
            return "null"
        entry = context.sockets.get(args[0])
        if entry is None:
            raise ValueError("Unknown socket name")
        if method in ("callback", "connected", "error", "closed"):
            entry.callbacks[method] = args[1]
            if method == "connected" and entry.state == "open":
                context.emit(args[1])
        elif method == "state":
            return json.dumps(entry.state)
        elif method == "send":
            if entry.state != "open":
                raise ValueError("Socket must be open before sending")
            message = args[1]
            if not isinstance(message, str):
                raise TypeError("WebSocket messages must be text")
            message = message.encode("utf-8")
            if len(message) > LIMIT or sum(len(item) for item in entry.sending) + len(message) > LIMIT:
                raise ValueError("Socket send buffer/message exceeds 1 MiB")
            entry.sending.append(message)
        elif method == "close":
            entry.close()
        else:
            raise ValueError("Unknown network operation")
        return "null"
