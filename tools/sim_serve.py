"""Serve the browser simulator, and fetch what was recently played for it.

The page can't ask Music Assistant itself (it needs the token, and Music
Assistant doesn't answer web pages), so this server does: GET /recent returns
the latest recently played tracks, resolved as tools/sim_data.py does, from
Music Assistant (read-only). The token stays here. POST /shot/<name>.png, from
this Mac only, saves a screenshot of the knob's screen into sim/build/shots
(for flow maps). Everything else is the built page and its copied library.

Usage: python3 -I tools/sim_serve.py <web dir> [port]
"""
import functools
import http.server
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sim_data  # noqa: E402

WEB = Path(sys.argv[1]).resolve()
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8360


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.split("?")[0] != "/recent":
            return super().do_GET()
        try:
            playlists = json.loads((WEB / "data" / "library.json").read_text()).get("playlists") or []
            body, status = json.dumps(sim_data.recent(playlists), ensure_ascii=False).encode(), 200
        except Exception as e:  # noqa: BLE001
            body, status = json.dumps({"error": str(e)}).encode(), 502
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


    def do_POST(self):
        name = self.path.split("?")[0].removeprefix("/shot/")
        ok = self.client_address[0] == "127.0.0.1" and self.path.startswith("/shot/") and name.endswith(".png") \
            and all(c.isalnum() or c in "-_." for c in name)
        if not ok:
            self.send_error(403)
            return
        shots = WEB.parent / "shots"
        shots.mkdir(exist_ok=True)
        (shots / name).write_bytes(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        self.send_response(204)
        self.end_headers()


if __name__ == "__main__":
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), functools.partial(Handler, directory=str(WEB)))
    print(f"Serving {WEB} on port {PORT}, with /recent from Music Assistant")
    server.serve_forever()
