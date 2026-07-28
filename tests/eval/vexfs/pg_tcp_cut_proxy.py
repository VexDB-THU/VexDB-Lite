#!/usr/bin/env python3
"""Loopback TCP relay with cut, new-connection blackhole and live-link stall."""

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import threading
import time
from pathlib import Path


class Relay:
    def __init__(self, target_host: str, target_port: int, ack_file: Path,
                 status_file: Path) -> None:
        self.target = (target_host, target_port)
        self.ack_file = ack_file
        self.status_file = status_file
        self.lock = threading.Lock()
        self.pairs: set[tuple[socket.socket, socket.socket]] = set()
        self.blackhole_clients: set[socket.socket] = set()
        self.stopping = threading.Event()
        self.cut_requested = threading.Event()
        self.toggle_blackhole_requested = threading.Event()
        self.toggle_existing_stall_requested = threading.Event()
        self.blackhole = threading.Event()
        self.existing_stall = threading.Event()
        self.cut_count = 0
        self.accepted = 0

    def snapshot(self) -> dict[str, int | str]:
        with self.lock:
            return {
                "accepted": self.accepted,
                "pairs": len(self.pairs),
                "blackhole_clients": len(self.blackhole_clients),
                "mode": (
                    "blackhole" if self.blackhole.is_set()
                    else "established-blackhole" if self.existing_stall.is_set()
                    else "relay"
                ),
            }

    @staticmethod
    def write_json(path: Path, value: dict[str, int | str]) -> None:
        temporary = path.with_suffix(f".tmp-{os.getpid()}")
        temporary.write_text(json.dumps(value) + "\n")
        temporary.replace(path)

    def write_status(self) -> None:
        self.write_json(self.status_file, self.snapshot())

    def close_pair(self, pair: tuple[socket.socket, socket.socket]) -> None:
        with self.lock:
            self.pairs.discard(pair)
        for stream in pair:
            try:
                stream.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            stream.close()

    def copy(self, source: socket.socket, destination: socket.socket,
             pair: tuple[socket.socket, socket.socket]) -> None:
        try:
            while not self.stopping.is_set():
                if self.existing_stall.is_set():
                    time.sleep(0.01)
                    continue
                try:
                    data = source.recv(64 * 1024)
                except (TimeoutError, socket.timeout):
                    continue
                if not data:
                    break
                destination.sendall(data)
        except OSError:
            pass
        finally:
            self.close_pair(pair)

    def accept(self, client: socket.socket) -> None:
        if self.blackhole.is_set():
            with self.lock:
                self.blackhole_clients.add(client)
                self.accepted += 1
            self.write_status()
            return
        try:
            server = socket.create_connection(self.target, timeout=5)
            client.settimeout(0.1)
            server.settimeout(0.1)
        except OSError:
            client.close()
            return
        pair = (client, server)
        with self.lock:
            self.pairs.add(pair)
            self.accepted += 1
        self.write_status()
        threading.Thread(target=self.copy, args=(client, server, pair), daemon=True).start()
        threading.Thread(target=self.copy, args=(server, client, pair), daemon=True).start()

    def close_blackhole_clients(self) -> int:
        with self.lock:
            clients = list(self.blackhole_clients)
            self.blackhole_clients.clear()
        for client in clients:
            client.close()
        return len(clients)

    def cut(self) -> None:
        with self.lock:
            pairs = list(self.pairs)
        for pair in pairs:
            self.close_pair(pair)
        blocked = self.close_blackhole_clients()
        self.cut_count += 1
        value = self.snapshot()
        value.update({"cuts": self.cut_count,
                      "connections": len(pairs) + blocked})
        self.write_json(self.ack_file, value)
        self.write_status()

    def toggle_blackhole(self) -> None:
        if self.blackhole.is_set():
            self.blackhole.clear()
            closed = self.close_blackhole_clients()
        else:
            self.blackhole.set()
            with self.lock:
                pairs = list(self.pairs)
            for pair in pairs:
                self.close_pair(pair)
            closed = len(pairs)
        value = self.snapshot()
        value.update({"cuts": self.cut_count, "connections": closed})
        self.write_json(self.ack_file, value)
        self.write_status()

    def toggle_existing_stall(self) -> None:
        if self.existing_stall.is_set():
            self.existing_stall.clear()
        else:
            self.existing_stall.set()
        value = self.snapshot()
        value.update({"cuts": self.cut_count, "connections": len(self.pairs)})
        self.write_json(self.ack_file, value)
        self.write_status()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-host", required=True)
    parser.add_argument("--target-port", required=True, type=int)
    parser.add_argument("--ready-file", required=True, type=Path)
    parser.add_argument("--ack-file", required=True, type=Path)
    parser.add_argument("--status-file", required=True, type=Path)
    arguments = parser.parse_args()

    relay = Relay(arguments.target_host, arguments.target_port, arguments.ack_file,
                  arguments.status_file)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(32)
    listener.settimeout(0.1)

    signal.signal(signal.SIGUSR1, lambda *_: relay.cut_requested.set())
    signal.signal(signal.SIGUSR2, lambda *_: relay.toggle_blackhole_requested.set())
    signal.signal(signal.SIGHUP, lambda *_: relay.toggle_existing_stall_requested.set())
    signal.signal(signal.SIGTERM, lambda *_: relay.stopping.set())
    temporary = arguments.ready_file.with_suffix(f".tmp-{os.getpid()}")
    temporary.write_text(json.dumps({"pid": os.getpid(),
                                     "port": listener.getsockname()[1]}) + "\n")
    temporary.replace(arguments.ready_file)
    relay.write_status()

    try:
        while not relay.stopping.is_set():
            if relay.cut_requested.is_set():
                relay.cut_requested.clear()
                relay.cut()
            if relay.toggle_blackhole_requested.is_set():
                relay.toggle_blackhole_requested.clear()
                relay.toggle_blackhole()
            if relay.toggle_existing_stall_requested.is_set():
                relay.toggle_existing_stall_requested.clear()
                relay.toggle_existing_stall()
            try:
                client, _ = listener.accept()
            except (TimeoutError, socket.timeout):
                continue
            except OSError:
                if relay.stopping.is_set():
                    break
                raise
            relay.accept(client)
    finally:
        relay.stopping.set()
        listener.close()
        relay.cut()
        time.sleep(0.05)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
