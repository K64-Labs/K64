#!/usr/bin/env python3
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import http.server
import socketserver
import threading
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


class TestHTTPServer(socketserver.TCPServer):
    allow_reuse_address = True


class TestHTTPHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"k64-http-ok\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Guest:
    def __init__(self):
        self.proc = None
        self.serial_fd = None
        self.sock = None

    def __enter__(self):
        serial_mode = os.environ.get("K64_SMOKE_SERIAL", "").lower()
        if serial_mode == "tcp" or os.environ.get("WSL_DISTRO_NAME"):
            self._start_tcp()
        elif os.name == "posix" and termios is not None:
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

    def read_chunk(self, timeout):
        if self.serial_fd is not None:
            ready, _, _ = select.select([self.serial_fd], [], [], timeout)
            if not ready:
                return ""
            return os.read(self.serial_fd, 2048).decode("utf-8", errors="replace")
        self.sock.setblocking(False)
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if not ready:
            return ""
        return self.sock.recv(2048).decode("utf-8", errors="replace")

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
        for byte in data:
            chunk = bytes([byte])
            if self.serial_fd is not None:
                os.write(self.serial_fd, chunk)
            else:
                self.sock.sendall(chunk)
            time.sleep(0.002)

    def command(self, cmd, expected, timeout=15):
        captured = ""
        self.drain()
        self.send(cmd + "\n")
        captured += self.read_until(cmd, timeout)
        command_pos = captured.find(cmd)
        output_start = command_pos + len(cmd) if command_pos >= 0 else 0
        deadline = time.time() + timeout
        while time.time() < deadline:
            post_command = captured[output_start:]
            expected_seen = expected == PROMPT_NEEDLE or expected in post_command
            if expected_seen and PROMPT_NEEDLE in post_command:
                return captured
            captured += self.read_chunk(max(0.0, min(0.25, deadline - time.time())))
        if expected != PROMPT_NEEDLE and expected not in captured[output_start:]:
            raise RuntimeError(f"{cmd!r} did not produce {expected!r}\nCaptured:\n{captured}")
        if PROMPT_NEEDLE not in captured[output_start:]:
            raise RuntimeError(f"{cmd!r} did not return to prompt\nCaptured:\n{captured}")
        return captured


def main():
    if shutil.which("qemu-system-x86_64") is None:
        print("qemu-system-x86_64 not found; skipping shell smoke test")
        return 0
    attach_disk = os.environ.get("K64_SMOKE_ATTACH_DISK", "1") != "0"
    if not os.path.isfile("k64.iso") or (attach_disk and not os.path.isfile("build/root.disk")):
        print("required boot artifacts missing; skipping shell smoke test")
        return 0

    http_server = TestHTTPServer(("0.0.0.0", 0), TestHTTPHandler)
    http_port = http_server.server_address[1]
    http_thread = threading.Thread(target=http_server.serve_forever, daemon=True)
    http_thread.start()

    try:
        with Guest() as guest:
            net_device = os.environ.get("K64_SMOKE_NET_DEVICE", "rtl8139")
            net_driver = "e1000" if net_device.startswith("e1000") else "rtl8139"
            checks = [
            ("help", "shutdown         - power down the machine"),
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
            ("cat /etc/keyboard/layout.cfg", "us"),
            ("servicectl list", "PID   STATE"),
            ("calls", "kernel.version"),
            ("call kernel.version", "0.3.20"),
            ("call fs.stat /etc/motd", "file /etc/motd"),
            ("driverctl list", "ID    STATE"),
            ("storagectl list", "size=" if attach_disk else PROMPT_NEEDLE),
            ("storagectl partitions ata0", "unallocated=" if attach_disk else "storagectl: disk not found"),
            ("storagectl root", "image_limit="),
            ("grow /", "grow complete") if attach_disk else ("grow /", "grow failed"),
            ("netctl status", f"net: driver={net_driver}"),
            ("kpm sources", "Sources:"),
            ("kpm source del smoke", PROMPT_NEEDLE),
            ("kpm source add smoke http://repo.k64os.org", "kpm: source added"),
            ("cat /etc/kpm/sources.cfg", "smoke http://repo.k64os.org"),
            ("netctl dhcp", "dhcp: complete"),
            ("netctl resolve 10.0.2.2", "resolve: 10.0.2.2 -> 10.0.2.2"),
            ("netctl arp 10.0.2.2", "net: arp request sent"),
            ("netctl poll", "net: poll complete"),
            ("ping 10.0.2.2", PROMPT_NEEDLE),
            ("kcurl https://example.com", "kcurl: only plain http://host[:port]/path URLs are supported"),
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
            ("args alpha beta", "argv[2]=beta"),
            ("elfrun /ex/procinfo.elf", "procinfo: pid="),
            ("elfrun /ex/args.elf one two", "argv[2]=two"),
            ("elfrun /ex/libctest.elf", "libctest: OK"),
            ("elfrun /ex/secprobe.elf", "security-probe: OK"),
            ("elfrun /ex/procmodel.elf", "procmodel: self pid="),
            ("elfrun /ex/spawnwait.elf", "spawnwait: OK"),
            ("elfrun /ex/badwait.elf", "badwait: OK"),
            ("elfrun /ex/proctree.elf", "proctree: OK"),
            ("elfrun /ex/fdtest.elf", "fdtest: OK"),
            ("elfrun /ex/pipetest.elf", "pipetest: OK"),
            ("elfrun /ex/spawnreal.elf", "spawnreal: OK"),
            ("elfrun /ex/waitblock.elf", "waitblock: OK"),
            ("elfrun /ex/spawnrace.elf", "spawnrace: OK"),
            ("elfrun /ex/multitask.elf", "multitask: OK"),
            ("elfrun /ex/faultwait.elf", "faultwait: OK"),
            ("elfrun /ex/calltest.elf", "calltest: OK"),
            ("elfrun /ex/fsservicetest.elf", "fsservicetest: OK"),
            ("elfrun /ex/procservicetest.elf", "procservicetest: OK"),
            ("elfrun /ex/badcall.elf", "badcall: OK"),
            ("elfrun /ex/manyproc.elf", "manyproc: OK"),
            ("unknown-smoke-command", "Unknown command: unknown-smoke-command"),
            ]
            if net_driver == "rtl8139":
                checks.insert(25, (f"kcurl http://10.0.2.2:{http_port}/", "k64-http-ok"))
            if os.environ.get("K64_SMOKE_EXTERNAL_NET") == "1":
                checks.insert(25, ("netctl resolve example.com", "resolve: example.com ->"))
                checks.insert(26, ("kcurl example.com", "Example Domain"))
            for cmd, expected in checks:
                guest.command(cmd, expected)

            guest.send("edit /tmp/edit-smoke.txt\n")
            guest.read_until("K64 edit", 10)
            guest.send("hello from edit\nsecond line")
            time.sleep(0.2)
            guest.send("@s")
            guest.read_until("Saved", 10)
            guest.send("@q")
            guest.read_until(PROMPT_NEEDLE, 10)
            guest.send("cat /tmp/edit-smoke.txt\n")
            guest.read_until("second line", 10)
    finally:
        http_server.shutdown()
        http_server.server_close()

    print("shell smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
