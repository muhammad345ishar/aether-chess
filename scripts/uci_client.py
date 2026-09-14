#!/usr/bin/env python3
"""Small deadline-bound UCI client shared by smoke tests and benchmarks."""

from __future__ import annotations

import queue
import subprocess
import threading
import time
from collections import deque
from pathlib import Path


class UciEngine:
    def __init__(self, executable: Path, timeout: float = 10.0):
        self.timeout = timeout
        self.recent: deque[str] = deque(maxlen=20)
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.process = subprocess.Popen(
            [str(executable.resolve())],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self.reader = threading.Thread(target=self._read_output, daemon=True)
        self.reader.start()
        try:
            self.send("uci")
            self.read_until("uciok")
            self.send("isready")
            self.read_until("readyok")
        except BaseException:
            self.close()
            raise

    def _read_output(self) -> None:
        assert self.process.stdout is not None
        try:
            for line in self.process.stdout:
                self.lines.put(line.rstrip())
        finally:
            self.lines.put(None)

    def send(self, command: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def read_line(self, timeout: float | None = None) -> str:
        try:
            line = self.lines.get(timeout=self.timeout if timeout is None else max(0, timeout))
        except queue.Empty as exc:
            raise TimeoutError(f"UCI response timed out; recent output: {list(self.recent)}") from exc
        if line is None:
            raise RuntimeError(f"engine exited before responding; recent output: {list(self.recent)}")
        self.recent.append(line)
        return line

    def read_until(self, prefix: str, timeout: float | None = None) -> list[str]:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        result = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"waiting for {prefix!r}; recent output: {list(self.recent)}")
            line = self.read_line(remaining)
            result.append(line)
            if line.startswith(prefix):
                return result

    def close(self) -> None:
        if self.process.poll() is None:
            try:
                self.send("quit")
                self.process.wait(timeout=2)
            except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                self.process.terminate()
                try:
                    self.process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=1)
        self.reader.join(timeout=1)
        if self.process.stdin:
            self.process.stdin.close()
        if self.process.stdout:
            self.process.stdout.close()

    def __enter__(self) -> UciEngine:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
