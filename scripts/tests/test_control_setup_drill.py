import copy, json, subprocess, sys, threading, unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "control-setup-drill.py"
INITIAL_STATE = {"inputs": [{"slot": 1, "sourceId": None}], "previewSceneId": "", "activeSceneId": ""}
INITIAL_SNAPSHOT = {"programFrame": {"sceneId": "", "sceneIdAttribution": "unknown"}, "recording": {"lifecycle": {"state": "idle"}}}

class Stub(BaseHTTPRequestHandler):
    state = copy.deepcopy(INITIAL_STATE)
    snapshot = copy.deepcopy(INITIAL_SNAPSHOT)
    break_take = False
    stale_program = False
    def log_message(self, *a): pass
    def _send(self, code, body):
        data = json.dumps(body).encode(); self.send_response(code)
        self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        if self.path == "/state": return self._send(200, Stub.state)
        if self.path == "/snapshot": return self._send(200, {"ok": True, "available": True, "stale": False, "snapshot": Stub.snapshot})
        self._send(404, {"ok": False, "error": "Not found"})
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        a, args = body["action"], body.get("args", [])
        if a == "input.assign": Stub.state["inputs"][0]["sourceId"] = args[1]
        if a == "scene.select": Stub.state["previewSceneId"] = args[0]
        if a == "transport.take" and not Stub.break_take:
            Stub.state["activeSceneId"] = Stub.state["previewSceneId"]
            if not Stub.stale_program:
                Stub.snapshot["programFrame"] = {"sceneId": Stub.state["activeSceneId"], "sceneIdAttribution": "live"}
        if a == "transport.record.set" and args[0] is False: Stub.snapshot["recording"] = {"lifecycle": {"state": "completed"}}
        self._send(200, {"ok": True})

class DrillTests(unittest.TestCase):
    def setUp(self):
        Stub.state = copy.deepcopy(INITIAL_STATE)
        Stub.snapshot = copy.deepcopy(INITIAL_SNAPSHOT)
        Stub.break_take = False
        Stub.stale_program = False
    def run_drill(self):
        server = HTTPServer(("127.0.0.1", 0), Stub); t = threading.Thread(target=server.serve_forever, daemon=True); t.start()
        try:
            return subprocess.run([sys.executable, "-I", str(SCRIPT), "--base", f"http://127.0.0.1:{server.server_port}",
                                   "--slot", "1", "--source", "capture:cam1", "--scene", "s3", "--record-seconds", "0"],
                                  capture_output=True, text=True, timeout=60)
        finally:
            server.shutdown()
    def test_all_steps_pass_against_a_conforming_host(self):
        r = self.run_drill(); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
    def test_a_take_that_never_lands_fails_naming_the_step(self):
        Stub.break_take = True
        r = self.run_drill(); self.assertEqual(r.returncode, 1); self.assertIn("transport.take", r.stdout)
    def test_a_take_the_core_never_attributes_fails(self):
        # /state says the Take happened; the core's programFrame never moved. The drill must not believe /state alone.
        Stub.stale_program = True
        r = self.run_drill(); self.assertEqual(r.returncode, 1); self.assertIn("FAIL transport.take", r.stdout)
    def test_an_unreachable_base_is_a_connection_error(self):
        r = subprocess.run([sys.executable, "-I", str(SCRIPT), "--base", "http://127.0.0.1:1", "--slot", "1",
                            "--source", "capture:x", "--scene", "s"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)

if __name__ == "__main__": unittest.main()
