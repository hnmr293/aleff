"""Strict continuation coverage for datetime and zoneinfo C accelerators."""

from __future__ import annotations

from collections.abc import Callable
from datetime import datetime, timedelta, time, timezone, tzinfo
from pathlib import Path
import struct
import subprocess
import sys
from typing import Any, Literal, cast
from zoneinfo import ZoneInfo

import pytest

from aleff import create_handler, effect


Case = Callable[[], None]
Choose = Callable[[], Any]
Outcome = tuple[str, Any]
CaseKind = Literal["normal", "error", "corner", "multishot"]

_CASES: dict[str, tuple[CaseKind, Case]] = {}
_SCENARIOS: tuple[CaseKind, ...] = ("normal", "error", "corner", "multishot")

_DATETIME_CONVERSION_TARGETS = (
    "datetime.datetime.now",
    "datetime.datetime.fromtimestamp",
    "datetime.datetime.astimezone",
    "datetime.datetime.astimezone source utcoffset",
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
    "datetime.time aware comparison right <",
    "datetime.time aware comparison right <=",
    "datetime.time aware comparison right ==",
    "datetime.time aware comparison right !=",
    "datetime.time aware comparison right >",
    "datetime.time aware comparison right >=",
)
_DATETIME_TARGETS = (
    "datetime.datetime.utcoffset",
    "datetime.datetime.dst",
    "datetime.datetime.tzname",
    "datetime.datetime.strftime (tzinfo.dst)",
    "datetime.datetime.strftime (tzinfo.utcoffset)",
    "datetime.datetime.strftime (tzinfo.tzname)",
    "datetime.datetime.isoformat",
    "datetime.datetime aware comparison <",
    "datetime.datetime aware comparison <=",
    "datetime.datetime aware comparison ==",
    "datetime.datetime aware comparison !=",
    "datetime.datetime aware comparison >",
    "datetime.datetime aware comparison >=",
    "datetime.datetime aware comparison right <",
    "datetime.datetime aware comparison right <=",
    "datetime.datetime aware comparison right ==",
    "datetime.datetime aware comparison right !=",
    "datetime.datetime aware comparison right >",
    "datetime.datetime aware comparison right >=",
    "datetime.datetime aware comparison second <",
    "datetime.datetime aware comparison second <=",
    "datetime.datetime aware comparison second ==",
    "datetime.datetime aware comparison second !=",
    "datetime.datetime aware comparison second >",
    "datetime.datetime aware comparison second >=",
    "datetime.datetime fold-left comparison ==",
    "datetime.datetime fold-left comparison !=",
    "datetime.datetime fold-right comparison ==",
    "datetime.datetime fold-right comparison !=",
    "datetime.datetime aware subtraction",
    "datetime.datetime aware subtraction right",
)
_ZONEINFO_TARGETS = ("zoneinfo.ZoneInfo.from_file",)


class ExpectedCallbackError(Exception):
    """An exception used to verify callback-exception restoration."""


def _outcome(call: Callable[[], Any]) -> Outcome:
    try:
        return "return", call()
    except BaseException as exc:
        return "raise", (type(exc).__name__, str(exc))


def _datetime_key(value: datetime) -> tuple[Any, ...]:
    return (
        value.year,
        value.month,
        value.day,
        value.hour,
        value.minute,
        value.second,
        value.microsecond,
        value.fold,
        value.tzinfo.__class__.__name__ if value.tzinfo is not None else None,
    )


def _scenario_decisions(scenario: CaseKind) -> tuple[Any, ...]:
    if scenario == "normal":
        return ("first", "second", "first")
    if scenario == "error":
        return ("raise", "first", "raise")
    if scenario == "corner":
        return ("invalid", "first", "invalid")
    return ("second", "first", "second")


def _assert_scenario(outcomes: list[Outcome], scenario: CaseKind) -> None:
    assert outcomes[0] == outcomes[2]
    if scenario in ("normal", "multishot"):
        assert all(outcome[0] == "return" for outcome in outcomes)
    elif scenario == "error":
        assert outcomes[0][0] == "raise"
        assert outcomes[1][0] == "return"


def _resume_against_fresh(
    run: Callable[[Choose, Any], Any],
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
    effect_name: str,
) -> list[Outcome]:
    """Compare every resume with a fresh ordinary execution of the operation."""

    choose = effect(effect_name)
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        outcomes: list[Outcome] = []
        for decision in decisions:
            actual = _outcome(lambda decision=decision: k(decision))
            expected = _outcome(lambda decision=decision: fresh(decision))
            assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            outcomes.append(actual)
        return outcomes

    result = handler(lambda: run(choose, decisions[0]))
    assert suspension_count == 1, "the scenario must suspend at exactly one tz callback"
    return cast(list[Outcome], result)


class _CallbackTZ(tzinfo):
    def __init__(
        self,
        choose: Choose,
        trigger: str,
        *,
        comparison: bool = False,
        trigger_occurrence: int = 1,
    ) -> None:
        self._choose = choose
        self._trigger = trigger
        self._comparison = comparison
        self._trigger_occurrence = trigger_occurrence
        self._occurrences = 0

    def _decision(self, method: str) -> Any:
        if method != self._trigger:
            if method == "utcoffset":
                return timedelta(hours=2)
            if method == "dst":
                return timedelta(0)
            if method == "tzname":
                return "FIXED"
            raise AssertionError(method)

        self._occurrences += 1
        if self._occurrences != self._trigger_occurrence:
            if method == "utcoffset":
                return timedelta(hours=-1 if self._comparison else 2)
            if method == "dst":
                return timedelta(0)
            if method == "tzname":
                return "FIXED"
            raise AssertionError(method)

        decision = self._choose()
        if decision == "raise":
            raise ExpectedCallbackError(f"{method} callback failed")
        if decision == "invalid":
            if method == "fromutc":
                return datetime(2000, 1, 2)
            if method in ("utcoffset", "dst"):
                return timedelta(days=1)
            return 1
        if method == "fromutc":
            hour = 20 if decision == "second" else 10
            return datetime(2000, 1, 2, hour, tzinfo=self)
        if method == "utcoffset":
            hours = (-1 if decision == "first" else 3) if self._comparison else (1 if decision == "first" else 3)
            return timedelta(hours=hours)
        if method == "dst":
            return timedelta(hours=1 if decision == "first" else 0)
        if method == "tzname":
            return "FIRST" if decision == "first" else "SECOND"
        raise AssertionError(method)

    def utcoffset(self, value: datetime | None) -> timedelta | None:
        if self._trigger == "utcoffset":
            assert value is None or (isinstance(value, datetime) and value.tzinfo is self)
        return cast(timedelta | None, self._decision("utcoffset"))

    def dst(self, value: datetime | None) -> timedelta | None:
        if self._trigger == "dst":
            assert value is None or (isinstance(value, datetime) and value.tzinfo is self)
        return cast(timedelta | None, self._decision("dst"))

    def tzname(self, value: datetime | None) -> str | None:
        if self._trigger == "tzname":
            assert value is None or (isinstance(value, datetime) and value.tzinfo is self)
        return cast(str | None, self._decision("tzname"))

    def fromutc(self, value: datetime) -> datetime:
        assert isinstance(value, datetime) and value.tzinfo is self
        return cast(datetime, self._decision("fromutc"))


class _FixedTZ(tzinfo):
    def __init__(self, hours: int) -> None:
        self.hours = hours

    def utcoffset(self, _value: datetime | None) -> timedelta:
        return timedelta(hours=self.hours)

    def dst(self, _value: datetime | None) -> timedelta:
        return timedelta(0)

    def tzname(self, _value: datetime | None) -> str:
        return f"UTC{self.hours:+d}"


class _FoldTZ(tzinfo):
    def __init__(self, choose: Choose | None) -> None:
        self._choose = choose

    def utcoffset(self, value: datetime | None) -> timedelta:
        assert isinstance(value, datetime) and value.tzinfo is self
        if value.fold == 0 or self._choose is None:
            return timedelta(0)
        decision = self._choose()
        if decision == "raise":
            raise ExpectedCallbackError("fold utcoffset callback failed")
        if decision == "invalid":
            return timedelta(days=1)
        return timedelta(hours=0 if decision == "first" else 1)

    def dst(self, _value: datetime | None) -> timedelta:
        return timedelta(0)


def _run_datetime_conversion(target: str, choose: Choose) -> Any:
    if target == "datetime.datetime.astimezone source utcoffset":
        source_tz = _CallbackTZ(choose, "utcoffset")
        destination_tz = _FixedTZ(0)
        value = datetime(2000, 1, 2, 3, 4, 5, 678901, tzinfo=source_tz)
        return _datetime_key(value.astimezone(destination_tz))

    trigger = "fromutc" if target != "datetime.datetime.timestamp" else "utcoffset"
    tz = _CallbackTZ(choose, trigger)
    if target == "datetime.datetime.now":
        return _datetime_key(datetime.now(tz))
    if target == "datetime.datetime.fromtimestamp":
        return _datetime_key(datetime.fromtimestamp(946684800.125, tz))
    if target == "datetime.datetime.astimezone":
        value = datetime(2000, 1, 2, 3, 4, 5, 678901, tzinfo=timezone.utc)
        return _datetime_key(value.astimezone(tz))
    if target == "datetime.datetime.timestamp":
        value = datetime(2000, 1, 2, 3, 4, 5, 678901, tzinfo=tz)
        return value.timestamp()
    raise AssertionError(target)


def _run_time_target(target: str, choose: Choose) -> Any:
    if target.startswith("datetime.time aware comparison "):
        right_callback = " comparison right " in target
        left_tz = _FixedTZ(3) if right_callback else _CallbackTZ(choose, "utcoffset", comparison=True)
        right_tz = _CallbackTZ(choose, "utcoffset", comparison=True) if right_callback else _FixedTZ(2)
        left = time(3, 0, tzinfo=left_tz)
        right = time(4, 0, tzinfo=right_tz)
        operation = target.rsplit(" ", 1)[1]
        return {
            "<": lambda: left < right,
            "<=": lambda: left <= right,
            "==": lambda: left == right,
            "!=": lambda: left != right,
            ">": lambda: left > right,
            ">=": lambda: left >= right,
        }[operation]()

    method = target.rsplit(".", 1)[1]
    trigger = "utcoffset" if method == "isoformat" else method
    tz = _CallbackTZ(choose, trigger)
    value = time(3, 4, 5, 678901, tzinfo=tz)
    if method == "utcoffset":
        return value.utcoffset()
    if method == "dst":
        return value.dst()
    if method == "tzname":
        return value.tzname()
    if method == "isoformat":
        return value.isoformat(timespec="microseconds")
    raise AssertionError(target)


def _run_datetime_target(target: str, choose: Choose) -> Any:
    if target.startswith("datetime.datetime fold-"):
        trigger_left = "fold-left" in target
        left_tz = _FoldTZ(choose if trigger_left else None)
        right_tz = _FoldTZ(None if trigger_left else choose)
        left = datetime(2000, 1, 2, 3, 0, tzinfo=left_tz)
        right = datetime(2000, 1, 2, 3, 0, tzinfo=right_tz)
        return left == right if target.endswith("==") else left != right

    if target.startswith("datetime.datetime aware comparison "):
        right_callback = " comparison right " in target
        trigger_occurrence = 2 if " comparison second " in target else 1
        left_tz = (
            _FixedTZ(3)
            if right_callback
            else _CallbackTZ(
                choose,
                "utcoffset",
                comparison=True,
                trigger_occurrence=trigger_occurrence,
            )
        )
        right_tz = _CallbackTZ(choose, "utcoffset", comparison=True) if right_callback else _FixedTZ(2)
        left = datetime(2000, 1, 2, 3, 0, tzinfo=left_tz)
        right = datetime(2000, 1, 2, 3, 0, tzinfo=right_tz)
        operation = target.rsplit(" ", 1)[1]
        return {
            "<": lambda: left < right,
            "<=": lambda: left <= right,
            "==": lambda: left == right,
            "!=": lambda: left != right,
            ">": lambda: left > right,
            ">=": lambda: left >= right,
        }[operation]()
    if target in (
        "datetime.datetime aware subtraction",
        "datetime.datetime aware subtraction right",
    ):
        right_callback = target.endswith(" right")
        left_tz = _FixedTZ(3) if right_callback else _CallbackTZ(choose, "utcoffset", comparison=True)
        right_tz = _CallbackTZ(choose, "utcoffset", comparison=True) if right_callback else _FixedTZ(2)
        left = datetime(2000, 1, 2, 3, 0, tzinfo=left_tz)
        right = datetime(2000, 1, 2, 3, 0, tzinfo=right_tz)
        return left - right

    if target == "datetime.datetime.utcoffset":
        trigger = "utcoffset"
    elif target == "datetime.datetime.dst":
        trigger = "dst"
    elif target == "datetime.datetime.tzname":
        trigger = "tzname"
    elif target.startswith("datetime.datetime.strftime"):
        trigger = target.rsplit("tzinfo.", 1)[1].removesuffix(")")
    else:
        trigger = "utcoffset"
    tz = _CallbackTZ(choose, trigger)
    value = datetime(2000, 1, 2, 3, 4, 5, 678901, tzinfo=tz)
    if target == "datetime.datetime.utcoffset":
        return value.utcoffset()
    if target == "datetime.datetime.dst":
        return value.dst()
    if target == "datetime.datetime.tzname":
        return value.tzname()
    if target.startswith("datetime.datetime.strftime"):
        return value.strftime("%Y-%m-%d %H:%M:%S.%f %z %Z")
    if target == "datetime.datetime.isoformat":
        return value.isoformat(timespec="microseconds")
    raise AssertionError(target)


_TZIF_READS = {
    4: b"TZif",
    1: b"\0",
    15: b"\0" * 15,
    24: struct.pack(">6l", 0, 0, 0, 0, 1, 4),
    6: struct.pack(">lbb", 0, 0, 0),
}
_TZIF_ABBR = b"UTC\0"


class _CallbackFile:
    def __init__(self, choose: Choose) -> None:
        self._choose = choose

    def read(self, size: int = -1) -> bytes:
        if size == 24:
            decision = self._choose()
            if decision == "raise":
                raise ExpectedCallbackError("file.read callback failed")
            if decision == "empty":
                return b""
            if decision == "short":
                return _TZIF_READS[size][:1]
        if size == 4 and not hasattr(self, "_abbr_read"):
            self._abbr_read = True
            return _TZIF_READS[size]
        if size == 4:
            return _TZIF_ABBR
        return _TZIF_READS[size]


def _run_zoneinfo_target(choose: Choose) -> Any:
    zone = ZoneInfo.from_file(cast(Any, _CallbackFile(choose)), key="issue56-utc")
    return zone.key, zone.utcoffset(None)


def _register_cases() -> None:
    for target in _DATETIME_CONVERSION_TARGETS:
        for scenario in _SCENARIOS:
            key = f"conversion::{target}::{scenario}"

            def conversion_case(target: str = target, scenario: CaseKind = scenario) -> None:
                decisions = _scenario_decisions(scenario)
                outcomes = _resume_against_fresh(
                    lambda choose, _decision: _run_datetime_conversion(target, choose),
                    lambda decision: _run_datetime_conversion(target, lambda: decision),
                    decisions,
                    f"datetime-conversion-{target}",
                )
                _assert_scenario(outcomes, scenario)

            _CASES[key] = (scenario, conversion_case)

    for target in _TIME_TARGETS:
        for scenario in _SCENARIOS:
            key = f"time::{target}::{scenario}"

            def time_case(target: str = target, scenario: CaseKind = scenario) -> None:
                decisions = _scenario_decisions(scenario)
                outcomes = _resume_against_fresh(
                    lambda choose, _decision: _run_time_target(target, choose),
                    lambda decision: _run_time_target(target, lambda: decision),
                    decisions,
                    f"datetime-time-{target}",
                )
                _assert_scenario(outcomes, scenario)

            _CASES[key] = (scenario, time_case)

    for target in _DATETIME_TARGETS:
        for scenario in _SCENARIOS:
            key = f"datetime::{target}::{scenario}"

            def datetime_case(target: str = target, scenario: CaseKind = scenario) -> None:
                decisions = _scenario_decisions(scenario)
                outcomes = _resume_against_fresh(
                    lambda choose, _decision: _run_datetime_target(target, choose),
                    lambda decision: _run_datetime_target(target, lambda: decision),
                    decisions,
                    f"datetime-instance-{target}",
                )
                _assert_scenario(outcomes, scenario)

            _CASES[key] = (scenario, datetime_case)

    for scenario in _SCENARIOS:
        key = f"zoneinfo::{_ZONEINFO_TARGETS[0]}::{scenario}"

        def zoneinfo_case(scenario: CaseKind = scenario) -> None:
            decisions = {
                "normal": ("first", "second", "first"),
                "error": ("raise", "continue", "raise"),
                "corner": ("short", "continue", "short"),
                "multishot": ("second", "first", "second"),
            }[scenario]
            outcomes = _resume_against_fresh(
                lambda choose, _decision: _run_zoneinfo_target(choose),
                lambda decision: _run_zoneinfo_target(lambda: decision),
                decisions,
                "zoneinfo-from-file",
            )
            _assert_scenario(outcomes, scenario)

        _CASES[key] = (scenario, zoneinfo_case)


_register_cases()


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=20,
        check=False,
    )


@pytest.mark.parametrize("target", _DATETIME_CONVERSION_TARGETS, ids=_DATETIME_CONVERSION_TARGETS)
@pytest.mark.parametrize("scenario", _SCENARIOS)
def test_datetime_conversion_target_continuations(target: str, scenario: CaseKind) -> None:
    result = _run_case(f"conversion::{target}::{scenario}")
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("target", _TIME_TARGETS, ids=_TIME_TARGETS)
@pytest.mark.parametrize("scenario", _SCENARIOS)
def test_time_target_continuations(target: str, scenario: CaseKind) -> None:
    result = _run_case(f"time::{target}::{scenario}")
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("target", _DATETIME_TARGETS, ids=_DATETIME_TARGETS)
@pytest.mark.parametrize("scenario", _SCENARIOS)
def test_datetime_target_continuations(target: str, scenario: CaseKind) -> None:
    result = _run_case(f"datetime::{target}::{scenario}")
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("target", _ZONEINFO_TARGETS, ids=_ZONEINFO_TARGETS)
@pytest.mark.parametrize("scenario", _SCENARIOS)
def test_zoneinfo_target_continuations(target: str, scenario: CaseKind) -> None:
    result = _run_case(f"zoneinfo::{target}::{scenario}")
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_datetime_zoneinfo.py --case CASE")
    _CASES[sys.argv[2]][1]()
