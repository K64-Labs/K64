#!/usr/bin/env python3
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import time

try:
    import termios
except ImportError:
    termios = None


QEMU_BASE_CMD = [
    "qemu-system-x86_64",
    "-cdrom",
    "k64.iso",
    "-drive",
    "file=build/root.disk,format=raw,if=ide,index=0",
    "-display",
    "none",
    "-monitor",
    "none",
    "-no-reboot",
    "-no-shutdown",
]

PTY_RE = re.compile(r"char device redirected to (?P<path>/dev/pts/\d+)")
BOOT_NEEDLE = "K64 shell started. Type 'help' for commands."
PERSIST_PATH = "/tmp/k64-persist-smoke"
PERSIST_TEXT = "k64 persistence survived"


def read_until_fd(fd, needle, timeout):
    deadline = time.time() + timeout
    data = ""
    while time.time() < deadline:
        wait = max(0.0, deadline - time.time())
        ready, _, _ = select.select([fd], [], [], wait)
        if not ready:
            continue
        chunk = os.read(fd, 1024).decode("utf-8", errors="replace")
        if not chunk:
            continue
        data += chunk
        if needle in data:
            return data
    raise RuntimeError(f"timed out waiting for {needle!r}\nCaptured:\n{data}")


def read_until_socket(sock, needle, timeout):
    deadline = time.time() + timeout
    data = ""
    sock.setblocking(False)
    while time.time() < deadline:
        wait = max(0.0, deadline - time.time())
        ready, _, _ = select.select([sock], [], [], wait)
        if not ready:
            continue
        chunk = sock.recv(1024).decode("utf-8", errors="replace")
        if not chunk:
            continue
        data += chunk
        if needle in data:
            return data
    raise RuntimeError(f"timed out waiting for {needle!r}\nCaptured:\n{data}")


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_tcp(port, timeout):
    deadline = time.time() + timeout
    last_error = None
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"failed to connect to QEMU serial TCP port {port}: {last_error}")


class Guest:
    def __init__(self):
        self.proc = None
        self.serial_fd = None
        self.sock = None

    def __enter__(self):
        if os.name == "posix" and termios is not None:
            self._start_pty()
        else:
            self._start_tcp()
        self.read_until(BOOT_NEEDLE, 25)
        self.send("\n")
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.serial_fd is not None:
            os.close(self.serial_fd)
        if self.sock is not None:
            self.sock.close()
        if self.proc is not None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass

    def _start_pty(self):
        self.proc = subprocess.Popen(
            QEMU_BASE_CMD + ["-serial", "pty"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        pty_path = None
        deadline = time.time() + 10
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                if self.proc.poll() is not None:
                    break
                continue
            match = PTY_RE.search(line)
            if match:
                pty_path = match.group("path")
                break
        if not pty_path:
            raise RuntimeError("failed to obtain QEMU serial PTY path")

        self.serial_fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(self.serial_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG)
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.serial_fd, termios.TCSANOW, attrs)

    def _start_tcp(self):
        port = free_tcp_port()
        self.proc = subprocess.Popen(
            QEMU_BASE_CMD + ["-serial", f"tcp:127.0.0.1:{port},server=on,wait=off"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        self.sock = wait_for_tcp(port, 10)

    def read_until(self, needle, timeout):
        if self.serial_fd is not None:
            return read_until_fd(self.serial_fd, needle, timeout)
        return read_until_socket(self.sock, needle, timeout)

    def send(self, text):
        data = text.encode("utf-8")
        if self.serial_fd is not None:
            os.write(self.serial_fd, data)
        else:
            self.sock.sendall(data)


def main():
    if shutil.which(QEMU_BASE_CMD[0]) is None:
        print("qemu-system-x86_64 not found; skipping persistence smoke test")
        return 0
    if not os.path.isfile("k64.iso") or not os.path.isfile("build/root.disk"):
        print("k64.iso or build/root.disk missing; skipping persistence smoke test")
        return 0

    with Guest() as guest:
        guest.send(f"write {PERSIST_PATH} {PERSIST_TEXT}\n")
        guest.send("sync\n")
        guest.read_until("sync complete", 15)

    with Guest() as guest:
        guest.send(f"cat {PERSIST_PATH}\n")
        captured = guest.read_until(PERSIST_TEXT, 15)
        if PERSIST_TEXT not in captured:
            raise RuntimeError(f"persisted file content missing\nCaptured:\n{captured}")

    print("persistence smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
