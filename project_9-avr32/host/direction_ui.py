#!/usr/bin/env python3
"""
Live LEFT / RIGHT display for the `direction` firmware (apps/direction).

    python3 host/direction_ui.py               # finds the board, opens your browser
    python3 host/direction_ui.py --demo        # no board: made-up events, to try the UI
    python3 host/direction_ui.py --port /dev/cu.usbmodem1101

How it fits together:
    board --USB serial--> this script --(local web server)--> browser page
The board does all the deciding. This script only reads its text lines (format
documented at the top of apps/direction/main.c), turns them into JSON and pushes
them to the page with Server-Sent Events. It also scores clap tests and writes
every event to a CSV file in host/logs/.

Needs Python 3.8+ and nothing else on macOS/Linux. If pyserial is installed
(pip3 install pyserial) it is used; otherwise the port is opened with the POSIX
termios module. Windows needs pyserial.

Only one program can hold the serial port: close `screen` before starting this.
"""
import argparse
import csv
import glob
import json
import math
import os
import queue
import random
import select
import sys
import threading
import time
import webbrowser
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    import serial                      # pyserial
    from serial.tools import list_ports
    HAVE_PYSERIAL = hasattr(serial, "Serial")
except ImportError:
    HAVE_PYSERIAL = False

HERE = Path(__file__).resolve().parent
PAGE = HERE / "direction_ui.html"

ATMEL_VID = 0x03EB
APP_PID = 0x2404                       # our firmware's USB CDC product ID
SIDES = ("LEFT", "RIGHT", "CENTRE")


# ---------------------------------------------------------------------------
# Parsing the board's lines
# ---------------------------------------------------------------------------

def _value(text):
    """'+812' -> 812, '-' -> None, 'RIGHT' -> 'RIGHT'."""
    if text == "-":
        return None
    try:
        return int(text)
    except ValueError:
        return text


def parse_line(line):
    """One line from the board -> dict with a 'kind', or None for human text."""
    words = line.split()
    if not words:
        return None
    key = words[0]
    try:
        if key == "EVT" or key == "CFG":
            fields = dict(w.split("=", 1) for w in words[1:])
            data = {k: _value(v) for k, v in fields.items()}
            if key == "EVT":
                data["m"] = [data.pop("m%d" % i, None) for i in range(1, 5)]
                data["agree"] = bool(data.get("agree"))
            return {"kind": key.lower(), **data}
        if key in ("LVL", "BASE") and len(words) == 5:
            return {"kind": key.lower(), "mics": [int(w) for w in words[1:]]}
        if key == "ARMED":
            return {"kind": "armed"}
    except ValueError:
        pass                            # half a line at connect time, etc.
    return None


# ---------------------------------------------------------------------------
# Shared state, fanned out to every open browser tab
# ---------------------------------------------------------------------------

class Hub:
    def __init__(self, log_dir):
        self.lock = threading.Lock()
        self.clients = set()
        self.log_dir = Path(log_dir)
        self.csv_file = None
        self.csv = None
        self.link = {"state": "starting", "port": None, "detail": ""}
        self.cfg = {}
        self.base = None
        self.expected = None            # ground truth for the next sounds, or None
        self.score = {s: [0, 0] for s in SIDES}   # side -> [hits, tries]
        self.history = deque(maxlen=40)
        self.raw = deque(maxlen=300)
        self.armed = False

    # -- fan-out ------------------------------------------------------------
    def subscribe(self):
        q = queue.Queue(maxsize=500)
        with self.lock:
            self.clients.add(q)
            q.put(self._snapshot())
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.clients.discard(q)

    def _publish(self, msg):
        for q in list(self.clients):
            try:
                q.put_nowait(msg)
            except queue.Full:          # a stalled tab; it resyncs on reconnect
                pass

    def _snapshot(self):
        return {"kind": "snapshot", "link": self.link, "cfg": self.cfg,
                "base": self.base, "expected": self.expected, "score": self.score,
                "history": list(self.history), "lines": list(self.raw),
                "armed": self.armed, "log": self.csv_file}

    # -- inputs -------------------------------------------------------------
    def set_link(self, state, port=None, detail=""):
        with self.lock:
            if (self.link["state"], self.link["port"], self.link["detail"]) == (state, port, detail):
                return
            self.link = {"state": state, "port": port, "detail": detail}
            if state != "connected" and state != "demo":
                self.armed = False
            self._publish({"kind": "link", **self.link})

    def set_expected(self, side):
        with self.lock:
            self.expected = side if side in SIDES else None
            self._publish({"kind": "expected", "expected": self.expected})

    def reset_score(self):
        with self.lock:
            self.score = {s: [0, 0] for s in SIDES}
            self.history.clear()
            self._publish({"kind": "score", "score": self.score, "cleared": True})

    def handle_line(self, line):
        line = line.strip()
        if not line:
            return
        msg = parse_line(line)
        with self.lock:
            self.raw.append(line)
            out = {"kind": "raw", "text": line}
            if msg is None:
                self._publish(out)
                return
            kind = msg["kind"]
            if kind == "cfg":
                self.cfg = msg
            elif kind == "base":
                self.base = msg["mics"]
            elif kind == "armed":
                self.armed = True
            elif kind == "evt":
                self.armed = False
                msg["time"] = datetime.now().strftime("%H:%M:%S")
                msg["expected"] = self.expected
                msg["hit"] = None
                if self.expected:
                    msg["hit"] = msg.get("side") == self.expected
                    self.score[self.expected][1] += 1
                    self.score[self.expected][0] += int(msg["hit"])
                msg["score"] = self.score
                self.history.append(msg)
                self._log_event(msg)
            msg["raw"] = line
            self._publish(msg)

    # -- CSV log ------------------------------------------------------------
    def _log_event(self, e):
        if self.csv is None:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            name = self.log_dir / datetime.now().strftime("direction_%Y%m%d_%H%M%S.csv")
            self.csv_fh = open(name, "w", newline="")
            self.csv = csv.writer(self.csv_fh)
            self.csv.writerow(["time", "n", "expected", "side", "hit", "dt_us", "up_us",
                               "down_us", "pairs", "agree", "first", "m1_us", "m2_us",
                               "m3_us", "m4_us"])
            self.csv_file = str(name)
            self._publish({"kind": "log", "log": self.csv_file})
        blank = lambda v: "" if v is None else v
        self.csv.writerow([e["time"], e.get("n"), blank(e["expected"]), e.get("side"),
                           blank(e["hit"]), blank(e.get("dt")), blank(e.get("up")),
                           blank(e.get("down")), e.get("pairs"), int(e["agree"]),
                           e.get("first")] + [blank(v) for v in e["m"]])
        self.csv_fh.flush()


# ---------------------------------------------------------------------------
# Serial port
# ---------------------------------------------------------------------------

def find_port():
    """The board's serial device, or None if it isn't plugged in / running our app."""
    if HAVE_PYSERIAL:
        for p in list_ports.comports():
            if p.vid == ATMEL_VID and p.pid == APP_PID:
                return p.device
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


class PySerialLink:
    def __init__(self, port):
        # Opening raises DTR, which is what tells the firmware a terminal is there.
        self.s = serial.Serial(port, 115200, timeout=0.2)

    def read(self):
        return self.s.read(4096)

    def close(self):
        self.s.close()


class PosixLink:
    """Minimal serial reader without pyserial (macOS / Linux)."""

    def __init__(self, port):
        import fcntl
        import termios
        import tty
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            tty.setraw(self.fd)                       # no echo, no line editing
            attrs = termios.tcgetattr(self.fd)
            attrs[4] = attrs[5] = termios.B115200     # ignored by USB CDC, but harmless
            attrs[2] |= termios.CLOCAL | termios.CREAD
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            if hasattr(termios, "TIOCMBIS"):          # make sure DTR is up
                import struct
                fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR))
        except OSError:
            pass                                      # e.g. a pty in tests
        flags = fcntl.fcntl(self.fd, fcntl.F_GETFL)
        fcntl.fcntl(self.fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)

    def read(self):
        ready, _, _ = select.select([self.fd], [], [], 0.2)
        if not ready:
            return b""
        data = os.read(self.fd, 4096)
        if not data:
            raise OSError("serial port closed")       # unplugged
        return data

    def close(self):
        os.close(self.fd)


def open_link(port):
    if HAVE_PYSERIAL:
        return PySerialLink(port)
    if os.name != "posix":
        sys.exit("On Windows, install pyserial first:  pip install pyserial")
    return PosixLink(port)


def serial_loop(hub, port_arg, stop):
    """Read lines forever; when the board goes away (reset, reflash, unplug),
    keep looking for it and reconnect."""
    while not stop.is_set():
        port = port_arg or find_port()
        if not port:
            hub.set_link("searching", None, "no board found")
            stop.wait(1.0)
            continue
        try:
            link = open_link(port)
        except (OSError, ValueError) as e:
            hub.set_link("searching", port, str(e))
            stop.wait(1.0)
            continue
        hub.set_link("connected", port)
        pending = b""
        try:
            while not stop.is_set():
                pending += link.read()
                *lines, pending = pending.split(b"\n")
                for raw in lines:
                    hub.handle_line(raw.decode("ascii", "replace"))
        except Exception as e:          # pyserial raises SerialException on unplug
            hub.set_link("searching", port, "lost: %s" % e)
        finally:
            link.close()
        stop.wait(1.0)


# ---------------------------------------------------------------------------
# Demo mode: a fake board that prints the same lines as the firmware
# ---------------------------------------------------------------------------

def decide(t_us, centre_us=100, max_us=1700):
    """Python copy of side_decide() in apps/direction/side.c (demo only).
    t_us: arrival per mic (index = mic - 1, mic n on ADn), None if it never crossed.
    Corners: AD1 up-left, AD2 up-right, AD3 down-left, AD4 down-right."""
    heard = [t for t in t_us if t is not None]
    first = min(range(4), key=lambda m: t_us[m] if t_us[m] is not None else 1e9)
    ok = [t is not None and t - t_us[first] <= max_us for t in t_us]
    up = t_us[0] - t_us[1] if ok[0] and ok[1] else None        # left minus right
    down = t_us[2] - t_us[3] if ok[2] and ok[3] else None
    pairs = [p for p in (up, down) if p is not None]
    cls = lambda dt: "RIGHT" if dt > centre_us else "LEFT" if dt < -centre_us else "CENTRE"
    dt = round(sum(pairs) / len(pairs)) if pairs else None
    side = cls(dt) if pairs else ("RIGHT" if first in (1, 3) else "LEFT")
    agree = len(pairs) == 2 and cls(up) == cls(down)
    return dict(side=side, dt=dt, up=up, down=down, pairs=len(pairs),
                agree=agree, first=first + 1 if heard else 0)


def fake_board(emit, stop, hold_ms=1500):
    def fmt(v, signed=True):
        return "-" if v is None else ("%+d" % v if signed else "%d" % v)

    emit("# Project_9 direction: DEMO, made-up events (no board connected)")
    emit("CFG fs=48000 rate=48000 thr=150 centre_us=100 max_us=1700 hold_ms=%d" % hold_ms)
    emit("BASE 381 382 383 381")
    emit("ARMED")
    rng = random.Random()
    width, depth, c = 0.35, 0.45, 343.0
    mics = [(-width / 2, depth / 2), (width / 2, depth / 2),       # AD1, AD2 (up)
            (-width / 2, -depth / 2), (width / 2, -depth / 2)]     # AD3, AD4 (down)
    n, loud, next_evt, armed_at = 0, [0.0] * 4, time.time() + 2.0, None
    while not stop.wait(0.1):
        now = time.time()
        if armed_at and now >= armed_at:
            emit("ARMED")
            armed_at = None
        if armed_at is None and now >= next_evt:
            n += 1
            ang = math.radians(rng.choice([-1, 1]) * rng.uniform(10, 170)
                               if rng.random() > 0.12 else rng.uniform(-6, 6))
            sx, sy = 1.0 * math.sin(ang), 1.0 * math.cos(ang)
            t = [math.hypot(sx - x, sy - y) / c * 1e6 for x, y in mics]
            t = [round(v - min(t) + rng.gauss(0, 25)) for v in t]
            if rng.random() < 0.08:                   # a mic that never crossed
                t[rng.randrange(4)] = None
            elif rng.random() < 0.06:                 # a late (AGC) or echo crossing
                t[rng.randrange(4)] += rng.choice([400, 1900])
            base = min(v for v in t if v is not None)
            t = [None if v is None else v - base for v in t]
            r = decide(t)
            emit("EVT n=%d side=%s dt=%s up=%s down=%s pairs=%d agree=%d first=%d %s"
                 % (n, r["side"], fmt(r["dt"]), fmt(r["up"]), fmt(r["down"]),
                    r["pairs"], r["agree"], r["first"],
                    " ".join("m%d=%s" % (i + 1, fmt(v, False)) for i, v in enumerate(t))))
            loud = [380.0 if v is not None else 120.0 for v in t]
            armed_at = now + hold_ms / 1000
            next_evt = now + rng.uniform(2.2, 4.0)
        loud = [v * 0.72 for v in loud]
        emit("LVL %s" % " ".join(str(int(40 + rng.uniform(0, 20) + v)) for v in loud))


# ---------------------------------------------------------------------------
# Web server
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    hub = None   # set in main()

    def log_message(self, *args):
        pass     # keep the terminal quiet

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            # Read on every request, so edits to the page show up on reload.
            self._send(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        elif self.path == "/events":
            self._events()
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            body = {}
        if self.path == "/expect":
            self.hub.set_expected(body.get("side"))
        elif self.path == "/reset":
            self.hub.reset_score()
        else:
            return self._send(404, b"not found", "text/plain")
        self._send(204, b"", "text/plain")

    def _events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        q = self.hub.subscribe()
        try:
            while True:
                try:
                    msg = q.get(timeout=15)
                    self.wfile.write(b"data: " + json.dumps(msg).encode() + b"\n\n")
                except queue.Empty:
                    self.wfile.write(b": keep-alive\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.hub.unsubscribe(q)


def start_server(port):
    for p in range(port, port + 10):
        try:
            server = ThreadingHTTPServer(("127.0.0.1", p), Handler)
            server.daemon_threads = True
            return server
        except OSError:
            continue
    sys.exit("No free local port in %d-%d" % (port, port + 9))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", help="serial device (default: find the board)")
    ap.add_argument("--demo", action="store_true", help="no board: made-up events")
    ap.add_argument("--http-port", type=int, default=8765, help="local web port")
    ap.add_argument("--no-browser", action="store_true", help="don't open a browser")
    ap.add_argument("--log-dir", default=str(HERE / "logs"), help="where CSV logs go")
    args = ap.parse_args()

    hub = Hub(args.log_dir)
    Handler.hub = hub
    server = start_server(args.http_port)
    url = "http://127.0.0.1:%d/" % server.server_address[1]

    stop = threading.Event()
    if args.demo:
        hub.set_link("demo", None, "made-up events")
        worker = threading.Thread(target=fake_board, args=(hub.handle_line, stop), daemon=True)
    else:
        if not HAVE_PYSERIAL:
            print("pyserial not installed; using the built-in POSIX serial reader.")
        worker = threading.Thread(target=serial_loop, args=(hub, args.port, stop), daemon=True)
    worker.start()

    print("Direction UI on %s  (Ctrl-C to stop)" % url)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        stop.set()
        server.server_close()


if __name__ == "__main__":
    main()
