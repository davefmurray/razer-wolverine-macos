#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
READER = ROOT / "build" / "giptool"
HOST = "127.0.0.1"
PORT = 8769
STALE_MS = 400
LOG_LIMIT = 80
STOP_GRACE = 1.0

AXES = ("lx", "ly", "rx", "ry", "lt", "rt")
BUTTONS = ("a", "b", "x", "y", "lb", "rb", "ls", "rs", "menu", "view", "du", "dd", "dl", "dr")
FILES = {
    "/": ("dashboard.html", "text/html; charset=utf-8"),
    "/dashboard.html": ("dashboard.html", "text/html; charset=utf-8"),
    "/dashboard.js": ("dashboard.js", "text/javascript; charset=utf-8"),
    "/dashboard.css": ("dashboard.css", "text/css; charset=utf-8"),
    "/fixtures.html": ("fixtures.html", "text/html; charset=utf-8"),
}
_FW = re.compile(r"fw=(\d+\.\d+\.\d+\.\d+)")


def blank_extrema():
    return {name: {"min": None, "max": None} for name in AXES}


def host_matches(headers, port):
    return headers.get("Host", "") == f"{HOST}:{port}"


def origin_matches(headers, port):
    origin = headers.get("Origin")
    return not origin or origin == f"http://{HOST}:{port}"


def read_allowed(headers, port):
    return host_matches(headers, port) and origin_matches(headers, port)


def mutation_allowed(headers, port):
    if not read_allowed(headers, port):
        return False
    site = headers.get("Sec-Fetch-Site")
    if site and site not in ("same-origin", "none"):
        return False
    ctype = headers.get("Content-Type", "")
    custom = headers.get("X-Wolverine-Request", "")
    return "application/json" in ctype or custom == "1"


class Capture:
    def __init__(self, argv=None, env=None, stop_grace=STOP_GRACE):
        self.argv = list(argv) if argv else [str(READER), "--json"]
        self.env = env
        self.stop_grace = stop_grace
        self.life = threading.Lock()
        self.state = threading.Lock()
        self.proc = None
        self.threads = []
        self.generation = 0
        self._ended = 0
        self._stop_requested = False
        self.accepting = False
        self.capturing = False
        self.phase = "stopped"
        self.usb = "not_reading"
        self.usb_detail = ""
        self.audio_note = ""
        self.handshake = "idle"
        self.firmware = None
        self.report_count = 0
        self.input = None
        self.guide = None
        self.last_input_mono = None
        self.last_guide_mono = None
        self.extrema = blank_extrema()
        self.logs = deque(maxlen=LOG_LIMIT)
        self.exit_code = None
        self.error = None
        self.replug_count = 0

    def start(self):
        with self.life:
            with self.state:
                proc = self.proc
                alive = bool(self.capturing and proc is not None and proc.poll() is None)
            if alive:
                return
            self._stop_body()
            program = Path(self.argv[0])
            if program.is_absolute() and not program.is_file():
                with self.state:
                    self.phase = "error"
                    self.error = "Reader not built. Run make."
                    self.capturing = False
                    self.accepting = False
                    self.usb = "not_reading"
                return
            with self.state:
                self.generation += 1
                gen = self.generation
                self._reset_session()
                self.capturing = True
                self.accepting = True
                self.phase = "starting"
            try:
                proc = subprocess.Popen(
                    self.argv,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    stdin=subprocess.DEVNULL,
                    env=self.env,
                    cwd=str(ROOT),
                    start_new_session=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    bufsize=1,
                )
            except OSError as exc:
                with self.state:
                    self.phase = "error"
                    self.error = str(exc)
                    self.capturing = False
                    self.accepting = False
                return
            threads = [
                threading.Thread(target=self._pump, args=(proc.stdout, gen, proc, "stdout"), name="gip-stdout", daemon=True),
                threading.Thread(target=self._pump, args=(proc.stderr, gen, proc, "stderr"), name="gip-stderr", daemon=True),
            ]
            with self.state:
                self.proc = proc
                self.threads = threads
                self._log("server", "capture started")
            for thread in threads:
                thread.start()

    def stop(self):
        with self.life:
            self._stop_body()

    def _stop_body(self):
        with self.state:
            proc = self.proc
            threads = list(self.threads)
            was = proc is not None or self.capturing
            self.proc = None
            self.threads = []
            self._stop_requested = True
            self.accepting = False
            self.capturing = False
            self._clear_readings(True)
            self.phase = "stopped"
            self.usb = "not_reading"
            self.usb_detail = ""
            if was:
                self._log("server", "capture stopped")
        if proc is not None and proc.poll() is None:
            self._reap(proc)
        elif proc is not None:
            try:
                proc.wait(timeout=0.2)
            except subprocess.TimeoutExpired:
                self._reap(proc)
        for thread in threads:
            thread.join(timeout=max(1.0, self.stop_grace))
            if thread.is_alive():
                print(f"reader thread {thread.name} still running", file=sys.stderr)
        with self.state:
            if proc is not None:
                self.exit_code = proc.returncode

    def _reap(self, proc):
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
            if proc.poll() is not None:
                return
            try:
                os.kill(proc.pid, sig)
            except ProcessLookupError:
                return
            try:
                proc.wait(timeout=self.stop_grace)
                return
            except subprocess.TimeoutExpired:
                continue

    def _pump(self, stream, gen, proc, name):
        try:
            for line in stream:
                self._on_line(gen, name, line)
        finally:
            try:
                stream.close()
            except Exception:
                pass
            self._stream_ended(gen, proc)

    def _stream_ended(self, gen, proc):
        with self.state:
            if gen != self.generation:
                return
            self._ended += 1
            finished = self._ended >= 2
        if not finished:
            return
        try:
            proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
        with self.state:
            if gen != self.generation or self._stop_requested:
                return
            self.exit_code = proc.returncode
            self.capturing = False
            self.accepting = False
            if self.proc is proc:
                self.proc = None
            self._clear_readings(True)
            if self.phase == "error" or proc.returncode not in (0, None):
                self.phase = "error"
                self.usb = "not_reading"
                if proc.returncode not in (0, None):
                    self.error = self.error or f"reader exited {proc.returncode}"
            else:
                self.phase = "stopped"
                self.usb = "not_reading"
                self.usb_detail = ""

    def _on_line(self, gen, stream, line):
        with self.state:
            if gen != self.generation or not self.accepting:
                return
            if stream == "stdout":
                self._apply_stdout(line)
            else:
                self._apply_stderr(line)

    def _apply_stdout(self, line):
        text = line.strip()
        if not text:
            return
        if not text.startswith("{"):
            self._log("stdout", text)
            return
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            self._log("stdout", "malformed json", "warn")
            return
        if not isinstance(obj, dict):
            self._log("stdout", "ignored non-object json", "warn")
            return
        kind = obj.get("kind")
        if kind == "input":
            self._apply_input(obj)
        elif kind == "guide":
            self._apply_guide(obj)
        else:
            self._log("stdout", "ignored message without input or guide kind", "warn")

    def _blocked(self):
        return self.phase in ("error", "reconnecting") or self.usb in ("lost", "claim_failed", "absent")

    def _apply_input(self, obj):
        if self._blocked():
            self._log("stdout", "ignored input while " + self.phase, "warn")
            return
        for key in AXES + BUTTONS:
            if type(obj.get(key)) is not int:
                self._log("stdout", "ignored input missing " + key, "warn")
                return
        self.input = {key: obj[key] for key in AXES + BUTTONS}
        now = time.monotonic()
        if type(obj.get("guide")) is int:
            self.guide = obj["guide"]
            self.input["guide"] = obj["guide"]
            self.last_guide_mono = now
        self.report_count += 1
        self.last_input_mono = now
        for key in AXES:
            value = obj[key]
            slot = self.extrema[key]
            if slot["min"] is None or value < slot["min"]:
                slot["min"] = value
            if slot["max"] is None or value > slot["max"]:
                slot["max"] = value
        self.phase = "live"
        self.error = None

    def _apply_guide(self, obj):
        if self._blocked():
            self._log("stdout", "ignored guide while " + self.phase, "warn")
            return
        if type(obj.get("guide")) is not int:
            self._log("stdout", "ignored guide without an integer guide field", "warn")
            return
        self.guide = obj["guide"]
        self.last_guide_mono = time.monotonic()

    def _apply_stderr(self, line):
        text = line.strip()
        if not text:
            return
        low = text.lower()
        if "audio interface" in low:
            if "ignored" in low or "error" in low:
                self.audio_note = "Audio interface ignored"
                self._log("stderr", text, "warn")
            else:
                self.audio_note = ""
                self._log("stderr", text)
            return
        if "claim_interface failed" in text or text.startswith("no GIP interface"):
            self.usb = "claim_failed"
            self.usb_detail = ""
            self.phase = "error"
            self.error = text
            self._clear_readings(True)
            self._log("stderr", text, "error")
            return
        if "not found / could not open" in text or text == "libusb_init failed":
            self.usb = "absent"
            self.usb_detail = ""
            self.phase = "error"
            self.error = text
            self._clear_readings(True)
            self._log("stderr", text, "error")
            return
        if "device lost" in low:
            self.usb = "lost"
            self.usb_detail = "waiting for reconnect"
            self.phase = "reconnecting"
            self.handshake = "idle"
            self._clear_readings(True)
            self._log("stderr", text, "warn")
            return
        if text.startswith("opened "):
            replug = self.usb == "lost" or self.phase == "reconnecting"
            self.usb = "open"
            self.usb_detail = text.split(",", 1)[0].replace("opened ", "", 1)
            self.error = None
            if replug:
                self.replug_count += 1
                self._clear_readings(True)
                self.phase = "waiting"
                self._log("server", "replug detected, readings cleared", "warn")
            elif self.phase != "live":
                self.phase = "waiting"
            self._log("stderr", text)
            return
        found = _FW.search(text)
        if found:
            self.firmware = found.group(1)
        level = "info"
        if "auth: starting" in low:
            self.handshake = "starting"
            if self.phase in ("starting", "stopped"):
                self.phase = "waiting"
        elif "handshake complete" in low:
            if self.handshake == "failed":
                self.phase = "waiting"
                self.error = None
            self.handshake = "complete"
            if self.report_count == 0 and self.phase not in ("error", "reconnecting", "live"):
                self.phase = "waiting"
        elif "auth: processing failed" in low:
            self.handshake = "failed"
            level = "error"
            self.phase = "error"
            self.error = "Controller handshake failed. Press Stop, then Start reading to retry."
            self._clear_readings(True)
        self._log("stderr", text, level)

    def _clear_readings(self, keep_count):
        self.input = None
        self.guide = None
        self.last_input_mono = None
        self.last_guide_mono = None
        self.extrema = blank_extrema()
        if not keep_count:
            self.report_count = 0

    def _reset_session(self):
        self._clear_readings(False)
        self.logs.clear()
        self.firmware = None
        self.handshake = "idle"
        self.audio_note = ""
        self.replug_count = 0
        self.error = None
        self.exit_code = None
        self.usb = "not_reading"
        self.usb_detail = ""
        self._ended = 0
        self._stop_requested = False

    def _log(self, stream, text, level="info"):
        self.logs.append({"t": time.strftime("%H:%M:%S"), "level": level, "stream": stream, "text": text[:400]})

    def snapshot(self):
        with self.state:
            now = time.monotonic()
            def age(stamp):
                if stamp is None:
                    return None
                return max(0, int((now - stamp) * 1000))
            return {
                "service": "wolverine-dashboard",
                "phase": self.phase,
                "capturing": self.capturing,
                "usb": self.usb,
                "usb_detail": self.usb_detail,
                "audio_note": self.audio_note,
                "handshake": self.handshake,
                "firmware": self.firmware,
                "report_count": self.report_count,
                "last_input_age_ms": age(self.last_input_mono),
                "last_guide_age_ms": age(self.last_guide_mono),
                "input": None if self.input is None else dict(self.input),
                "guide": self.guide,
                "extrema": {key: dict(value) for key, value in self.extrema.items()},
                "logs": [dict(item) for item in self.logs],
                "exit_code": self.exit_code,
                "error": self.error,
                "replug_count": self.replug_count,
                "link": "ok",
                "stale_after_ms": STALE_MS,
            }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        return

    def _port(self):
        return self.server.server_address[1]

    def do_GET(self):
        if not read_allowed(self.headers, self._port()):
            self._json(403, {"error": "rejected"})
            return
        path = urlsplit(self.path).path
        if path == "/api/state":
            self._json(200, self.server.capture.snapshot())
            return
        item = FILES.get(path)
        if not item:
            self._json(404, {"error": "not found"})
            return
        self._file(item[0], item[1])

    def do_POST(self):
        if not mutation_allowed(self.headers, self._port()):
            self._discard_body()
            self._json(403, {"error": "rejected"})
            return
        if self._discard_body() is None:
            self._json(400, {"error": "body too large"})
            return
        path = urlsplit(self.path).path
        capture = self.server.capture
        if path == "/api/start":
            capture.start()
            self._json(200, capture.snapshot())
        elif path == "/api/stop":
            capture.stop()
            self._json(200, capture.snapshot())
        else:
            self._json(404, {"error": "not found"})

    def _discard_body(self):
        raw = self.headers.get("Content-Length", "0")
        try:
            length = int(raw)
        except ValueError:
            length = 0
        if length < 0 or length > 4096:
            return None
        if length:
            self.rfile.read(length)
        return b""

    def _json(self, code, obj):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(data)

    def _file(self, name, content_type):
        path = (WEB / name).resolve()
        if path.parent != WEB.resolve() or not path.is_file():
            self._json(404, {"error": "not found"})
            return
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(data)


class DashServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, capture):
        self.capture = capture
        super().__init__(address, Handler)


def serve(capture, host=HOST, port=PORT):
    if host != HOST:
        raise SystemExit("dashboard binds 127.0.0.1 only")
    try:
        httpd = DashServer((host, port), capture)
    except OSError as exc:
        print(f"Could not bind {host}:{port}: {exc}", file=sys.stderr)
        return None
    return httpd


def main():
    capture = Capture()
    httpd = serve(capture, HOST, PORT)
    if httpd is None:
        return 1
    print(f"Wolverine dashboard at http://{HOST}:{PORT}", flush=True)
    print("Capture is stopped. Press Start reading in the browser. Ctrl-C stops the server.", flush=True)
    once = threading.Lock()

    def _sig(signum, frame):
        if not once.acquire(blocking=False):
            return
        threading.Thread(target=lambda: (capture.stop(), httpd.shutdown()), daemon=True).start()

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)
    try:
        signal.signal(signal.SIGHUP, _sig)
    except (ValueError, OSError):
        pass
    try:
        httpd.serve_forever()
    finally:
        capture.stop()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
