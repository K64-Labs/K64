#!/usr/bin/env python3
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time


BOOT_NEEDLE = "K64 shell started. Type 'help' for commands."


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


def read_until(sock, needle, timeout):
    deadline = time.time() + timeout
    data = ""
    sock.setblocking(False)
    while time.time() < deadline:
        ready, _, _ = select.select([sock], [], [], max(0.0, deadline - time.time()))
        if not ready:
            continue
        chunk = sock.recv(4096).decode("utf-8", errors="replace")
        if not chunk:
            continue
        data += chunk
        if needle in data:
            return data
    raise RuntimeError(f"timed out waiting for {needle!r}\nCaptured:\n{data[-5000:]}")


def send(sock, text):
    for byte in text.encode("utf-8"):
        sock.sendall(bytes([byte]))
        time.sleep(0.002)


def stop_guest(proc, sock):
    if sock is not None:
        sock.close()
    if proc is not None:
        proc.kill()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass


def run_guest(cmd, boot_needle_timeout):
    port = free_tcp_port()
    proc = subprocess.Popen(
        cmd + ["-serial", f"tcp:127.0.0.1:{port},server=on,wait=off"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    sock = wait_for_tcp(port, 10)
    read_until(sock, BOOT_NEEDLE, boot_needle_timeout)
    return proc, sock


def main():
    if shutil.which("qemu-system-x86_64") is None:
        print("qemu-system-x86_64 not found; skipping install boot smoke test")
        return 0
    if not os.path.isfile("k64.iso"):
        print("k64.iso missing; skipping install boot smoke test")
        return 0

    fd, disk = tempfile.mkstemp(prefix="k64-install-", suffix=".disk")
    os.close(fd)
    proc = None
    sock = None
    try:
        with open(disk, "wb") as f:
            f.truncate(64 * 1024 * 1024)

        install_cmd = [
            "qemu-system-x86_64",
            "-boot", "order=d",
            "-cdrom", "k64.iso",
            "-drive", f"file={disk},format=raw,if=ide,index=0",
            "-display", "none",
            "-monitor", "none",
            "-no-reboot",
            "-no-shutdown",
        ]
        proc, sock = run_guest(install_cmd, 40)
        send(sock, "login guest guest\n")
        read_until(sock, "logged in as guest", 10)
        send(sock, "install ata0 yes\n")
        read_until(sock, "installer: root filesystem installed", 70)
        stop_guest(proc, sock)
        proc = None
        sock = None

        boot_cmd = [
            "qemu-system-x86_64",
            "-boot", "order=c",
            "-drive", f"file={disk},format=raw,if=ide,index=0",
            "-display", "none",
            "-monitor", "none",
            "-no-reboot",
            "-no-shutdown",
        ]
        proc, sock = run_guest(boot_cmd, 60)
        print("install boot smoke test passed")
        return 0
    finally:
        stop_guest(proc, sock)
        try:
            os.unlink(disk)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
