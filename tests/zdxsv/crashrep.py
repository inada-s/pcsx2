#!/usr/bin/env python3
"""Crash report contents: crash a build with the Sentry SDK against a local DSN and check the upload.

Usage: crashrep.py <pcsx2-qt exe> <out dir>
Exit 0 when the upload carries PCSX2.ini without [Folders], [GameList], [Achievements] or absolute
path values, keeps the other sections, and carries emulog-tail.txt (not on macOS: crashpad runs no in-process crash callback there)
with the home folder name of its paths masked (the data path sits under Users/<name>).
"""

import gzip
import http.server
import os
import subprocess
import sys
import threading
import time

SECRET = "CrashRepSecretName"
KEEP = "crashrep-keep-this-line"
HDD = f"C:\\Users\\{SECRET}\\hdd.raw" if os.name == "nt" else f"/Users/{SECRET}/hdd.raw"

INI = f"""[UI]
SettingsVersion = 1
SetupWizardIncomplete = false

[AutoUpdater]
CheckAtStartup = false

[Folders]
Bios = C:\\Users\\{SECRET}\\bios

[GameList]
RecursivePaths = D:\\{SECRET}\\iso

[Achievements]
Username = {SECRET}

[DEV9/Hdd]
HddFile = {HDD}

[CrashRepKeep]
Marker = {KEEP}
"""


def attachment(body, name):
    at = body.find(f'filename="{name}"'.encode())
    if at < 0:
        return None
    start = body.find(b"\r\n\r\n", at) + 4
    end = body.find(b"\r\n--", start)
    return body[start:end if end >= 0 else len(body)]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    exe, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    data = os.path.join(out, "Users", SECRET, "data")
    inis = os.path.join(data, "PCSX2", "inis")
    os.makedirs(inis, exist_ok=True)
    with open(os.path.join(inis, "PCSX2.ini"), "w", encoding="utf-8") as f:
        f.write(INI)

    bodies = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n) if n else b""
            if not n and "chunked" in (self.headers.get("Transfer-Encoding") or ""):
                while True:
                    size = int(self.rfile.readline().strip(), 16)
                    if size == 0:
                        self.rfile.readline()
                        break
                    body += self.rfile.read(size)
                    self.rfile.readline()
            if self.headers.get("Content-Encoding") == "gzip" or body[:2] == b"\x1f\x8b":
                body = gzip.decompress(body)
            bodies.append((self.path, body))
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"{}")

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ)
    env["ZDXSV_SENTRY_DSN"] = f"http://k@127.0.0.1:{server.server_port}/1"
    env["ZDXSV_CRASH_REPORT_TEST"] = "1"
    proc = subprocess.Popen([exe, "-datapath", data], env=env)

    deadline = time.time() + 60
    while time.time() < deadline and not any("minidump" in p for p, _ in bodies):
        time.sleep(0.5)
    if proc.poll() is None:
        proc.kill()
    server.shutdown()

    uploads = [b for p, b in bodies if "minidump" in p]
    if not uploads:
        print("FAIL: no minidump upload in 60 s (a build without the SDK sends none)")
        return 1
    body = uploads[0]
    with open(os.path.join(out, "upload.bin"), "wb") as f:
        f.write(body)
    ini, tail = attachment(body, "PCSX2.ini"), attachment(body, "emulog-tail.txt")
    checks = {
        "PCSX2.ini attached": ini is not None,
        "other sections kept": KEEP.encode() in (ini or b""),
        "no folder, game list, achievements or path values": SECRET.encode() not in (ini or b""),
    }
    if sys.platform != "darwin":
        checks["emulog-tail.txt attached"] = tail is not None
        checks["log tail: home folder name masked"] = os.path.join("Users", "*").encode() in (tail or b"") and SECRET.encode() not in (tail or b"")
    for name, ok in checks.items():
        print(("PASS " if ok else "FAIL ") + name)
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
