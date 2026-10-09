# apiserve.py PORT FILE: stand-in for the lobby's public /lbs/replay (rplay.sh API=1). Any battle_code gets a JSON list
# whose replay_url (/pb) serves FILE; FILE=none: an answer without replay_url.
import http.server, json, sys

port, pb = int(sys.argv[1]), sys.argv[2]


class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/lbs/replay"):
            rec = {"battle_code": self.path.split("=")[-1]}
            if pb != "none":
                rec["replay_url"] = "http://127.0.0.1:%d/pb" % port
            body = json.dumps([rec]).encode()
        elif self.path == "/pb" and pb != "none":
            body = open(pb, "rb").read()
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
