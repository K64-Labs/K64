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


def qemu_base_cmd():
    net_device = os.environ.get("K64_SMOKE_NET_DEVICE", "rtl8139")
    cmd = [
        "qemu-system-x86_64",
        "-boot",
        "order=d",
        "-cdrom",
        "k64.iso",
    ]
    if os.environ.get("K64_SMOKE_ATTACH_DISK", "1") != "0":
        cmd += [
            "-drive",
            "file=build/root.disk,format=raw,if=ide,index=0",
        ]
    cmd += [
        "-netdev",
        "user,id=k64net",
        "-device",
        f"{net_device},netdev=k64net",
        "-display",
        "none",
        "-monitor",
        "none",
        "-no-reboot",
        "-no-shutdown",
    ]
    return cmd

PTY_RE = re.compile(r"char device redirected to (?P<path>/dev/pts/\d+)")
BOOT_NEEDLE = "K64 shell started. Type 'help' for commands."
PROMPT_NEEDLE = ">>>"


def read_until_fd(fd, needle, timeout):
    deadline = time.time() + timeout
    data = ""
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0.0, deadline - time.time()))
        if not ready:
            continue
        chunk = os.read(fd, 2048).decode("utf-8", errors="replace")
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
        ready, _, _ = select.select([sock], [], [], max(0.0, deadline - time.time()))
        if not ready:
            continue
        chunk = sock.recv(2048).decode("utf-8", errors="replace")
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
        self.read_until(PROMPT_NEEDLE, 10)
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
            qemu_base_cmd() + ["-serial", "pty"],
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
            qemu_base_cmd() + ["-serial", f"tcp:127.0.0.1:{port},server=on,wait=off"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        self.sock = wait_for_tcp(port, 10)

    def read_until(self, needle, timeout):
        if self.serial_fd is not None:
            return read_until_fd(self.serial_fd, needle, timeout)
        return read_until_socket(self.sock, needle, timeout)

    def drain(self):
        deadline = time.time() + 0.15
        while time.time() < deadline:
            if self.serial_fd is not None:
                ready, _, _ = select.select([self.serial_fd], [], [], 0.01)
                if ready:
                    os.read(self.serial_fd, 4096)
                    continue
            elif self.sock is not None:
                self.sock.setblocking(False)
                ready, _, _ = select.select([self.sock], [], [], 0.01)
                if ready:
                    self.sock.recv(4096)
                    continue
            break

    def send(self, text):
        data = text.encode("utf-8")
        if self.serial_fd is not None:
            os.write(self.serial_fd, data)
        else:
            self.sock.sendall(data)

    def command(self, cmd, expected, timeout=15):
        captured = ""
        self.drain()
        self.send(cmd + "\n")
        captured += self.read_until(cmd, timeout)
        if expected != PROMPT_NEEDLE:
            captured += self.read_until(expected, timeout)
        captured += self.read_until(PROMPT_NEEDLE, timeout)
        if expected not in captured:
            raise RuntimeError(f"{cmd!r} did not produce {expected!r}\nCaptured:\n{captured}")
        return captured


def main():
    if shutil.which("qemu-system-x86_64") is None:
        print("qemu-system-x86_64 not found; skipping shell smoke test")
        return 0
    attach_disk = os.environ.get("K64_SMOKE_ATTACH_DISK", "1") != "0"
    if not os.path.isfile("k64.iso") or (attach_disk and not os.path.isfile("build/root.disk")):
        print("required boot artifacts missing; skipping shell smoke test")
        return 0

    with Guest() as guest:
        net_device = os.environ.get("K64_SMOKE_NET_DEVICE", "rtl8139")
        net_driver = "e1000" if net_device.startswith("e1000") else "rtl8139"
        checks = [
            ("help", "Commands:"),
            ("echo shell-smoke-ok", "shell-smoke-ok"),
            ("sysfetch", "Kernel:"),
            ("uname", "K64"),
            ("ticks", "PIT ticks:"),
            ("task", "Current task id:"),
            ("ps", "PID   STATE"),
            ("serial", "Serial:"),
            ("sched", "Scheduler stats:"),
            ("layout", "Current layout:"),
            ("layout us", "Keyboard layout switched to us"),
            ("servicectl list", "PID   STATE"),
            ("driverctl list", "ID    STATE"),
            ("storagectl list", "ata0 id=" if attach_disk else PROMPT_NEEDLE),
            ("storagectl root", "rootfs source:"),
            ("netctl status", f"net: driver={net_driver}"),
            ("netctl arp 10.0.2.2", "net: arp request sent"),
            ("netctl poll", "net: poll complete"),
            ("ping 10.0.2.2", PROMPT_NEEDLE),
            ("udp send 10.0.2.2 9 shell-smoke", PROMPT_NEEDLE),
            ("install", "K64 installer"),
            ("install ata0 yes", "installer: root filesystem installed") if attach_disk else ("install ata0 yes", "installer: failed"),
            ("pwd", "/"),
            ("ls /", "etc/"),
            ("stat /etc/motd", "file /etc/motd"),
            ("cat /etc/motd", "welcome to K64"),
            ("mkdir /tmp/shell-smoke", PROMPT_NEEDLE),
            ("touch /tmp/shell-smoke/empty", PROMPT_NEEDLE),
            ("write /tmp/shell-smoke/file one", PROMPT_NEEDLE),
            ("append /tmp/shell-smoke/file -two", PROMPT_NEEDLE),
            ("cat /tmp/shell-smoke/file", "one-two"),
            ("cp /tmp/shell-smoke/file /tmp/shell-smoke/copy", PROMPT_NEEDLE),
            ("mv /tmp/shell-smoke/copy /tmp/shell-smoke/moved", PROMPT_NEEDLE),
            ("stat /tmp/shell-smoke/moved", "file /tmp/shell-smoke/moved"),
            ("rm /tmp/shell-smoke/moved", PROMPT_NEEDLE),
            ("rm /tmp/shell-smoke/file", PROMPT_NEEDLE),
            ("rm /tmp/shell-smoke/empty", PROMPT_NEEDLE),
            ("rmdir /tmp/shell-smoke", PROMPT_NEEDLE),
            ("sync", "sync complete"),
            ("whoami", "guest"),
            ("id", "real=guest"),
            ("users", "guest"),
            ("groups", "guest"),
            ("elfrun /ex/procinfo.elf", "procinfo: pid="),
            ("elfrun /ex/libctest.elf", "libctest: OK"),
            ("unknown-smoke-command", "Unknown command: unknown-smoke-command"),
        ]
        for cmd, expected in checks:
            guest.command(cmd, expected)

    print("shell smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
