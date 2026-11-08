"""Houston's PC side: gathers GPU, printer, Claude Code and calendar state and sends it to the
display over Wi-Fi.

Only what the screen shows leaves this program: numbers, a print's file name, a session's title
and the next event's name and time. Prompts and tool input arrive with Claude Code's hooks and are
dropped on the spot. Each source fails on its own and shows as unknown on the display, never as old
numbers.

    python houston_helper.py          run (pythonw at login)
    python houston_helper.py --once   gather once, print the message, send nothing
"""

import argparse
import json
import logging
import os
import re
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unicodedata
import urllib.request
import zlib
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from zoneinfo import ZoneInfo

import calendar_ics

DISPLAY_HOST = "houston.local"
DISPLAY_PORT = 4210
HOOK_PORT = 8767
PRINTER_PORT = 8883
PRINTER_USER = "bblp"
SETTINGS_PATH = Path.home() / ".claude" / "settings.json"
CONFIG_PATH = Path(__file__).resolve().parent / "houston.local.json"  # git-ignored

HEARTBEAT_S = 2.0  # the display shows "--" after 15 s without one
GPU_EVERY_S = 5.0  # light query, but a game is running: not more often
HOOKS_EVERY_S = 60.0
SESSION_QUIET_S = 30 * 60  # a session silent this long stops counting
TEXT_MAX = 23  # the display's text fields, minus the terminator
MAX_HOOK_BYTES = 16 << 20
TITLE_TAIL_BYTES = 1 << 20  # transcripts reach 100 MB: only their end is read
CALENDAR_EVERY_S = 600.0
CALENDAR_RETRY_S = 60.0
CALENDAR_KEEP_S = 6 * 3600.0  # last good answer while the feed fails, then nothing
MAX_CALENDAR_BYTES = 8 << 20
NEXT_HORIZON = timedelta(hours=24)  # the line under the digits shows a time without a date
SOON = timedelta(minutes=10)
PRINT_UNKNOWN = {"pk": 0, "ps": "", "pp": 0, "pm": -1, "pf": "", "pw": ""}

log = logging.getLogger("houston_helper")


def load_config(path=CONFIG_PATH):
    """The settings file, or nothing. Errors by type only: the file holds secrets."""
    try:
        # Notepad may add a byte order mark
        config = json.loads(path.read_text(encoding="utf-8-sig"))
    except OSError:
        return {}
    except ValueError as error:
        log.warning("settings file: %s", type(error).__name__)
        return {}
    return config if isinstance(config, dict) else {}


def local_zone(config):
    """The configured IANA zone, otherwise this PC's current offset."""
    name = config.get("timezone")
    if name:
        try:
            return ZoneInfo(name)
        except (KeyError, ValueError, OSError) as error:
            log.warning("timezone: %s", type(error).__name__)
    return datetime.now().astimezone().tzinfo


def screen_text(text, limit=TEXT_MAX):
    """Upper-case ASCII that fits the display's fields. Accents fold to their letter, anything else
    becomes a space."""
    folded = (
        unicodedata.normalize("NFKD", text or "").encode("ascii", "ignore").decode("ascii").upper()
    )
    cleaned = "".join(c if " " <= c <= "~" else " " for c in folded)
    return " ".join(cleaned.split())[:limit].rstrip()


class GameDetector:
    """Sustained GPU load means a game (or a render: the temperature matters then too)."""

    ON_LOAD, OFF_LOAD = 50, 20
    ON_S, OFF_S = 20.0, 60.0

    def __init__(self):
        self.gaming = False
        self.unreadable = False
        self.high_since = None
        self.low_since = None

    def update(self, load, now):
        if load >= self.ON_LOAD:
            self.low_since = None
            self.high_since = now if self.high_since is None else self.high_since
            if now - self.high_since >= self.ON_S:
                self.gaming = True
        elif load < self.OFF_LOAD:
            self.high_since = None
            self.low_since = now if self.low_since is None else self.low_since
            if now - self.low_since >= self.OFF_S:
                self.gaming = False
        else:
            self.high_since = self.low_since = None
        return self.gaming


def read_gpu():
    """(temperature, load). No console window: one flashing up would knock a full-screen game
    out."""
    done = subprocess.run(
        [
            "nvidia-smi",
            "--query-gpu=temperature.gpu,utilization.gpu",
            "--format=csv,noheader,nounits",
        ],
        capture_output=True,
        text=True,
        timeout=5,
        check=True,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    temperature, load = done.stdout.splitlines()[0].split(",")
    return round(float(temperature)), round(float(load))


class PrinterLink:
    """Status from a Bambu printer on the local network, over its MQTT port. Asks once for the full
    status on connecting, as some models only report changes; it sends nothing else, so it can't
    start, stop or change anything. Errors by type only: the access code is a secret."""

    KEPT = ("gcode_state", "mc_percent", "mc_remaining_time", "subtask_name")

    def __init__(self, host, serial, access_code):
        self.host, self.serial, self.access_code = host, serial, access_code
        self.lock = threading.Lock()
        self.status = {}
        self.client = None

    def start(self):
        import paho.mqtt.client as mqtt  # only needed with a printer in the settings

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.username_pw_set(PRINTER_USER, self.access_code)
        # The printer's certificate is self-signed, so its name can't be checked
        self.client.tls_set(cert_reqs=ssl.CERT_NONE)
        self.client.tls_insecure_set(True)
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message
        self.client.connect_async(self.host, PRINTER_PORT, keepalive=60)
        self.client.loop_start()

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.warning("printer: connection refused")
            return
        log.info("printer: connected")
        client.subscribe(f"device/{self.serial}/report")
        client.publish(
            f"device/{self.serial}/request",
            json.dumps({"pushing": {"sequence_id": "0", "command": "pushall"}}),
        )

    def on_disconnect(self, client, userdata, flags, reason_code, properties):
        log.info("printer: disconnected")
        with self.lock:
            self.status = {}

    def on_message(self, client, userdata, message):
        self.update(message.payload)

    def update(self, payload):
        """Reports can carry only what changed, so they add to what's known."""
        try:
            report = json.loads(payload).get("print")
        except (ValueError, AttributeError):
            return
        if not isinstance(report, dict):
            return
        with self.lock:
            self.status.update({k: report[k] for k in self.KEPT if k in report})

    def snapshot(self):
        with self.lock:
            return dict(self.status)


def print_summary(status):
    state = str((status or {}).get("gcode_state") or "").upper()
    if not state:
        return dict(PRINT_UNKNOWN)
    percent = max(0, min(100, int(status.get("mc_percent") or 0)))
    name = re.sub(
        r"(\.gcode)?(\.3mf)?$", "", str(status.get("subtask_name") or ""), flags=re.IGNORECASE
    )
    out = {"pk": 1, "ps": "idle", "pp": percent, "pm": -1, "pf": screen_text(name), "pw": ""}
    if state in ("RUNNING", "PREPARE", "SLICING"):
        minutes = int(status.get("mc_remaining_time") or 0)
        out.update(ps="run", pm=minutes if minutes > 0 else -1)
    elif state in ("PAUSE", "PAUSED"):
        out.update(ps="stop", pw=f"PAUSED {percent} PERCENT")
    elif state == "FAILED":
        out.update(ps="stop", pw=f"FAILED {percent} PERCENT")
    elif state == "FINISH":
        out.update(ps="done")
    return out


def start_printer(config):
    """The printer link when the settings have one, else None: the lamp then shows "--"."""
    settings = config.get("printer") or {}
    host, serial, code = (settings.get(k) for k in ("host", "serial", "access_code"))
    if not (host and serial and code):
        return None
    printer = PrinterLink(host, serial, code)
    try:
        printer.start()
    except Exception as error:
        log.warning("printer: %s", type(error).__name__)
        return None
    return printer


def title_from_transcript(path):
    """A session's title: the last custom-title record near the transcript's end. Nothing else is
    kept."""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - TITLE_TAIL_BYTES))
            tail = f.read()
    except (OSError, TypeError):
        return None
    for line in reversed(tail.splitlines()):
        if b'"custom-title"' in line:
            try:
                return json.loads(line).get("customTitle")
            except ValueError:
                continue
    return None


class ClaudeTracker:
    """Sessions from Claude Code's hooks. Keeps a session id, a state, a title and when it last
    spoke, nothing more.

    There is no hook for "approved": after a yes a session counts as waiting until that tool
    finishes.
    """

    NEEDS_NOTIFICATIONS = {"permission_prompt", "elicitation_dialog", "agent_needs_input"}
    WAITING = {"needs", "finished", "error"}

    def __init__(self, read_title=title_from_transcript):
        self.lock = threading.Lock()
        self.sessions = {}
        self.ask_id = 0
        self.ask_title = ""
        self.read_title = read_title

    def state_after(self, payload):
        event = payload.get("hook_event_name")
        if event in ("UserPromptSubmit", "PostToolUse", "PostToolUseFailure"):
            return "working"
        if event == "PreToolUse":
            return "needs" if payload.get("tool_name") == "AskUserQuestion" else "working"
        if event == "PermissionRequest":
            return "needs"
        if event == "Notification":
            return "needs" if payload.get("notification_type") in self.NEEDS_NOTIFICATIONS else None
        return {"Stop": "finished", "StopFailure": "error"}.get(event)

    def event(self, payload, now):
        session_id = payload.get("session_id")
        if not session_id:
            return
        with self.lock:
            if payload.get("hook_event_name") == "SessionEnd":
                self.sessions.pop(session_id, None)
                return
            session = self.sessions.setdefault(session_id, {"state": "working", "seen": now})
            session["seen"] = now
            state = self.state_after(payload)
            if state is None:
                return
            if state == "needs" and session["state"] != "needs":
                # Event names only; titles stay out of the log
                log.info(
                    "claude: needs you (%s %s)",
                    payload.get("hook_event_name"),
                    payload.get("notification_type") or "",
                )
                title = (
                    self.read_title(payload.get("transcript_path"))
                    or Path(str(payload.get("cwd") or "")).name
                )
                self.ask_id += 1
                self.ask_title = screen_text(title) or "A SESSION"
            session["state"] = state

    def summary(self, hooks_installed, now):
        if not hooks_installed:
            return {"ck": 0, "cn": 0, "ci": 0, "ca": ""}
        with self.lock:
            for session_id in [
                k for k, s in self.sessions.items() if now - s["seen"] > SESSION_QUIET_S
            ]:
                del self.sessions[session_id]
            count = sum(s["state"] in self.WAITING for s in self.sessions.values())
            return {"ck": 1, "cn": count, "ci": self.ask_id, "ca": self.ask_title}


def hooks_installed(path=SETTINGS_PATH):
    try:
        return f"127.0.0.1:{HOOK_PORT}" in path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return False


class HookHandler(BaseHTTPRequestHandler):
    tracker = None

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length <= MAX_HOOK_BYTES else None
        # A web page can post to localhost too, but it always sends an Origin; Claude Code's hooks
        # don't
        if body is not None and self.path == "/hook" and "Origin" not in self.headers:
            try:
                self.tracker.event(json.loads(body), time.monotonic())
            except (ValueError, AttributeError):
                pass
        self.close_connection = body is None
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *args):
        pass


def start_hook_server(tracker, port=HOOK_PORT):
    handler = type("BoundHookHandler", (HookHandler,), {"tracker": tracker})
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def next_event_summary(events, now):
    """The next timed event in the coming day. All-day and cancelled ones aren't appointments to
    pause for."""
    upcoming = [
        e
        for e in events
        if not (e.cancelled or e.declined or e.all_day) and now < e.start <= now + NEXT_HORIZON
    ]
    if not upcoming:
        return {"na": "", "ns": 0, "ni": 0, "nn": ""}
    event = min(upcoming, key=lambda e: (e.start, e.title))
    event_id = zlib.crc32(f"{event.start.isoformat()}|{event.title}".encode()) & 0x7FFFFFFF
    return {
        "na": event.start.astimezone(now.tzinfo).strftime("%H:%M"),
        "ns": int(event.start - now <= SOON),
        "ni": event_id,
        "nn": screen_text(event.title),
    }


def calendar_link(value):
    link = str(value or "").strip()
    return re.sub(r"^webcal://", "https://", link, flags=re.IGNORECASE) or None


class CalendarSource(threading.Thread):
    """Fetches the feed on its own thread. Errors are logged by type only: the link is a secret."""

    def __init__(self, link, zone):
        super().__init__(daemon=True)
        self.link = calendar_link(link)
        self.zone = zone
        self.lock = threading.Lock()
        self.events = None
        self.fetched_at = 0.0

    def fetch(self):
        now = datetime.now(self.zone)
        with urllib.request.urlopen(self.link, timeout=10) as response:
            text = response.read(MAX_CALENDAR_BYTES).decode("utf-8", "replace")
        result = calendar_ics.parse(
            text, now - timedelta(days=1), now + timedelta(days=3), self.zone
        )
        # A partly read feed can't prove what's next
        if result.unsupported or result.malformed:
            raise ValueError(
                f"feed partly unreadable: {result.unsupported} unsupported, "
                f"{result.malformed} malformed"
            )
        return result.events

    def run(self):
        while self.link:
            wait = CALENDAR_EVERY_S
            try:
                events = self.fetch()
                with self.lock:
                    self.events, self.fetched_at = events, time.monotonic()
            except ValueError as error:
                log.warning("calendar: %s", error)
                wait = CALENDAR_RETRY_S
            except Exception as error:
                log.warning("calendar: %s", type(error).__name__)
                wait = CALENDAR_RETRY_S
            time.sleep(wait)

    def summary(self):
        with self.lock:
            events = self.events if time.monotonic() - self.fetched_at < CALENDAR_KEEP_S else None
        return next_event_summary(events or [], datetime.now(self.zone))


def encode(game, gpu, printing, claude, upcoming):
    message = {"v": 1, "game": int(game), "gpu": gpu, **printing, **claude, **upcoming}
    return json.dumps(message, separators=(",", ":"), ensure_ascii=True)


class Sender:
    """UDP to the display by name: outgoing, so Windows' firewall isn't involved, and only the
    display receives it."""

    RESOLVE_EVERY_S, RETRY_S = 60.0, 15.0

    def __init__(self, host=DISPLAY_HOST, port=DISPLAY_PORT):
        self.host, self.port = host, port
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.address = None
        self.resolve_at = 0.0
        self.failing = None

    def send(self, message, now):
        try:
            if now >= self.resolve_at:
                self.resolve_at = now + self.RETRY_S
                self.address = socket.gethostbyname(self.host)
                self.resolve_at = now + self.RESOLVE_EVERY_S
            if self.address:
                self.socket.sendto(message.encode("ascii"), (self.address, self.port))
            self.failing = None
        except OSError as error:
            if self.failing != type(error).__name__:
                self.failing = type(error).__name__
                log.warning("sending to the display: %s", self.failing)


def gather_print(printer):
    try:
        return print_summary(printer.snapshot() if printer else None)
    except Exception as error:
        log.debug("printer: %s", type(error).__name__)
        return dict(PRINT_UNKNOWN)


def gather_gpu(detector, now):
    try:
        temperature, load = read_gpu()
    except Exception as error:
        if not detector.unreadable:  # once per outage, not every 5 s
            log.warning("gpu: %s", type(error).__name__)
        detector.unreadable = True
        return -1, False
    detector.unreadable = False
    return temperature, detector.update(load, now)


def once():
    now = time.monotonic()
    config = load_config()
    calendar = CalendarSource(config.get("calendar_url"), local_zone(config))
    if calendar.link:
        try:
            calendar.events, calendar.fetched_at = calendar.fetch(), now
        except Exception as error:
            print("calendar failed:", type(error).__name__)
    printer = start_printer(config)
    if printer:
        time.sleep(5)  # the printer's first report
    gpu, game = gather_gpu(GameDetector(), now)
    claude = ClaudeTracker().summary(hooks_installed(), now)
    print(encode(game, gpu, gather_print(printer), claude, calendar.summary()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--once", action="store_true", help="gather once, print the message, send nothing"
    )
    if parser.parse_args().once:
        return once()
    logging.basicConfig(
        filename=os.path.join(tempfile.gettempdir(), "houston_helper.log"),
        level=logging.INFO,
        format="%(asctime)s %(message)s",
    )
    config = load_config()
    tracker = ClaudeTracker()
    start_hook_server(tracker)  # fails if another helper already runs: one is enough
    calendar = CalendarSource(config.get("calendar_url"), local_zone(config))
    calendar.start()
    printer = start_printer(config)
    sender = Sender()
    detector = GameDetector()
    gpu, game, installed = -1, False, False
    gpu_at = hooks_at = sent_at = 0.0
    last = None
    log.info("started")
    while True:
        now = time.monotonic()
        if now >= gpu_at:
            gpu_at = now + GPU_EVERY_S
            gpu, game = gather_gpu(detector, now)
        if now >= hooks_at:
            hooks_at = now + HOOKS_EVERY_S
            installed = hooks_installed()
        message = encode(
            game, gpu, gather_print(printer), tracker.summary(installed, now), calendar.summary()
        )
        if message != last or now - sent_at >= HEARTBEAT_S:
            sender.send(message, now)
            last, sent_at = message, now
        time.sleep(0.25)


if __name__ == "__main__":
    main()
