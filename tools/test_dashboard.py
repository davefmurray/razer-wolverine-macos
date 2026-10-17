#!/usr/bin/env python3

import http.client
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("wolverine_dashboard", ROOT / "tools" / "dashboard.py")
dash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dash)

FIELDS = (
    "lx", "ly", "rx", "ry", "lt", "rt",
    "a", "b", "x", "y", "lb", "rb", "ls", "rs",
    "menu", "view", "guide", "du", "dd", "dl", "dr",
)

FAKE = r"""
import os, signal, sys, time
def _stop(signum, frame):
    os._exit(0)
signal.signal(signal.SIGINT, _stop)
signal.signal(signal.SIGTERM, _stop)
pidfile = os.environ.get("GIP_PIDFILE", "")
if pidfile:
    with open(pidfile, "a", encoding="utf-8") as fh:
        fh.write(str(os.getpid()) + "\n")
out = os.environ.get("GIP_OUT", "")
err = os.environ.get("GIP_ERR", "")
if out:
    sys.stdout.write(out if out.endswith("\n") else out + "\n")
    sys.stdout.flush()
if err:
    sys.stderr.write(err if err.endswith("\n") else err + "\n")
    sys.stderr.flush()
time.sleep(60)
"""

STUBBORN = r"""
import os, signal, time
signal.signal(signal.SIGINT, signal.SIG_IGN)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
pidfile = os.environ.get("GIP_PIDFILE", "")
if pidfile:
    with open(pidfile, "a", encoding="utf-8") as fh:
        fh.write(str(os.getpid()) + "\n")
time.sleep(60)
"""

OPEN = "opened 1532:0a3f, GIP interface #0, EP IN 0x81, EP OUT 0x02"
HELLO = "Hello: vid=1532 pid=0a3f fw=1.1.1026.0"
AUTH = "auth: handshake complete (session key derived)"
AUDIO = "could not claim audio interface #1 (ignored)"
LOST = "read error: LIBUSB_ERROR_NO_DEVICE -- device lost, waiting for reconnect..."


def packet(kind, **over):
    obj = {"kind": kind}
    for name in FIELDS:
        obj[name] = 0
    obj.update(over)
    return json.dumps(obj, separators=(",", ":"))



class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pidfile = Path(self.tmp.name) / "pids"
        self.caps = []
        self.addCleanup(self.stop_caps)
        self.addCleanup(self.tmp.cleanup)

    def stop_caps(self):
        for cap in self.caps:
            cap.stop()

    def env_for(self, out="", err=""):
        env = os.environ.copy()
        env["GIP_OUT"] = out
        env["GIP_ERR"] = err
        env["GIP_PIDFILE"] = str(self.pidfile)
        return env

    def make(self, out="", err="", grace=0.8, code=FAKE):
        cap = dash.Capture(
            [sys.executable, "-c", code],
            env=self.env_for(out, err),
            stop_grace=grace,
        )
        self.caps.append(cap)
        return cap

    def wait_until(self, pred, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.02)
        return False

    def test_idle_snapshot_has_no_readings(self):
        cap = dash.Capture(["/bin/echo"])
        self.caps.append(cap)
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "stopped")
        self.assertFalse(snap["capturing"])
        self.assertIsNone(snap["input"])
        self.assertIsNone(snap["guide"])
        self.assertEqual(snap["report_count"], 0)
        self.assertIsNone(snap["extrema"]["lx"]["min"])
        self.assertEqual(snap["service"], "wolverine-dashboard")
        self.assertEqual(dash.HOST, "127.0.0.1")
        self.assertEqual(dash.PORT, 8769)

    def test_status_without_stdout_is_not_a_reading(self):
        err = "\n".join([AUDIO, OPEN, HELLO, AUTH])
        cap = self.make(err=err)
        cap.start()
        self.assertTrue(self.wait_until(lambda: cap.snapshot()["handshake"] == "complete"))
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "waiting")
        self.assertEqual(snap["firmware"], "1.1.1026.0")
        self.assertEqual(snap["usb"], "open")
        self.assertEqual(snap["audio_note"], "Audio interface ignored")
        self.assertIsNone(snap["input"])
        self.assertIsNone(snap["guide"])
        self.assertEqual(snap["report_count"], 0)
        self.assertIsNone(snap["extrema"]["lx"]["min"])
        self.assertIsNone(snap["extrema"]["lt"]["max"])
        self.assertTrue(snap["capturing"])

    def test_failed_handshake_is_visible_and_can_recover(self):
        cap = self.make()
        cap.start()
        gen = cap.generation
        cap._on_line(gen, "stderr", OPEN)
        cap._on_line(gen, "stderr", "auth: processing failed (0x05)")
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "error")
        self.assertEqual(snap["handshake"], "failed")
        self.assertIn("retry", snap["error"])
        self.assertIsNone(snap["input"])
        cap._on_line(gen, "stderr", AUTH)
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "waiting")
        self.assertIsNone(snap["error"])
        cap._on_line(gen, "stdout", packet("input", lx=100))
        self.assertEqual(cap.snapshot()["input"]["lx"], 100)

    def test_guide_packet_does_not_count_or_invent_axes(self):
        out = packet("guide", guide=1, lx=-32768, ly=32767, lt=1023, a=1)
        err = "\n".join([OPEN, HELLO, AUTH])
        cap = self.make(out=out, err=err)
        cap.start()
        self.assertTrue(self.wait_until(lambda: cap.snapshot()["guide"] == 1 and cap.snapshot()["handshake"] == "complete"))
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "waiting")
        self.assertEqual(snap["handshake"], "complete")
        self.assertEqual(snap["report_count"], 0)
        self.assertIsNone(snap["input"])
        self.assertIsNone(snap["extrema"]["lx"]["min"])
        self.assertIsNone(snap["extrema"]["ly"]["max"])
        self.assertIsNone(snap["extrema"]["lt"]["max"])

    def test_input_counts_and_later_guide_does_not_move_axes(self):
        out = "\n".join([
            packet("input", lx=-32768, ly=10, a=1),
            packet("guide", guide=1, lx=5, ly=-9, lt=80),
        ])
        cap = self.make(out=out, err=OPEN)
        cap.start()
        self.assertTrue(self.wait_until(lambda: cap.snapshot()["guide"] == 1))
        snap = cap.snapshot()
        self.assertEqual(snap["report_count"], 1)
        self.assertEqual(snap["phase"], "live")
        self.assertEqual(snap["input"]["lx"], -32768)
        self.assertEqual(snap["input"]["ly"], 10)
        self.assertEqual(snap["input"]["a"], 1)
        self.assertEqual(snap["extrema"]["lx"], {"min": -32768, "max": -32768})
        self.assertEqual(snap["extrema"]["ly"], {"min": 10, "max": 10})
        self.assertEqual(snap["guide"], 1)

    def test_kindless_json_is_ignored(self):
        cap = self.make()
        cap.start()
        kindless = json.loads(packet("input", lx=4))
        del kindless["kind"]
        cap._on_line(cap.generation, "stdout", json.dumps(kindless))
        snap = cap.snapshot()
        self.assertEqual(snap["report_count"], 0)
        self.assertIsNone(snap["input"])

    def test_replug_clears_readings_until_new_input(self):
        cap = self.make()
        cap.start()
        gen = cap.generation
        cap._on_line(gen, "stderr", OPEN)
        cap._on_line(gen, "stdout", packet("input", lx=-32768, lt=1023))
        self.assertEqual(cap.snapshot()["report_count"], 1)
        cap._on_line(gen, "stderr", LOST)
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "reconnecting")
        self.assertEqual(snap["usb"], "lost")
        self.assertIsNone(snap["input"])
        self.assertIsNone(snap["guide"])
        self.assertIsNone(snap["extrema"]["lx"]["min"])
        self.assertEqual(snap["report_count"], 1)
        cap._on_line(gen, "stdout", packet("input", lx=99))
        self.assertIsNone(cap.snapshot()["input"])
        cap._on_line(gen, "stderr", OPEN)
        snap = cap.snapshot()
        self.assertEqual(snap["replug_count"], 1)
        self.assertEqual(snap["phase"], "waiting")
        self.assertEqual(snap["usb"], "open")
        self.assertIsNone(snap["input"])
        cap._on_line(gen, "stdout", packet("guide", guide=1, lx=-32768))
        self.assertEqual(cap.snapshot()["report_count"], 1)
        self.assertIsNone(cap.snapshot()["input"])
        cap._on_line(gen, "stdout", packet("input", lx=10))
        snap = cap.snapshot()
        self.assertEqual(snap["report_count"], 2)
        self.assertEqual(snap["phase"], "live")
        self.assertEqual(snap["extrema"]["lx"], {"min": 10, "max": 10})

    def test_audio_ignore_is_not_claim_failure(self):
        cap = self.make()
        cap.start()
        gen = cap.generation
        cap._on_line(gen, "stderr", OPEN)
        cap._on_line(gen, "stderr", AUDIO)
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "waiting")
        self.assertEqual(snap["usb"], "open")
        self.assertEqual(snap["audio_note"], "Audio interface ignored")
        cap._on_line(gen, "stdout", packet("input", lx=1))
        cap._on_line(gen, "stderr", "claim_interface failed: LIBUSB_ERROR_BUSY")
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "error")
        self.assertEqual(snap["usb"], "claim_failed")
        self.assertIsNone(snap["input"])
        cap2 = self.make()
        cap2.start()
        cap2._on_line(cap2.generation, "stderr", "device 1532:0a3f not found / could not open")
        snap = cap2.snapshot()
        self.assertEqual(snap["usb"], "absent")
        self.assertEqual(snap["phase"], "error")
        self.assertIsNone(snap["input"])

    def test_stop_and_restart_reset_session(self):
        cap = self.make()
        cap.start()
        for line in (OPEN, HELLO, AUTH):
            cap._on_line(cap.generation, "stderr", line)
        cap._on_line(cap.generation, "stdout", packet("input", lx=-5, ly=8))
        self.assertEqual(cap.snapshot()["firmware"], "1.1.1026.0")
        old = cap.generation
        cap.stop()
        snap = cap.snapshot()
        self.assertFalse(snap["capturing"])
        self.assertEqual(snap["phase"], "stopped")
        self.assertEqual(snap["usb"], "not_reading")
        self.assertIsNone(snap["input"])
        self.assertIsNone(snap["guide"])
        self.assertIsNone(snap["extrema"]["lx"]["min"])
        self.assertEqual(snap["report_count"], 1)
        cap.start()
        snap = cap.snapshot()
        self.assertTrue(snap["capturing"])
        self.assertEqual(snap["report_count"], 0)
        self.assertIsNone(snap["input"])
        self.assertIsNone(snap["firmware"])
        self.assertIsNone(snap["extrema"]["ly"]["min"])
        self.assertNotEqual(cap.generation, old)
        cap._on_line(old, "stdout", packet("input", lx=3, a=1))
        self.assertEqual(cap.snapshot()["report_count"], 0)
        self.assertIsNone(cap.snapshot()["input"])

    def test_duplicate_start_keeps_one_process(self):
        cap = self.make(out=packet("input", lx=4))
        cap.start()
        self.assertTrue(self.wait_until(lambda: cap.snapshot()["report_count"] == 1))
        pid = cap.proc.pid
        minimum = cap.snapshot()["extrema"]["lx"]["min"]
        cap.start()
        self.assertEqual(cap.proc.pid, pid)
        self.assertEqual(cap.snapshot()["report_count"], 1)
        self.assertEqual(cap.snapshot()["extrema"]["lx"]["min"], minimum)
        self.assertEqual(self.pidfile.read_text().split(), [str(pid)])

    def test_stop_kills_only_the_owned_child(self):
        cap = self.make(code=STUBBORN, grace=0.15)
        bystander = subprocess.Popen(["sleep", "30"])
        try:
            cap.start()
            pid = cap.proc.pid
            cap.stop()
            self.assertIsNone(cap.proc)
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
            self.assertIsNone(bystander.poll())
        finally:
            if bystander.poll() is None:
                bystander.kill()
                bystander.wait(timeout=2)

    def test_missing_reader_does_not_spawn(self):
        cap = dash.Capture(["/tmp/does-not-exist-wolverine-giptool"])
        self.caps.append(cap)
        cap.start()
        snap = cap.snapshot()
        self.assertEqual(snap["phase"], "error")
        self.assertIn("make", snap["error"])
        self.assertIsNone(snap["input"])
        self.assertIsNone(cap.proc)
        self.assertFalse(snap["capturing"])


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        env = os.environ.copy()
        env["GIP_PIDFILE"] = str(Path(self.tmp.name) / "pids")
        env["GIP_OUT"] = ""
        env["GIP_ERR"] = ""
        self.cap = dash.Capture([sys.executable, "-c", FAKE], env=env, stop_grace=0.4)
        self.httpd = dash.DashServer(("127.0.0.1", 0), self.cap)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.shutdown)

    def shutdown(self):
        self.httpd.shutdown()
        self.thread.join(timeout=2)
        self.httpd.server_close()
        self.cap.stop()

    def request(self, method, path, headers=None, body=b""):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        conn.request(method, path, body=body, headers=headers or {})
        res = conn.getresponse()
        data = res.read()
        conn.close()
        return res.status, data

    def local_headers(self, **extra):
        headers = {
            "Host": f"127.0.0.1:{self.port}",
            "Content-Type": "application/json",
            "Origin": f"http://127.0.0.1:{self.port}",
            "X-Wolverine-Request": "1",
        }
        headers.update(extra)
        return headers

    def test_state_and_pages(self):
        status, data = self.request("GET", "/api/state")
        self.assertEqual(status, 200)
        snap = json.loads(data)
        self.assertIsNone(snap["input"])
        self.assertEqual(snap["report_count"], 0)
        self.assertEqual(snap["phase"], "stopped")
        self.assertFalse(snap["capturing"])
        status, html = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b"Wolverine / Live input", html)
        self.assertIn(b"/dashboard.js", html)
        status, fixtures = self.request("GET", "/fixtures.html")
        self.assertEqual(status, 200)
        self.assertIn(b"Test fixtures", fixtures)
        self.assertNotIn(b"https://", html + fixtures)
        status, _ = self.request("GET", "/../README.md")
        self.assertEqual(status, 404)
        status, _ = self.request("GET", "/src/giptool.c")
        self.assertEqual(status, 404)

    def test_rejects_foreign_host_origin_and_plain_post(self):
        status, _ = self.request("GET", "/api/state", {"Host": "evil.example"})
        self.assertEqual(status, 403)
        status, _ = self.request("GET", "/api/state", {
            "Host": f"127.0.0.1:{self.port}",
            "Origin": "http://evil.example",
        })
        self.assertEqual(status, 403)
        status, _ = self.request("POST", "/api/start", {
            "Host": f"127.0.0.1:{self.port}",
            "Content-Type": "text/plain",
        }, b"{}")
        self.assertEqual(status, 403)
        status, _ = self.request("POST", "/api/start", {
            "Host": f"127.0.0.1:{self.port}",
            "Content-Type": "application/json",
            "Origin": "http://evil.example",
            "X-Wolverine-Request": "1",
        }, b"{}")
        self.assertEqual(status, 403)
        status, _ = self.request("POST", "/api/start", {
            "Host": f"127.0.0.1:{self.port}",
            "Content-Type": "application/json",
            "Origin": f"http://127.0.0.1:{self.port}",
            "Sec-Fetch-Site": "cross-site",
            "X-Wolverine-Request": "1",
        }, b"{}")
        self.assertEqual(status, 403)
        self.assertIsNone(self.cap.proc)
        self.assertFalse(self.cap.snapshot()["capturing"])

    def test_start_is_idempotent_and_stop_clears_live_state(self):
        status, data = self.request("POST", "/api/start", self.local_headers(), b"{}")
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(data)["capturing"])
        self.assertIsNone(json.loads(data)["input"])
        pid = self.cap.proc.pid
        status, _ = self.request("POST", "/api/start", {
            "Host": f"127.0.0.1:{self.port}",
            "X-Wolverine-Request": "1",
        }, b"")
        self.assertEqual(status, 200)
        self.assertEqual(self.cap.proc.pid, pid)
        self.cap._on_line(self.cap.generation, "stdout", packet("input", lx=7, a=1))
        self.assertEqual(self.cap.snapshot()["report_count"], 1)
        status, data = self.request("POST", "/api/stop", self.local_headers(), b"{}")
        self.assertEqual(status, 200)
        snap = json.loads(data)
        self.assertFalse(snap["capturing"])
        self.assertEqual(snap["phase"], "stopped")
        self.assertIsNone(snap["input"])
        self.assertIsNone(snap["extrema"]["lx"]["min"])


class ContractTests(unittest.TestCase):
    def test_js_fixtures_do_not_invent_live_controls(self):
        jsc = find_jsc()
        if not jsc:
            self.skipTest("jsc not installed")
        script = (
            'load("%s");'
            "var rows = globalThis.FIXTURES.map(function (fx) {"
            " var p = globalThis.presentation(fx.snapshot);"
            " return {label: fx.label, mode: p.mode, sticks: p.sticks, guideLit: p.guideLit, a: !!p.lit.a, reports: p.reportCount};"
            "});"
            "print(JSON.stringify({rows: rows, neg: globalThis.stickPercent(-32768), pos: globalThis.stickPercent(32767), trig: globalThis.triggerPercent(1023)}));"
        ) % (ROOT / "web" / "dashboard.js")
        done = subprocess.run([jsc, "-e", script], check=True, capture_output=True, text=True)
        payload = json.loads(done.stdout)
        self.assertEqual(payload["neg"], -100)
        self.assertEqual(payload["pos"], 100)
        self.assertEqual(payload["trig"], 100)
        rows = {row["label"]: row for row in payload["rows"]}
        self.assertEqual(rows["No data"]["sticks"], "blank")
        self.assertFalse(rows["No data"]["guideLit"])
        self.assertEqual(rows["No data"]["reports"], 0)
        self.assertEqual(rows["Guide only"]["sticks"], "blank")
        self.assertTrue(rows["Guide only"]["guideLit"])
        self.assertFalse(rows["Guide only"]["a"])
        self.assertEqual(rows["Guide only"]["reports"], 0)
        self.assertEqual(rows["Live input"]["sticks"], "live")
        self.assertTrue(rows["Live input"]["a"])
        self.assertEqual(rows["Live input"]["mode"], "live")
        self.assertEqual(rows["Last received"]["sticks"], "held")
        self.assertFalse(rows["Last received"]["a"])
        self.assertFalse(rows["Last received"]["guideLit"])
        self.assertEqual(rows["Link lost"]["mode"], "down")
        self.assertFalse(rows["Link lost"]["a"])
        self.assertEqual(rows["Replug"]["sticks"], "blank")
        self.assertEqual(rows["Replug"]["reports"], 4)


def find_jsc():
    candidates = [
        Path("/usr/bin/jsc"),
        Path("/System/Library/Frameworks/JavaScriptCore.framework/Helpers/jsc"),
        Path("/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc"),
    ]
    for path in candidates:
        if path.is_file():
            return str(path)
    return None


if __name__ == "__main__":
    unittest.main()
