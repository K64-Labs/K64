#!/usr/bin/env python3
import os
import pathlib
import select
import socket
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
MONITOR = "/tmp/k64-nano-monitor.sock"
SERIAL_PORT = 46065


def wait_tcp(port, timeout=15):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError as exc:
            last = exc
            time.sleep(0.05)
    raise RuntimeError(last)


def read_until(sock, needle, timeout=30):
    data = ""
    deadline = time.time() + timeout
    sock.setblocking(False)
    while time.time() < deadline:
        ready, _, _ = select.select([sock], [], [], max(0.0, min(0.5, deadline - time.time())))
        if not ready:
            continue
        chunk = sock.recv(4096).decode("utf-8", errors="replace")
        if chunk:
            data += chunk
            if needle in data:
                return data
    raise RuntimeError(f"timeout waiting for {needle!r}; tail={data[-1000:]}")


def send_slow(sock, text):
    for byte in text.encode("utf-8"):
        sock.sendall(bytes([byte]))
        time.sleep(0.002)


def monitor_command(command):
    deadline = time.time() + 10
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as mon:
        while True:
            try:
                mon.connect(MONITOR)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)
        mon.settimeout(2)
        try:
            mon.recv(4096)
        except OSError:
            pass
        mon.sendall((command + "\n").encode("utf-8"))
        time.sleep(0.5)


def main():
    subprocess.run(["pkill", "-9", "-x", "qemu-system-x86"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    try:
        os.unlink(MONITOR)
    except FileNotFoundError:
        pass
    log = open(ROOT / ".k64_klcs_gui_qemu.log", "w", encoding="utf-8")
    cmd = [
        "qemu-system-x86_64", "-m", "768M", "-cdrom", "k64.iso", "-boot", "d",
        "-display", "gtk", "-monitor", f"unix:{MONITOR},server=on,wait=off",
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=off",
        "-no-reboot", "-no-shutdown",
    ]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    print(f"QEMU_PID={proc.pid}")
    serial = wait_tcp(SERIAL_PORT)
    time.sleep(1)
    send_slow(serial, "\n")
    read_until(serial, "K64 shell started. Type 'help' for commands.", 30)
    send_slow(serial, "login guest guest\n")
    read_until(serial, "logged in as guest", 10)
    send_slow(serial, "clear\n")
    read_until(serial, ">>>", 5)
    send_slow(serial, "klcs run nano\n")
    read_until(serial, "ELF: executing", 10)
    time.sleep(3)
    shot = ROOT / "build" / "nano-running.ppm"
    monitor_command(f"screendump {shot}")
    print(f"SCREENSHOT={shot}")
    send_slow(serial, "\x18")
    time.sleep(1)
    print(f"QEMU_ALIVE={proc.poll() is None}")


if __name__ == "__main__":
    main()
