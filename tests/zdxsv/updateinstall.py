"""Install an update with the macOS updater of a release build (pcsx2-qt AutoUpdaterDialog).

  python updateinstall.py --app PCSX2.app --tag zdxsv-X.Y.Z --inis DIR [--offer zdxsv-X.Y.Z] [--timeout S]

--app is a Devel build at the zdxsv-X.Y.Z tag --tag (only a tagged build checks for updates). It is
copied as PCSX2-<tag>.app into a new directory, and a second copy with a marker file is packed as the
offered update. A local server answers the release list (ZDXSV_UPDATE_URL) with --offer, which must
be newer than --tag. ZDXSV_UPDATE_INSTALL=1 presses Download and Install once the change list from
GitHub has loaded or failed (an unknown tag or the API rate limit only logs an error).
Pass: the old bundle is gone, PCSX2-<offer>.app holds the marker, has no quarantine attribute and
runs. Control: --offer older than --tag must fail ("No update needed.").
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

ASSET = "pcsx2-zdxsv-macos.tar.xz"

ap = argparse.ArgumentParser()
ap.add_argument("--app", required=True)
ap.add_argument("--tag", required=True, help="the zdxsv-X.Y.Z tag the build is at")
ap.add_argument("--inis", required=True, help="inis dir of a set-up PCSX2 data dir (PCSX2.ini copied)")
ap.add_argument("--offer", default="zdxsv-1.0.4")
ap.add_argument("--timeout", type=float, default=120)
args = ap.parse_args()

if sys.platform != "darwin":
    print("FAIL: macOS only")
    sys.exit(2)
src_ini = os.path.join(args.inis, "PCSX2.ini")
if not os.path.isdir(args.app) or not os.path.isfile(src_ini):
    print("FAIL: no %s or %s" % (args.app, src_ini))
    sys.exit(2)

work = tempfile.mkdtemp(prefix="updateinstall-")
inst = os.path.join(work, "inst")
old_app = os.path.join(inst, "PCSX2-%s.app" % args.tag)
new_app = os.path.join(inst, "PCSX2-%s.app" % args.offer)
pkg_app = os.path.join(work, "pkg", "PCSX2-%s.app" % args.offer)
marker = os.path.join("Contents", "Resources", "update-marker")


def copy_signed(dst, mark):
    shutil.copytree(args.app, dst, symlinks=True)
    if mark:
        open(os.path.join(dst, marker), "w").write(args.offer)
    subprocess.run(["codesign", "-s", "-", "--force", "--deep", dst], check=True, capture_output=True)


copy_signed(old_app, False)
copy_signed(pkg_app, True)
asset = os.path.join(work, ASSET)
subprocess.run(["tar", "-cJf", asset, "-C", os.path.dirname(pkg_app), os.path.basename(pkg_app)],
               check=True, env=dict(os.environ, XZ_OPT="-1"))
data = open(asset, "rb").read()

requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests.append(self.path)
        if self.path == "/releases":
            body = json.dumps([{
                "tag_name": args.offer, "draft": False, "prerelease": False, "published_at": "2026-01-01T00:00:00Z",
                "body": "", "assets": [{"name": ASSET, "size": len(data),
                                        "browser_download_url": "http://127.0.0.1:%d/asset" % port}],
            }]).encode()
        elif self.path == "/asset":
            body = data
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

datadir = os.path.join(work, "data")
os.makedirs(os.path.join(datadir, "PCSX2", "inis"))
out, section = [], None
for line in open(src_ini, encoding="utf-8-sig").read().splitlines():
    if line.startswith("["):
        section = line
    if section == "[AutoUpdater]" and line.split("=")[0].strip() in ("CheckAtStartup", "LastVersion"):
        continue
    out.append(line)
    if line == "[AutoUpdater]":
        out.append("CheckAtStartup = true")
if "[AutoUpdater]" not in out:
    out += ["", "[AutoUpdater]", "CheckAtStartup = true"]
open(os.path.join(datadir, "PCSX2", "inis", "PCSX2.ini"), "w", encoding="utf-8").write("\n".join(out) + "\n")
log = os.path.join(datadir, "PCSX2", "logs", "emulog.txt")

env = dict(os.environ, ZDXSV_UPDATE_URL="http://127.0.0.1:%d/releases" % port, ZDXSV_UPDATE_INSTALL="1")
proc = subprocess.Popen([os.path.join(old_app, "Contents", "MacOS", "PCSX2"), "-datapath", datadir], env=env)


def new_pids():
    r = subprocess.run(["pgrep", "-f", os.path.join(new_app, "Contents", "MacOS", "PCSX2")],
                       capture_output=True, text=True)
    return r.stdout.split()


text, running = "", []
end = time.time() + args.timeout
while time.time() < end:
    time.sleep(1)
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.isfile(log) else ""
    running = new_pids()
    errors = [l for l in text.splitlines() if "Updater Error" in l and "change list" not in l]
    if running or errors or "No update needed." in text:
        break
time.sleep(2)
running = new_pids()
proc.kill()
proc.wait()
for pid in running:
    subprocess.run(["kill", "-9", pid])
server.shutdown()

for line in text.splitlines():
    if any(k in line for k in ("Latest version:", "pdate needed.", "Updater Error")):
        print(line.strip())
has_marker = os.path.isfile(os.path.join(new_app, marker))
quarantine = subprocess.run(["xattr", "-p", "com.apple.quarantine", new_app], capture_output=True).returncode == 0
old_gone = not os.path.exists(old_app)
print("downloads %d, old bundle gone %s, new bundle marker %s, quarantine %s, new process %s" %
      (requests.count("/asset"), old_gone, has_marker, quarantine, bool(running)))
shutil.rmtree(work, ignore_errors=True)

ok = old_gone and has_marker and not quarantine and bool(running)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
