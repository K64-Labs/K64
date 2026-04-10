#!/usr/bin/env python3
import os
import re
import select
import subprocess
import sys
import termios
import time


QEMU_CMD = [
    "qemu-system-x86_64",
    "-cdrom",
    "k64.iso",
    "-drive",
    "file=build/root.disk,format=raw,if=ide,index=0",
    "-display",
    "none",
    "-monitor",
    "none",
    "-serial",
    "pty",
    "-no-reboot",
    "-no-shutdown",
]

PTY_RE = re.compile(r"char device redirected to (?P<path>/dev/pts/\d+)")


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


def main():
    proc = subprocess.Popen(
        QEMU_CMD,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    serial_fd = None
    try:
        pty_path = None
        deadline = time.time() + 10
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                continue
            match = PTY_RE.search(line)
            if match:
                pty_path = match.group("path")
                break
        if not pty_path:
            raise RuntimeError("failed to obtain QEMU serial PTY path")

        serial_fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(serial_fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG)
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(serial_fd, termios.TCSANOW, attrs)

        boot = read_until_fd(serial_fd, "K64 shell started. Type 'help' for commands.", 25)
        os.write(serial_fd, b"\n")
        os.write(serial_fd, b"elfrun /ex/hello.elf\n")
        hello = read_until_fd(serial_fd, "ELF: exit code 42", 15)
        os.write(serial_fd, b"elfrun /ex/catmotd.elf\n")
        motd = read_until_fd(serial_fd, "ELF: exit code 0", 15)
        combined = boot + hello + motd
        if "hello from K64 user mode" not in combined:
            raise RuntimeError(f"user hello output missing\nCaptured:\n{combined}")
        if "motd => " not in combined or "welcome to K64" not in combined:
            raise RuntimeError(f"user file I/O output missing\nCaptured:\n{combined}")
        print("user ELF smoke test passed")
        return 0
    finally:
        if serial_fd is not None:
            os.close(serial_fd)
        proc.kill()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
