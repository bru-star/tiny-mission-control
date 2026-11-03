"""Tests for the helper's logic and its message to the display. No GPU, printer or display needed.

    python -m unittest test_houston_helper
"""

import json
import socket
import tempfile
import unittest
import urllib.request
from datetime import UTC, datetime, timedelta
from pathlib import Path
from types import SimpleNamespace

import houston_helper as h


class ScreenText(unittest.TestCase):
    def test_folds_and_fits(self):
        self.assertEqual(h.screen_text("Zo\u00eb  M\u00fcller"), "ZOE MULLER")
        self.assertEqual(h.screen_text("Call \U0001f389 Sam"), "CALL SAM")
        self.assertEqual(len(h.screen_text("x" * 80)), h.TEXT_MAX)
        self.assertEqual(h.screen_text(None), "")


class Settings(unittest.TestCase):
    def test_missing_or_broken_file_is_no_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "settings.json"
            self.assertEqual(h.load_config(path), {})
            path.write_text("not json", encoding="utf-8")
            self.assertEqual(h.load_config(path), {})
            path.write_text("[1]", encoding="utf-8")
            self.assertEqual(h.load_config(path), {})
            path.write_text('\ufeff{"timezone": "UTC"}', encoding="utf-8")
            self.assertEqual(h.load_config(path), {"timezone": "UTC"})

    def test_a_bad_zone_falls_back_to_this_pc(self):
        self.assertIsNotNone(h.local_zone({"timezone": "No/Such_Zone"}))
        self.assertIsNotNone(h.local_zone({}))
        self.assertEqual(str(h.local_zone({"timezone": "UTC"})), "UTC")


class Game(unittest.TestCase):
    def test_needs_sustained_load_both_ways(self):
        d = h.GameDetector()
        self.assertFalse(d.update(90, 0))
        self.assertFalse(d.update(90, 19))
        self.assertTrue(d.update(90, 20))
        self.assertTrue(d.update(5, 21))  # a loading screen doesn't end the game
        self.assertTrue(d.update(5, 80))
        self.assertFalse(d.update(5, 81))

    def test_a_spike_is_not_a_game(self):
        d = h.GameDetector()
        d.update(90, 0)
        d.update(30, 10)  # in between: the wait starts over
        self.assertFalse(d.update(90, 25))


class Printing(unittest.TestCase):
    def status(self, **kw):
        return {
            "gcode_state": "RUNNING",
            "mc_percent": 0,
            "mc_remaining_time": 0,
            "subtask_name": "test_part.gcode.3mf",
            **kw,
        }

    def test_unknown_until_the_printer_says(self):
        self.assertEqual(h.print_summary(None)["pk"], 0)
        self.assertEqual(h.print_summary({})["pk"], 0)
        self.assertEqual(h.print_summary({"mc_percent": 5})["pk"], 0)

    def test_states(self):
        self.assertEqual(h.print_summary(self.status(gcode_state="IDLE"))["ps"], "idle")
        self.assertEqual(h.print_summary(self.status(gcode_state="FINISH"))["ps"], "done")
        paused = h.print_summary(self.status(gcode_state="PAUSE", mc_percent=62))
        self.assertEqual(
            (paused["ps"], paused["pw"], paused["pf"]), ("stop", "PAUSED 62 PERCENT", "TEST_PART")
        )
        self.assertEqual(h.print_summary(self.status(gcode_state="FAILED"))["ps"], "stop")

    def test_minutes_come_from_the_printer(self):
        running = h.print_summary(self.status(mc_percent=62, mc_remaining_time=42))
        self.assertEqual((running["ps"], running["pp"], running["pm"]), ("run", 62, 42))
        self.assertEqual(h.print_summary(self.status(mc_remaining_time=0))["pm"], -1)

    def test_a_broken_report_is_unknown_not_a_crash(self):
        printer = SimpleNamespace(snapshot=lambda: self.status(mc_percent="abc"))
        self.assertEqual(h.gather_print(printer), h.PRINT_UNKNOWN)
        self.assertEqual(h.gather_print(None), h.PRINT_UNKNOWN)


class Printer(unittest.TestCase):
    def link(self):
        return h.PrinterLink("192.0.2.1", "SERIAL", "CODE")

    def test_reports_add_to_what_is_known(self):
        p = self.link()
        p.update(json.dumps({"print": {"gcode_state": "RUNNING", "mc_percent": 1}}).encode())
        p.update(json.dumps({"print": {"mc_percent": 2}}).encode())
        self.assertEqual(p.snapshot(), {"gcode_state": "RUNNING", "mc_percent": 2})

    def test_keeps_only_what_the_screen_needs(self):
        p = self.link()
        p.update(json.dumps({"print": {"mc_percent": 2, "wifi_signal": "-40dBm"}}).encode())
        self.assertEqual(p.snapshot(), {"mc_percent": 2})

    def test_ignores_other_messages_and_rubbish(self):
        p = self.link()
        for payload in (b"not json", b"[1]", b'{"print": 1}', b'{"info": {}}'):
            p.update(payload)
        self.assertEqual(p.snapshot(), {})

    def test_forgets_everything_on_disconnect(self):
        p = self.link()
        p.update(json.dumps({"print": {"gcode_state": "RUNNING"}}).encode())
        p.on_disconnect(None, None, None, None, None)
        self.assertEqual(h.print_summary(p.snapshot())["pk"], 0)

    def test_on_connect_only_subscribes_and_asks_for_status_once(self):
        sent = []
        client = SimpleNamespace(
            subscribe=lambda topic: sent.append(("subscribe", topic)),
            publish=lambda topic, body: sent.append((topic, json.loads(body))),
        )
        self.link().on_connect(client, None, None, SimpleNamespace(is_failure=False), None)
        self.assertEqual(
            sent,
            [
                ("subscribe", "device/SERIAL/report"),
                (
                    "device/SERIAL/request",
                    {"pushing": {"sequence_id": "0", "command": "pushall"}},
                ),
            ],
        )

    def test_a_refused_connection_sends_nothing(self):
        client = SimpleNamespace()  # any call on it would raise
        self.link().on_connect(client, None, None, SimpleNamespace(is_failure=True), None)

    def test_no_printer_in_the_settings(self):
        self.assertIsNone(h.start_printer({}))
        self.assertIsNone(h.start_printer({"printer": {"host": "192.0.2.1"}}))


class Claude(unittest.TestCase):
    def tracker(self):
        return h.ClaudeTracker(read_title=lambda path: "Some project")

    def post(self, t, event, now=0, session="s1", **kw):
        t.event({"session_id": session, "hook_event_name": event, **kw}, now)

    def test_counts_and_clears(self):
        t = self.tracker()
        self.post(t, "UserPromptSubmit")
        self.assertEqual(t.summary(True, 0)["cn"], 0)
        self.post(t, "PermissionRequest", tool_name="Bash")
        s = t.summary(True, 0)
        self.assertEqual((s["cn"], s["ci"], s["ca"]), (1, 1, "SOME PROJECT"))
        self.post(t, "PostToolUse", tool_name="Bash")
        self.assertEqual(t.summary(True, 0)["cn"], 0)
        self.post(t, "Stop")
        self.assertEqual(t.summary(True, 0)["cn"], 1)
        self.post(t, "SessionEnd")
        self.assertEqual(t.summary(True, 0)["cn"], 0)

    def test_what_means_needs_you(self):
        for kw in (
            {"hook_event_name": "PreToolUse", "tool_name": "AskUserQuestion"},
            {"hook_event_name": "Notification", "notification_type": "permission_prompt"},
            {"hook_event_name": "Notification", "notification_type": "elicitation_dialog"},
        ):
            t = self.tracker()
            t.event({"session_id": "s", **kw}, 0)
            self.assertEqual(t.summary(True, 0)["cn"], 1, kw)
        t = self.tracker()
        self.post(t, "Notification", notification_type="idle_prompt")
        self.post(t, "PreToolUse", tool_name="Bash")
        self.assertEqual(t.summary(True, 0)["cn"], 0)

    def test_one_ask_panel_per_wait(self):
        t = self.tracker()
        self.post(t, "PermissionRequest")
        self.post(t, "Notification", notification_type="permission_prompt")  # the same wait
        self.assertEqual(t.summary(True, 0)["ci"], 1)
        self.post(t, "PostToolUse")
        self.post(t, "PermissionRequest")
        self.assertEqual(t.summary(True, 0)["ci"], 2)

    def test_quiet_sessions_stop_counting(self):
        t = self.tracker()
        self.post(t, "Stop", now=0)
        self.assertEqual(t.summary(True, h.SESSION_QUIET_S)["cn"], 1)
        self.assertEqual(t.summary(True, h.SESSION_QUIET_S + 1)["cn"], 0)

    def test_unknown_until_hooks_exist(self):
        self.assertEqual(self.tracker().summary(False, 0)["ck"], 0)

    def test_keeps_nothing_private(self):
        t = self.tracker()
        self.post(
            t,
            "UserPromptSubmit",
            prompt="SECRET",
            tool_input={"command": "SECRET"},
            cwd="work/SECRET",
        )
        self.post(
            t, "PermissionRequest", tool_input={"command": "SECRET"}, transcript_path="SECRET"
        )
        self.assertNotIn("SECRET", repr(t.sessions) + t.ask_title)
        self.assertNotIn(
            "SECRET",
            h.encode(
                False,
                1,
                dict(h.PRINT_UNKNOWN),
                t.summary(True, 0),
                h.next_event_summary([], datetime.now(UTC)),
            ),
        )

    def test_title_falls_back_to_the_folder(self):
        t = h.ClaudeTracker(read_title=lambda path: None)
        self.post(t, "PermissionRequest", cwd="work/my-project")
        self.assertEqual(t.summary(True, 0)["ca"], "MY-PROJECT")


class Title(unittest.TestCase):
    def test_last_title_from_the_tail(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "t.jsonl"
            rows = [
                {"type": "custom-title", "customTitle": "Old"},
                {"type": "user", "message": "words"},
                {"type": "custom-title", "customTitle": "New"},
                {"type": "assistant", "message": "more"},
            ]
            path.write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
            self.assertEqual(h.title_from_transcript(path), "New")
            path.write_text(json.dumps({"type": "user"}), encoding="utf-8")
            self.assertIsNone(h.title_from_transcript(path))
        self.assertIsNone(h.title_from_transcript(None))
        self.assertIsNone(h.title_from_transcript("no such file"))


class Hooks(unittest.TestCase):
    def setUp(self):
        self.tracker = h.ClaudeTracker(read_title=lambda path: "T")
        self.server = h.start_hook_server(self.tracker, port=0)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def post(self, path, body, **headers):
        request = urllib.request.Request(self.url + path, body, headers, method="POST")
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status

    def test_accepts_a_hook_and_answers_at_once(self):
        body = json.dumps({"session_id": "s", "hook_event_name": "PermissionRequest"}).encode()
        self.assertEqual(self.post("/hook", body), 200)
        self.assertEqual(self.tracker.summary(True, 0)["cn"], 1)

    def test_ignores_web_pages_other_paths_and_rubbish(self):
        body = json.dumps({"session_id": "s", "hook_event_name": "PermissionRequest"}).encode()
        self.assertEqual(self.post("/hook", body, Origin="https://example.com"), 200)
        self.assertEqual(self.post("/other", body), 200)
        self.assertEqual(self.post("/hook", b"not json"), 200)
        self.assertEqual(self.post("/hook", b"[1, 2]"), 200)
        self.assertEqual(self.tracker.summary(True, 0)["cn"], 0)

    def test_settings_check(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "settings.json"
            self.assertFalse(h.hooks_installed(path))
            path.write_text('{"hooks": {"url": "http://127.0.0.1:8767/hook"}}', encoding="utf-8")
            self.assertTrue(h.hooks_installed(path))


class Calendar(unittest.TestCase):
    now = datetime(2030, 1, 15, 21, 7, tzinfo=UTC)

    def event(self, minutes, title="Dentist", **kw):
        fields = {"cancelled": False, "declined": False, "all_day": False, **kw}
        return SimpleNamespace(title=title, start=self.now + timedelta(minutes=minutes), **fields)

    def test_picks_the_next_timed_event(self):
        s = h.next_event_summary(
            [self.event(120, "Later"), self.event(83), self.event(-5, "Started")], self.now
        )
        self.assertEqual((s["na"], s["ns"], s["nn"]), ("22:30", 0, "DENTIST"))
        self.assertNotEqual(s["ni"], 0)

    def test_skips_what_isnt_an_appointment(self):
        events = [
            self.event(5, cancelled=True),
            self.event(5, declined=True),
            self.event(5, all_day=True),
            self.event(25 * 60),
        ]
        self.assertEqual(
            h.next_event_summary(events, self.now), {"na": "", "ns": 0, "ni": 0, "nn": ""}
        )

    def test_soon_is_ten_minutes(self):
        self.assertEqual(h.next_event_summary([self.event(10)], self.now)["ns"], 1)
        self.assertEqual(h.next_event_summary([self.event(11)], self.now)["ns"], 0)

    def test_link(self):
        self.assertIsNone(h.calendar_link(None))
        self.assertIsNone(h.calendar_link("  "))
        self.assertEqual(h.calendar_link("webcal://example.com/a.ics"), "https://example.com/a.ics")

    def test_no_link_ends_the_thread_at_once(self):
        source = h.CalendarSource(None, UTC)
        source.run()
        self.assertEqual(source.summary()["nn"], "")

    def test_old_answers_expire(self):
        source = h.CalendarSource(None, UTC)
        source.events = [
            SimpleNamespace(
                title="X",
                start=datetime.now(UTC) + timedelta(hours=1),
                cancelled=False,
                declined=False,
                all_day=False,
            )
        ]
        source.fetched_at = h.time.monotonic()
        self.assertEqual(source.summary()["nn"], "X")
        source.fetched_at = h.time.monotonic() - h.CALENDAR_KEEP_S - 1
        self.assertEqual(source.summary()["nn"], "")


class Message(unittest.TestCase):
    def test_carries_every_field_the_firmware_reads(self):
        message = json.loads(
            h.encode(
                True,
                71,
                {"pk": 1, "ps": "run", "pp": 62, "pm": 42, "pf": "TEST_PART", "pw": ""},
                {"ck": 1, "cn": 2, "ci": 7, "ca": "SOME PROJECT"},
                {"na": "22:30", "ns": 0, "ni": 123456, "nn": "DENTIST"},
            )
        )
        self.assertEqual(
            set(message),
            set("v game gpu pk ps pp pm pf pw ck cn ci ca na ns ni nn".split()),
        )
        self.assertEqual((message["v"], message["game"], message["gpu"]), (1, 1, 71))

    def test_longest_message_fits_the_display_buffer(self):
        text = "W" * h.TEXT_MAX
        message = h.encode(
            True,
            100,
            {"pk": 1, "ps": "stop", "pp": 100, "pm": 9999, "pf": text, "pw": text},
            {"ck": 1, "cn": 99, "ci": 999999, "ca": text},
            {"na": "22:30", "ns": 1, "ni": 0x7FFFFFFF, "nn": text},
        )
        self.assertLess(len(message), 512)

    def test_reaches_the_display_by_udp(self):
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(receiver.close)
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(3)
        sender = h.Sender("127.0.0.1", receiver.getsockname()[1])
        self.addCleanup(sender.socket.close)
        sender.send('{"v":1}', 0)
        self.assertEqual(receiver.recv(512), b'{"v":1}')

    def test_a_missing_display_is_not_an_error(self):
        sender = h.Sender("no-such-display.invalid", 4210)
        self.addCleanup(sender.socket.close)
        sender.send('{"v":1}', 0)
        self.assertEqual(sender.failing, "gaierror")


if __name__ == "__main__":
    unittest.main()
