"""Check the startup update check of a release build (pcsx2-qt AutoUpdaterDialog).

  python updatecheck.py --exe pcsx2-qtx64.exe --inis DIR [--offer zdxsv-X.Y.Z] [--timeout S]

The build must be at a zdxsv-X.Y.Z tag: only a tagged build checks for updates. A local server
answers the release list (ZDXSV_UPDATE_URL) with one release, --offer (default zdxsv-999.0.0),
that has this platform's asset. PCSX2 starts without a game in a new data dir whose PCSX2.ini is
copied from DIR, with the startup check on. Pass: the release list was fetched once and the log
names the offered version and "Update needed.". Control: --offer zdxsv-0.0.0 (older than any
release) must fail ("No update needed."). PCSX2 is closed once the check logged its result, so
the update dialog fetches nothing else.
Exit 0 = pass, 1 = fail, 2 = setup error.
"""
import argparse
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ASSET = {"win32": "pcsx2-zdxsv-windows-x64.7z", "darwin": "pcsx2-zdxsv-macos.tar.xz"}.get(sys.platform, "pcsx2-zdxsv-linux-x64.AppImage")

ap = argparse.ArgumentParser()
ap.add_argument("--exe", required=True)
ap.add_argument("--inis", required=True, help="inis dir of a set-up PCSX2 data dir (PCSX2.ini copied)")
ap.add_argument("--offer", default="zdxsv-999.0.0")
ap.add_argument("--timeout", type=float, default=60)
args = ap.parse_args()

requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests.append(self.path)
        body = json.dumps([{
            "tag_name": args.offer, "draft": False, "prerelease": False, "published_at": "2026-01-01T00:00:00Z",
            "assets": [{"name": ASSET, "size": 1, "browser_download_url": "http://127.0.0.1:%d/none" % port}],
        }]).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

data = tempfile.mkdtemp(prefix="updatecheck-")
inis = os.path.join(data, "PCSX2", "inis")
os.makedirs(inis)
src = os.path.join(args.inis, "PCSX2.ini")
if not os.path.isfile(src):
    print("FAIL: no %s" % src)
    sys.exit(2)
out, section = [], None
for line in open(src, encoding="utf-8-sig").read().splitlines():
    if line.startswith("["):
        section = line
    # startup check on, no skipped version; GS Null renderer
    if section == "[AutoUpdater]" and line.split("=")[0].strip() in ("CheckAtStartup", "LastVersion"):
        continue
    if section == "[EmuCore/GS]" and line.split("=")[0].strip() == "Renderer":
        line = "Renderer = 11"
    out.append(line)
    if line == "[AutoUpdater]":
        out.append("CheckAtStartup = true")
if "[AutoUpdater]" not in out:
    out += ["", "[AutoUpdater]", "CheckAtStartup = true"]
open(os.path.join(inis, "PCSX2.ini"), "w", encoding="utf-8").write("\n".join(out) + "\n")

env = dict(os.environ, ZDXSV_UPDATE_URL="http://127.0.0.1:%d/releases" % port)
log = os.path.join(data, "PCSX2", "logs", "emulog.txt")
proc = subprocess.Popen([args.exe, "-datapath", data], env=env)
text, result = "", None
end = time.time() + args.timeout
while time.time() < end and proc.poll() is None:
    time.sleep(0.5)
    if os.path.isfile(log):
        text = open(log, encoding="utf-8", errors="replace").read()
        result = next((r for r in ("No update needed.", "Update needed.", "Updater Error") if r in text), None)
        if result:
            time.sleep(0.5)
            break
proc.kill()
proc.wait()
server.shutdown()
for line in text.splitlines():
    if any(k in line for k in ("Current version:", "Latest version:", "pdate needed.", "Updater Error")):
        print(line.strip())
shutil.rmtree(data, ignore_errors=True)

ok = len(requests) == 1 and ("Latest version: " + args.offer) in text and result == "Update needed."
print("release list requests %d, result %s" % (len(requests), result))
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
