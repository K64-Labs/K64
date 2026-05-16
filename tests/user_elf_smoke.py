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


def run_smoke(read_until, send_line):
    boot = read_until("K64 shell started. Type 'help' for commands.", 25)
    send_line("\n")
    combined = boot
    requested = os.environ.get("K64_SMOKE_ELFS", "hello,catmotd")
    elfs = [name.strip() for name in requested.split(",") if name.strip()]

    if "hello" in elfs:
        send_line("elfrun /ex/hello.elf\n")
        combined += read_until("ELF: exit code 42", 15)
        if "hello from K64 user mode" not in combined:
            raise RuntimeError(f"user hello output missing\nCaptured:\n{combined}")

    if "catmotd" in elfs:
        send_line("elfrun /ex/catmotd.elf\n")
        combined += read_until("ELF: exit code 0", 15)
        if "motd => " not in combined or "welcome to K64" not in combined:
            raise RuntimeError(f"user file I/O output missing\nCaptured:\n{combined}")

    print("user ELF smoke test passed")


def main():
    if shutil.which(QEMU_BASE_CMD[0]) is None:
        print("qemu-system-x86_64 not found; skipping user ELF smoke test")
        return 0
    if not os.path.isfile("k64.iso") or not os.path.isfile("build/root.disk"):
        print("k64.iso or build/root.disk missing; skipping user ELF smoke test")
        return 0

    if os.name != "posix" or termios is None:
        return main_tcp_serial()
    return main_pty_serial()


def main_pty_serial():
    qemu_cmd = QEMU_BASE_CMD + ["-serial", "pty"]
    proc = subprocess.Popen(
        qemu_cmd,
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

        run_smoke(lambda needle, timeout: read_until_fd(serial_fd, needle, timeout),
                  lambda text: os.write(serial_fd, text.encode("utf-8")))
        return 0
    finally:
        if serial_fd is not None:
            os.close(serial_fd)
        proc.kill()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass


def main_tcp_serial():
    port = free_tcp_port()
    qemu_cmd = QEMU_BASE_CMD + ["-serial", f"tcp:127.0.0.1:{port},server=on,wait=off"]
    proc = subprocess.Popen(
        qemu_cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    sock = None
    try:
        sock = wait_for_tcp(port, 10)
        run_smoke(lambda needle, timeout: read_until_socket(sock, needle, timeout),
                  lambda text: sock.sendall(text.encode("utf-8")))
        return 0
    finally:
        if sock is not None:
            sock.close()
        proc.kill()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
