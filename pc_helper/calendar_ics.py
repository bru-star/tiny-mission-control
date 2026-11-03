"""Bounded iCalendar (RFC 5545) reader for the next-event line.

Only what that line needs: VEVENTs with their start, end, status, title, the
user's own attendance, and recurrences expanded inside a bounded window. No
dependency: a calendar feed is a small, line-based format.

Recurrence support is deliberately a closed subset (FREQ DAILY/WEEKLY/MONTHLY/
YEARLY with INTERVAL, COUNT, UNTIL, BYDAY, BYMONTHDAY, BYMONTH, WKST, plus
EXDATE and RECURRENCE-ID overrides). A rule outside it is never guessed at: the
series is skipped and counted in `unsupported`, so the caller can report that
the calendar was read incompletely instead of silently showing the wrong event.
"""

from dataclasses import dataclass, field
from datetime import date, datetime, time, timedelta, timezone
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


@dataclass
class IcsEvent:
    uid: str
    title: str
    start: datetime          # aware
    end: datetime            # aware
    all_day: bool
    cancelled: bool
    declined: bool


@dataclass
class IcsResult:
    events: list = field(default_factory=list)
    unsupported: int = 0     # recurring series skipped (rule outside the subset)
    malformed: int = 0       # components that could not be read whole


WEEKDAYS = {"MO": 0, "TU": 1, "WE": 2, "TH": 3, "FR": 4, "SA": 5, "SU": 6}
SUPPORTED_RULE_PARTS = {"FREQ", "INTERVAL", "COUNT", "UNTIL", "BYDAY", "BYMONTHDAY", "BYMONTH", "WKST"}
MAX_ITERATIONS = 20000


class UnsupportedCalendar(ValueError):
    """A construct or bound prevents proving the next-event answer."""


def unfold(text):
    lines = []
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if raw[:1] in (" ", "\t") and lines:
            lines[-1] += raw[1:]
        elif raw:
            lines.append(raw)
    return lines


def split_property(line):
    """'DTSTART;TZID=America/New_York:20300115T183000' -> (name, params, value)."""
    colon = -1
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif c == ":" and not quoted:
            colon = i
            break
    if colon < 0:
        return None
    head, value = line[:colon], line[colon + 1:]
    parts = head.split(";")
    params = {}
    for part in parts[1:]:
        if "=" in part:
            key, _, val = part.partition("=")
            params[key.upper()] = val.strip('"')
    return parts[0].upper(), params, value


def unescape(value):
    out, i = [], 0
    while i < len(value):
        c = value[i]
        if c == "\\" and i + 1 < len(value):
            n = value[i + 1]
            out.append(" " if n in "nN" else n)
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def zone_for(params, default_tz):
    tzid = params.get("TZID")
    if not tzid:
        return default_tz
    try:
        return ZoneInfo(tzid)
    except (ZoneInfoNotFoundError, ValueError):
        return None


def parse_datetime(value, params, default_tz):
    """-> (aware datetime, all_day) or None. All-day starts are local midnight."""
    value = value.strip()
    if params.get("VALUE", "").upper() == "DATE" or (len(value) == 8 and value.isdigit()):
        day = datetime.strptime(value[:8], "%Y%m%d").date()
        return datetime.combine(day, time(0, 0), default_tz), True
    try:
        naive = datetime.strptime(value.rstrip("Z"), "%Y%m%dT%H%M%S")
    except ValueError:
        return None
    if value.endswith("Z"):
        return naive.replace(tzinfo=timezone.utc), False
    tz = zone_for(params, default_tz)
    if tz is None:
        return None
    aware = naive.replace(tzinfo=tz)
    if aware.astimezone(timezone.utc).astimezone(tz).replace(tzinfo=None) != naive:
        return None  # nonexistent local time; never silently shift an appointment
    return aware, False


def parse_duration(value):
    sign = -1 if value.startswith("-") else 1
    value = value.lstrip("+-")
    if not value.startswith("P"):
        return None
    days = seconds = 0
    number = ""
    in_time = False
    for c in value[1:]:
        if c == "T":
            in_time = True
        elif c.isdigit():
            number += c
        elif c in "WDHMS" and number:
            n = int(number)
            number = ""
            if c == "W":
                days += 7 * n
            elif c == "D":
                days += n
            elif c == "H" and in_time:
                seconds += 3600 * n
            elif c == "M" and in_time:
                seconds += 60 * n
            elif c == "S" and in_time:
                seconds += n
            else:
                return None
        else:
            return None
    return None if number else sign * timedelta(days=days, seconds=seconds)


def parse_rule(value):
    rule = {}
    for part in value.split(";"):
        if "=" not in part:
            return None
        key, _, val = part.partition("=")
        if key.upper() in rule or not val:
            return None
        rule[key.upper()] = val.upper()
    if not set(rule) <= SUPPORTED_RULE_PARTS or rule.get("FREQ") not in ("DAILY", "WEEKLY", "MONTHLY", "YEARLY"):
        return None
    try:
        rule["INTERVAL"] = int(rule.get("INTERVAL", "1"))
        if "COUNT" in rule:
            rule["COUNT"] = int(rule["COUNT"])
        if rule["INTERVAL"] < 1 or rule.get("COUNT", 1) < 1 or ("COUNT" in rule and "UNTIL" in rule):
            return None
        byday = []
        for token in filter(None, rule.get("BYDAY", "").split(",")):
            ordinal, name = token[:-2], token[-2:]
            if name not in WEEKDAYS:
                return None
            byday.append((int(ordinal) if ordinal not in ("", "+") else 0, WEEKDAYS[name]))
        rule["BYDAY"] = byday
        rule["BYMONTHDAY"] = [int(v) for v in filter(None, rule.get("BYMONTHDAY", "").split(","))]
        rule["BYMONTH"] = [int(v) for v in filter(None, rule.get("BYMONTH", "").split(","))]
    except ValueError:
        return None
    if any(m < 1 or m > 12 for m in rule["BYMONTH"]) or any(d == 0 or abs(d) > 31 for d in rule["BYMONTHDAY"]):
        return None
    if rule.get("WKST", "MO") not in WEEKDAYS:
        return None
    # Closed subset: reject combinations whose RFC expansion/limiting rules
    # are not implemented below. A refused series makes the feed incomplete.
    if rule["FREQ"] != "MONTHLY" and any(o for o, _ in rule["BYDAY"]):
        return None
    if any(abs(o) > 5 for o, _ in rule["BYDAY"]):
        return None
    if rule["FREQ"] in ("DAILY", "WEEKLY") and rule["BYMONTHDAY"]:
        return None
    if rule["BYMONTHDAY"] and rule["BYDAY"]:
        return None
    if rule["FREQ"] == "YEARLY" and rule["BYDAY"]:
        return None
    rule["BYMONTH"] = sorted(set(rule["BYMONTH"]))
    return rule


def month_days(year, month):
    if month == 12:
        return (date(year + 1, 1, 1) - date(year, 12, 1)).days
    return (date(year, month + 1, 1) - date(year, month, 1)).days


def monthly_candidates(year, month, rule, dtstart):
    """Days of one month that the rule selects, in order."""
    days = set()
    n = month_days(year, month)
    for d in rule["BYMONTHDAY"]:
        day = d if d > 0 else n + d + 1
        if 1 <= day <= n:
            days.add(day)
    for ordinal, weekday in rule["BYDAY"]:
        matches = [d for d in range(1, n + 1) if date(year, month, d).weekday() == weekday]
        if ordinal == 0:
            days.update(matches)
        elif -len(matches) <= ordinal <= len(matches) and ordinal != 0:
            days.add(matches[ordinal - 1] if ordinal > 0 else matches[ordinal])
    if not rule["BYMONTHDAY"] and not rule["BYDAY"]:
        if dtstart.day <= n:
            days.add(dtstart.day)
    return sorted(days)


def occurrences(dtstart_local, rule, window_end_local):
    """Naive local start times of a series, from DTSTART, in order."""
    freq, interval = rule["FREQ"], rule["INTERVAL"]
    count = rule.get("COUNT")
    produced = 0
    wall = dtstart_local.time()
    start_day = dtstart_local.date()

    def emit(day):
        return datetime.combine(day, wall)

    iterations = 0
    if freq == "DAILY":
        day = start_day
        while True:
            iterations += 1
            if iterations > MAX_ITERATIONS:
                raise UnsupportedCalendar("recurrence iteration limit")
            if emit(day) > window_end_local:
                return
            if (not rule["BYMONTH"] or day.month in rule["BYMONTH"]) and \
                    (not rule["BYDAY"] or day.weekday() in [w for _, w in rule["BYDAY"]]):
                candidate = emit(day)
                if candidate > window_end_local:
                    return
                yield candidate
                produced += 1
                if count is not None and produced >= count:
                    return
            day += timedelta(days=interval)
    elif freq == "WEEKLY":
        wkst = WEEKDAYS.get(rule.get("WKST", "MO"), 0)
        weekdays = sorted({w for _, w in rule["BYDAY"]} or {start_day.weekday()}, key=lambda w: (w - wkst) % 7)
        week = start_day - timedelta(days=(start_day.weekday() - wkst) % 7)
        while True:
            iterations += 1
            if iterations > MAX_ITERATIONS:
                raise UnsupportedCalendar("recurrence iteration limit")
            if emit(week) > window_end_local:
                return
            for weekday in weekdays:
                day = week + timedelta(days=(weekday - wkst) % 7)
                if day < start_day:
                    continue
                if rule["BYMONTH"] and day.month not in rule["BYMONTH"]:
                    continue
                candidate = emit(day)
                if candidate > window_end_local:
                    return
                yield candidate
                produced += 1
                if count is not None and produced >= count:
                    return
            week += timedelta(weeks=interval)
    else:
        year, month = start_day.year, start_day.month
        while True:
            iterations += 1
            if iterations > MAX_ITERATIONS:
                raise UnsupportedCalendar("recurrence iteration limit")
            if date(year, month if freq == "MONTHLY" else 1, 1) > window_end_local.date():
                return
            if freq == "YEARLY":
                months = rule["BYMONTH"] or [start_day.month]
            else:
                months = [month] if not rule["BYMONTH"] or month in rule["BYMONTH"] else []
            for m in months:
                if freq == "YEARLY" and not rule["BYDAY"] and not rule["BYMONTHDAY"]:
                    days = [start_day.day] if start_day.day <= month_days(year, m) else []
                else:
                    days = monthly_candidates(year, m, rule, start_day)
                for d in days:
                    day = date(year, m, d)
                    if day < start_day:
                        continue
                    candidate = emit(day)
                    if candidate > window_end_local:
                        return
                    yield candidate
                    produced += 1
                    if count is not None and produced >= count:
                        return
            if freq == "YEARLY":
                year += interval
            else:
                month += interval
                year += (month - 1) // 12
                month = (month - 1) % 12 + 1


def parse(text, window_start, window_end, default_tz, self_emails=()):
    """All occurrences overlapping [window_start, window_end) as IcsEvents."""
    result = IcsResult()
    lines = unfold(text)
    if not lines or lines[0].upper() != "BEGIN:VCALENDAR" or lines[-1].upper() != "END:VCALENDAR":
        result.malformed += 1
    self_emails = {e.lower() for e in self_emails}
    components, current, depth = [], None, 0
    for line in lines:
        upper = line.upper()
        if upper == "BEGIN:VEVENT":
            if current is not None:
                result.malformed += 1
            current, depth = [], 1
        elif upper.startswith("BEGIN:") and current is not None:
            depth += 1  # nested VALARM etc.
        elif upper == "END:VEVENT" and current is not None and depth == 1:
            components.append(current)
            current = None
        elif upper.startswith("END:") and current is not None:
            depth -= 1
        elif current is not None and depth == 1:
            current.append(line)
    if current is not None:
        result.malformed += 1

    masters, overrides = [], {}
    for lines in components:
        props = {}
        for line in lines:
            parsed = split_property(line)
            if parsed:
                props.setdefault(parsed[0], []).append(parsed)
        try:
            item = read_component(props, default_tz, self_emails)
        except UnsupportedCalendar:
            result.unsupported += 1
            continue
        except (ValueError, KeyError):
            item = None
        if item is None:
            result.malformed += 1
            continue
        if item["recurrence_id"] is not None:
            overrides[(item["uid"], item["recurrence_id"])] = item
        else:
            masters.append(item)

    for item in masters:
        duration = item["end"] - item["start"]
        if item["rule"] is None and item["rrule_text"] is not None:
            result.unsupported += 1
            continue
        if item["rule"] is None:
            starts = [item["start"]]
        else:
            tz = item["start"].tzinfo
            local_start = item["start"].replace(tzinfo=None)
            until = item["rule"].get("UNTIL")
            until_dt = None
            if until:
                parsed_until = parse_datetime(until, {}, tz)
                if parsed_until is None:
                    result.unsupported += 1
                    continue
                until_dt = parsed_until[0]
            local_end = window_end.astimezone(tz).replace(tzinfo=None)
            starts = []
            try:
                for naive in occurrences(local_start, item["rule"], local_end):
                    aware = naive.replace(tzinfo=tz)
                    if until_dt is not None and aware > until_dt:
                        break
                    if aware.astimezone(timezone.utc).astimezone(tz).replace(tzinfo=None) != naive:
                        raise UnsupportedCalendar("recurrence crosses nonexistent local time")
                    starts.append(aware)
            except (UnsupportedCalendar, ValueError, OverflowError):
                result.unsupported += 1
                continue
        for start in starts:
            if start in item["exdates"]:
                continue
            override = overrides.pop((item["uid"], start), None)
            if override is not None:
                event = make_event(override, override["start"], override["end"] - override["start"])
            else:
                event = make_event(item, start, duration)
            if event.end > window_start and event.start < window_end:
                result.events.append(event)
    # Overrides whose master was skipped or lies outside the window.
    for override in overrides.values():
        event = make_event(override, override["start"], override["end"] - override["start"])
        if event.end > window_start and event.start < window_end:
            result.events.append(event)
    return result


def make_event(item, start, duration):
    return IcsEvent(uid=item["uid"], title=item["title"], start=start, end=start + duration,
                    all_day=item["all_day"], cancelled=item["cancelled"], declined=item["declined"])


def read_component(props, default_tz, self_emails):
    def first(name):
        values = props.get(name)
        return values[0] if values else None

    if "RDATE" in props or "EXRULE" in props or len(props.get("RRULE", [])) > 1:
        raise UnsupportedCalendar("unsupported recurrence property")
    if first("RECURRENCE-ID") and first("RECURRENCE-ID")[1].get("RANGE"):
        raise UnsupportedCalendar("recurrence range override")

    dtstart = first("DTSTART")
    if dtstart is None:
        return None
    parsed = parse_datetime(dtstart[2], dtstart[1], default_tz)
    if parsed is None:
        return None
    start, all_day = parsed
    end = None
    dtend = first("DTEND")
    if dtend is not None:
        parsed_end = parse_datetime(dtend[2], dtend[1], default_tz)
        if parsed_end is None:
            return None
        end = parsed_end[0]
    elif first("DURATION") is not None:
        delta = parse_duration(first("DURATION")[2])
        if delta is None:
            return None
        end = start + delta
    else:
        end = start + (timedelta(days=1) if all_day else timedelta(0))
    if end < start:
        return None
    uid = first("UID")[2] if first("UID") else ""
    summary = unescape(first("SUMMARY")[2]) if first("SUMMARY") else ""
    status = first("STATUS")[2].upper() if first("STATUS") else ""
    declined = False
    for _, params, value in props.get("ATTENDEE", []):
        email = value.lower().replace("mailto:", "")
        if email in self_emails and params.get("PARTSTAT", "").upper() == "DECLINED":
            declined = True
    exdates = set()
    for _, params, value in props.get("EXDATE", []):
        for token in value.split(","):
            parsed_ex = parse_datetime(token, params, start.tzinfo)
            if parsed_ex is None:
                raise ValueError("unreadable EXDATE")
            exdates.add(parsed_ex[0])
    recurrence_id = None
    rid = first("RECURRENCE-ID")
    if rid is not None:
        parsed_rid = parse_datetime(rid[2], rid[1], start.tzinfo)
        if parsed_rid is None:
            return None
        recurrence_id = parsed_rid[0]
    rrule = first("RRULE")
    return {
        "uid": uid, "title": summary, "start": start, "end": end, "all_day": all_day,
        "cancelled": status == "CANCELLED", "declined": declined, "exdates": exdates,
        "recurrence_id": recurrence_id,
        "rrule_text": rrule[2] if rrule else None,
        "rule": parse_rule(rrule[2]) if rrule else None,
    }
