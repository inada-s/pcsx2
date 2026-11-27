"""Check the startup update check of a tagged Devel build (pcsx2-qt AutoUpdaterDialog).

  python updatecheck.py --exe pcsx2-qtx64.exe --inis DIR [--offer zdxsv-X.Y.Z] [--no-auto-install]
                        [--sums ok|bad|missing|host] [--timeout S]

The build must be at a zdxsv-X.Y.Z tag: only a tagged build checks for updates. A Release build
ignores ZDXSV_UPDATE_URL and fetches the real list (requests 0: FAIL). A local server
answers the release list (ZDXSV_UPDATE_URL) with one release, --offer (default zdxsv-999.0.0),
that has this platform's asset and SHA256SUMS.txt at the release download paths of this fork
(/inada-s/pcsx2/releases/download/<tag>/<name> on the server). PCSX2 starts without a game in a new
data dir whose PCSX2.ini is copied from DIR, with the startup check on. Pass: the release list was
fetched once, the log names the offered version and "Update needed.", SHA256SUMS.txt and then the
update were downloaded without a click and the update passed the checksum (the served file is not an
update, so nothing is installed). --no-auto-install sets [AutoUpdater] ZdxsvAutoInstall = false:
pass when nothing is downloaded within 10 s.
--sums bad: SHA256SUMS.txt lists another hash; pass when the update is downloaded and refused
("Update refused"). --sums missing (no SHA256SUMS.txt in the release) and --sums host (the asset URL
on another host name): pass when the release is refused and nothing is downloaded.
Control: --offer zdxsv-0.0.0 (older than any release) must fail ("No update needed.").
Exit 0 = pass, 1 = fail, 2 = setup error.
"""
import argparse
import hashlib
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
SUMS = "SHA256SUMS.txt"
PAYLOAD = b"not an update\n"

ap = argparse.ArgumentParser()
ap.add_argument("--exe", required=True)
ap.add_argument("--inis", required=True, help="inis dir of a set-up PCSX2 data dir (PCSX2.ini copied)")
ap.add_argument("--offer", default="zdxsv-999.0.0")
ap.add_argument("--no-auto-install", action="store_true")
ap.add_argument("--sums", choices=("ok", "bad", "missing", "host"), default="ok")
ap.add_argument("--timeout", type=float, default=60)
args = ap.parse_args()

base = "/inada-s/pcsx2/releases/download/%s/" % args.offer
digest = hashlib.sha256(PAYLOAD if args.sums != "bad" else b"another file\n").hexdigest()
sums_text = ("%s  %s\n%s  %s\n" % ("0" * 64, "pcsx2-zdxsv-windows-x64.zip", digest, ASSET)).encode()
requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests.append(self.path)
        if self.path == base + ASSET:
            body = PAYLOAD
        elif self.path == base + SUMS:
            body = sums_text
        else:
            host = "localhost" if args.sums == "host" else "127.0.0.1"
            assets = [{"name": ASSET, "size": len(PAYLOAD), "browser_download_url": "http://%s:%d%s%s" % (host, port, base, ASSET)}]
            if args.sums != "missing":
                assets.append({"name": SUMS, "size": len(sums_text), "browser_download_url": "http://127.0.0.1:%d%s%s" % (port, base, SUMS)})
            body = json.dumps([{"tag_name": args.offer, "draft": False, "prerelease": False,
                                "published_at": "2026-01-01T00:00:00Z", "assets": assets}]).encode()
        self.send_response(200)
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
updater = ["CheckAtStartup = true"] + (["ZdxsvAutoInstall = false"] if args.no_auto_install else [])
out, section = [], None
for line in open(src, encoding="utf-8-sig").read().splitlines():
    if line.startswith("["):
        section = line
    # startup check on, no skipped version; GS Null renderer
    if section == "[AutoUpdater]" and line.split("=")[0].strip() in ("CheckAtStartup", "LastVersion", "ZdxsvAutoInstall"):
        continue
    if section == "[EmuCore/GS]" and line.split("=")[0].strip() == "Renderer":
        line = "Renderer = 11"
    out.append(line)
    if line == "[AutoUpdater]":
        out += updater
if "[AutoUpdater]" not in out:
    out += ["", "[AutoUpdater]"] + updater
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
            break
if result == "Update needed.":
    # the change list comes from GitHub first; after the update download, its checksum result is logged
    wait_end = time.time() + (10 if args.no_auto_install else 40)
    while time.time() < wait_end and proc.poll() is None:
        time.sleep(0.5)
        text = open(log, encoding="utf-8", errors="replace").read()
        if base + ASSET in requests and "Download failed" not in text and text.count("Updater Error") > text.count("change list"):
            break
    time.sleep(1)
    text = open(log, encoding="utf-8", errors="replace").read()
proc.kill()
proc.wait()
server.shutdown()
for line in text.splitlines():
    if any(k in line for k in ("Current version:", "Latest version:", "pdate needed.", "Updater Error")):
        print(line.strip())
shutil.rmtree(data, ignore_errors=True)

lists = sum(1 for r in requests if not r.startswith(base))
got_sums = base + SUMS in requests
got_asset = base + ASSET in requests
refused_release = (args.offer + " refused:") in text
refused_update = "Update refused:" in text
if args.sums in ("missing", "host"):
    ok = lists == 1 and refused_release and not got_sums and not got_asset
elif args.no_auto_install:
    ok = lists == 1 and result == "Update needed." and not got_sums and not got_asset
else:
    ok = (lists == 1 and ("Latest version: " + args.offer) in text and result == "Update needed." and got_sums
          and got_asset and refused_update == (args.sums == "bad"))
print("release list requests %d, %s requested %s, update requested %s, release refused %s, update refused %s, result %s"
      % (lists, SUMS, got_sums, got_asset, refused_release, refused_update, result))
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
