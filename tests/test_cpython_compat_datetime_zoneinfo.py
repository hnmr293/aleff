"""CPython observable compatibility for datetime and zoneinfo accelerators."""

from textwrap import dedent

import pytest

from cpython_compat_support import assert_cpython_compatible


_DATETIME_CONVERSION_TARGETS = (
    "datetime.datetime.now",
    "datetime.datetime.fromtimestamp",
    "datetime.datetime.astimezone",
    "datetime.datetime.timestamp",
)
_TIME_TARGETS = (
    "datetime.time.utcoffset",
    "datetime.time.dst",
    "datetime.time.tzname",
    "datetime.time.isoformat",
    "datetime.time aware comparison <",
    "datetime.time aware comparison <=",
    "datetime.time aware comparison ==",
    "datetime.time aware comparison !=",
    "datetime.time aware comparison >",
    "datetime.time aware comparison >=",
)
_DATETIME_TARGETS = (
    "datetime.datetime.utcoffset",
    "datetime.datetime.dst",
    "datetime.datetime.tzname",
    "datetime.datetime.strftime",
    "datetime.datetime.isoformat",
    "datetime.datetime aware comparison <",
    "datetime.datetime aware comparison <=",
    "datetime.datetime aware comparison ==",
    "datetime.datetime aware comparison !=",
    "datetime.datetime aware comparison >",
    "datetime.datetime aware comparison >=",
    "datetime.datetime aware subtraction",
)
_ZONEINFO_TARGETS = ("zoneinfo.ZoneInfo.from_file",)


@pytest.mark.parametrize("target", _DATETIME_CONVERSION_TARGETS, ids=_DATETIME_CONVERSION_TARGETS)
def test_datetime_conversion_targets_match_cpython(target: str) -> None:
    assert_cpython_compatible(
        dedent(
            f"""
            from datetime import datetime, timedelta, timezone, tzinfo

            target = {target!r}
            events = []


            class Tz(tzinfo):
                def utcoffset(self, value):
                    events.append(("utcoffset", value is not None))
                    return timedelta(hours=2)

                def dst(self, value):
                    events.append(("dst", value is not None))
                    return timedelta(0)

                def tzname(self, value):
                    events.append(("tzname", value is not None))
                    return "ISSUE56"

                def fromutc(self, value):
                    events.append(("fromutc", value.tzinfo is self))
                    return datetime(2000, 1, 2, 12, 0, 0, 123456, tzinfo=self)


            def key(value):
                return (value.year, value.month, value.day, value.hour, value.minute, value.second, value.microsecond, value.fold)


            tz = Tz()
            if target == "datetime.datetime.now":
                result = key(datetime.now(tz))
            elif target == "datetime.datetime.fromtimestamp":
                result = key(datetime.fromtimestamp(946684800.125, tz))
            elif target == "datetime.datetime.astimezone":
                value = datetime(2000, 1, 2, 3, 4, 5, 678901, tzinfo=timezone.utc)
                result = key(value.astimezone(tz))
            elif target == "datetime.datetime.timestamp":
                value = datetime(2000, 1, 2, 3, 4, 5, 678901, tzinfo=tz)
                result = value.timestamp()
            else:
                raise AssertionError(target)
            print(target, result, events)
            """
        ).strip()
    )


@pytest.mark.parametrize("target", _TIME_TARGETS, ids=_TIME_TARGETS)
def test_time_targets_match_cpython(target: str) -> None:
    assert_cpython_compatible(
        dedent(
            f"""
            from datetime import timedelta, time, tzinfo

            target = {target!r}
            events = []


            class Tz(tzinfo):
                def __init__(self, label, hours):
                    self.label = label
                    self.hours = hours

                def utcoffset(self, value):
                    events.append((self.label, "utcoffset", value is not None))
                    return timedelta(hours=self.hours)

                def dst(self, value):
                    events.append((self.label, "dst", value is not None))
                    return timedelta(0)

                def tzname(self, value):
                    events.append((self.label, "tzname", value is not None))
                    return self.label


            if target.startswith("datetime.time aware comparison"):
                left = time(3, 0, tzinfo=Tz("left", 3))
                right = time(4, 0, tzinfo=Tz("right", 2))
                operation = target.rsplit(" ", 1)[1]
                result = {{
                    "<": lambda: left < right,
                    "<=": lambda: left <= right,
                    "==": lambda: left == right,
                    "!=": lambda: left != right,
                    ">": lambda: left > right,
                    ">=": lambda: left >= right,
                }}[operation]()
            else:
                value = time(3, 4, 5, 678901, tzinfo=Tz("time", 2))
                if target == "datetime.time.utcoffset":
                    result = value.utcoffset()
                elif target == "datetime.time.dst":
                    result = value.dst()
                elif target == "datetime.time.tzname":
                    result = value.tzname()
                elif target == "datetime.time.isoformat":
                    result = value.isoformat(timespec="microseconds")
                else:
                    raise AssertionError(target)
            print(target, result, events)
            """
        ).strip()
    )


@pytest.mark.parametrize("target", _DATETIME_TARGETS, ids=_DATETIME_TARGETS)
def test_datetime_targets_match_cpython(target: str) -> None:
    assert_cpython_compatible(
        dedent(
            f"""
            from datetime import datetime, timedelta, tzinfo

            target = {target!r}
            events = []


            class Tz(tzinfo):
                def __init__(self, label, hours):
                    self.label = label
                    self.hours = hours

                def utcoffset(self, value):
                    events.append((self.label, "utcoffset", value is not None))
                    return timedelta(hours=self.hours)

                def dst(self, value):
                    events.append((self.label, "dst", value is not None))
                    return timedelta(0)

                def tzname(self, value):
                    events.append((self.label, "tzname", value is not None))
                    return self.label

                def fromutc(self, value):
                    events.append((self.label, "fromutc", value.tzinfo is self))
                    return value


            value = datetime(2000, 1, 2, 3, 4, 5, 678901, tzinfo=Tz("value", 2))
            if target == "datetime.datetime.utcoffset":
                result = value.utcoffset()
            elif target == "datetime.datetime.dst":
                result = value.dst()
            elif target == "datetime.datetime.tzname":
                result = value.tzname()
            elif target == "datetime.datetime.strftime":
                result = value.strftime("%Y-%m-%d %H:%M:%S.%f %z %Z")
            elif target == "datetime.datetime.isoformat":
                result = value.isoformat(timespec="microseconds")
            elif target.startswith("datetime.datetime aware comparison"):
                left = datetime(2000, 1, 2, 3, 0, tzinfo=Tz("left", 3))
                right = datetime(2000, 1, 2, 3, 0, tzinfo=Tz("right", 2))
                operation = target.rsplit(" ", 1)[1]
                result = {{
                    "<": lambda: left < right,
                    "<=": lambda: left <= right,
                    "==": lambda: left == right,
                    "!=": lambda: left != right,
                    ">": lambda: left > right,
                    ">=": lambda: left >= right,
                }}[operation]()
            elif target == "datetime.datetime aware subtraction":
                left = datetime(2000, 1, 2, 3, 0, tzinfo=Tz("left", 3))
                right = datetime(2000, 1, 2, 3, 0, tzinfo=Tz("right", 2))
                result = left - right
            else:
                raise AssertionError(target)
            print(target, result, events)
            """
        ).strip()
    )


def test_datetime_and_zoneinfo_error_and_corner_behavior_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
from datetime import datetime, time, timedelta, tzinfo
from zoneinfo import ZoneInfo


class BadTz(tzinfo):
    def utcoffset(self, _value):
        return timedelta(days=1)

    def dst(self, _value):
        return timedelta(days=1)

    def tzname(self, _value):
        return 1

    def fromutc(self, _value):
        return datetime(2000, 1, 2)


def report(label, operation):
    try:
        result = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(result).__name__)


bad = BadTz()
report("now_bad_fromutc", lambda: datetime.now(bad))
report("fromtimestamp_bad_fromutc", lambda: datetime.fromtimestamp(0, bad))
report("astimezone_bad_fromutc", lambda: datetime(2000, 1, 2, tzinfo=bad).astimezone(bad))
report("timestamp_bad_offset", lambda: datetime(2000, 1, 2, tzinfo=bad).timestamp())
report("time_bad_offset", lambda: time(3, tzinfo=bad).utcoffset())
report("time_bad_dst", lambda: time(3, tzinfo=bad).dst())
report("time_bad_name", lambda: time(3, tzinfo=bad).tzname())
report("datetime_bad_offset", lambda: datetime(2000, 1, 2, tzinfo=bad).utcoffset())
report("datetime_bad_dst", lambda: datetime(2000, 1, 2, tzinfo=bad).dst())
report("datetime_bad_name", lambda: datetime(2000, 1, 2, tzinfo=bad).tzname())
report("datetime_bad_strftime", lambda: datetime(2000, 1, 2, tzinfo=bad).strftime("%z %Z"))
report("datetime_bad_isoformat", lambda: datetime(2000, 1, 2, tzinfo=bad).isoformat())
report("now_bad_type", lambda: datetime.now(object()))
report("fromtimestamp_bad_type", lambda: datetime.fromtimestamp(0, object()))
report("time_bad_type", lambda: time(3, tzinfo=object()).isoformat())


class EmptyFile:
    def read(self, _size=-1):
        return b""


report("zoneinfo_empty_file", lambda: ZoneInfo.from_file(EmptyFile(), key="issue56"))
""".strip()
    )


def test_datetime_comparison_repeated_offsets_and_fold_probes_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
from datetime import datetime, timedelta, tzinfo


class StatefulTz(tzinfo):
    def __init__(self, label, offsets):
        self.label = label
        self.offsets = iter(offsets)

    def utcoffset(self, value):
        offset = next(self.offsets)
        print(self.label, "utcoffset", value.fold, offset)
        return offset


left = datetime(2000, 1, 2, 3, tzinfo=StatefulTz("left", (timedelta(hours=3), timedelta(hours=1))))
right = datetime(2000, 1, 2, 3, tzinfo=StatefulTz("right", (timedelta(hours=2), timedelta(hours=4))))
print("stateful", left < right)


class FoldTz(tzinfo):
    def __init__(self, label, folded_hours):
        self.label = label
        self.folded_hours = folded_hours

    def utcoffset(self, value):
        print(self.label, "fold", value.fold)
        return timedelta(hours=self.folded_hours if value.fold else 0)


left = datetime(2000, 1, 2, 3, tzinfo=FoldTz("left-fold", 1))
right = datetime(2000, 1, 2, 3, tzinfo=FoldTz("right-fixed", 0))
print("fold-left", left == right, left != right)

left = datetime(2000, 1, 2, 3, tzinfo=FoldTz("left-fixed", 0))
right = datetime(2000, 1, 2, 3, tzinfo=FoldTz("right-fold", 1))
print("fold-right", left == right, left != right)
""".strip()
    )


@pytest.mark.parametrize("target", _ZONEINFO_TARGETS, ids=_ZONEINFO_TARGETS)
def test_zoneinfo_from_file_target_match_cpython(target: str) -> None:
    assert target == "zoneinfo.ZoneInfo.from_file"
    assert_cpython_compatible(
        r"""
from datetime import timedelta
import struct
from zoneinfo import ZoneInfo


payloads = {
    4: b"TZif",
    1: b"\0",
    15: b"\0" * 15,
    24: struct.pack(">6l", 0, 0, 0, 0, 1, 4),
    6: struct.pack(">lbb", 0, 0, 0),
}


class ReadFile:
    def __init__(self):
        self.calls = []

    def read(self, size=-1):
        self.calls.append(size)
        if size == 4 and len(self.calls) == 1:
            return payloads[size]
        if size == 4:
            return b"UTC\0"
        return payloads[size]


file = ReadFile()
zone = ZoneInfo.from_file(file, key="issue56-utc")
print(zone.key, zone.utcoffset(None), file.calls)
""".strip()
    )
