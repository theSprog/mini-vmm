#!/usr/bin/env python3
"""tests/net_peer.py — host 侧的网络对端，给 e2e 测试做 TCP 收发的参照

guest 经虚拟网卡和它通信：
  - HTTP：GET /blob 返回一段随机数据，guest 用 wget 下载后算 sha256；
  - sink：接受一个 TCP 连接，读到 EOF，算出收到数据的 sha256。

两个 socket 都在打印第一行 "ready ..." 之前 bind + listen 完毕，调用方读到
这一行就可以让 guest 连接，不需要猜服务起来要多久。sink 收完之后打印第二
行 "upload ..." 并退出。两行都是 stdout 上的一行文本，调用方（bash coproc）
按行读取。

数据在内存里随机生成，每次运行不同：guest 端算出的哈希和这里打印的一致，
说明每个字节都经过了网卡的 DMA、校验和与分段路径，而不是碰巧对上。
"""

from __future__ import annotations

import argparse
import hashlib
import os
import socket
import sys
import threading
from enum import IntEnum
from typing import NoReturn


class Exit(IntEnum):
    OK = 0
    FAIL = 1      # 超时或连接出错
    USAGE = 2


HTTP_HEADER_LIMIT = 8192
RECV_CHUNK = 1 << 16


def die(msg: str, code: Exit = Exit.FAIL) -> NoReturn:
    print(f"net_peer: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


def listen(addr: str, port: int) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((addr, port))
    except OSError as e:
        die(f"cannot bind {addr}:{port}: {e}")
    s.listen(4)
    return s


def serve_http(srv: socket.socket, blob: bytes) -> None:
    # 只实现 wget 需要的最小子集：一次请求一个连接，HTTP/1.0 语义
    while True:
        conn, _ = srv.accept()
        with conn:
            req = b""
            while b"\r\n\r\n" not in req and len(req) < HTTP_HEADER_LIMIT:
                chunk = conn.recv(1024)
                if not chunk:
                    break
                req += chunk
            line = req.split(b"\r\n", 1)[0].split()
            if len(line) >= 2 and line[0] == b"GET" and line[1] == b"/blob":
                conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n" % len(blob))
                conn.sendall(blob)
            else:
                conn.sendall(b"HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n")


def main() -> Exit:
    ap = argparse.ArgumentParser(
        prog="tests/net_peer.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Host-side TCP peer for guest network tests: serves a random blob "
                    "over HTTP and hashes one upload.",
        epilog="""\
EXAMPLE:
  tests/net_peer.py --addr 10.0.2.1
      prints 'ready blob_bytes=67108864 blob_sha256=<hex>' once both ports listen;
      guest: wget -q -O - http://10.0.2.1:8080/blob | sha256sum
      guest: nc 10.0.2.1 9000 < file
      prints 'upload bytes=<n> sha256=<hex>' when the upload reaches EOF, then exits

NOTE:
  Both sockets are listening before the 'ready' line is printed, so callers can
  start the guest side as soon as they have read it.
  The blob is random and generated per run.
  Exits 1 if no upload completes within --timeout seconds.
""")
    ap.add_argument("--addr", required=True, help="host address to bind (e.g. the TAP address)")
    ap.add_argument("--http-port", type=int, default=8080, help="HTTP port (default: %(default)s)")
    ap.add_argument("--sink-port", type=int, default=9000, help="upload port (default: %(default)s)")
    ap.add_argument("--blob-mib", type=int, default=64, help="blob size in MiB (default: %(default)s)")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="seconds to wait for the upload (default: %(default)s)")
    args = ap.parse_args()
    if args.blob_mib <= 0:
        die("--blob-mib must be positive", Exit.USAGE)

    blob = os.urandom(args.blob_mib << 20)
    http = listen(args.addr, args.http_port)
    sink = listen(args.addr, args.sink_port)
    print(f"ready blob_bytes={len(blob)} blob_sha256={hashlib.sha256(blob).hexdigest()}", flush=True)

    threading.Thread(target=serve_http, args=(http, blob), daemon=True).start()

    sink.settimeout(args.timeout)
    try:
        conn, _ = sink.accept()
    except socket.timeout:
        die(f"no upload connection within {args.timeout:g} s")
    h = hashlib.sha256()
    n = 0
    with conn:
        conn.settimeout(args.timeout)
        try:
            while chunk := conn.recv(RECV_CHUNK):
                h.update(chunk)
                n += len(chunk)
        except socket.timeout:
            die(f"upload stalled after {n} bytes")
    print(f"upload bytes={n} sha256={h.hexdigest()}", flush=True)
    return Exit.OK


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        die("interrupted")
