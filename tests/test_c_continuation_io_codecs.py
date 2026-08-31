"""Strict subprocess-isolated continuation tests for I/O and codec callbacks."""

from __future__ import annotations

from collections.abc import Callable
import codecs
import io
import itertools
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Literal, cast

import pytest

from aleff import create_handler, effect


Choose = Callable[[], Any]
Case = Callable[[], None]
Outcome = tuple[str, Any]
CaseKind = Literal["normal", "error", "corner"]
_CASES: dict[str, tuple[CaseKind, Case]] = {}
_CODEC_NAMES = itertools.count()


class ExpectedCallbackError(Exception):
    """An exception used to verify callback-exception restoration."""


def _case(kind: CaseKind, name: str) -> Callable[[Case], Case]:
    def register(case: Case) -> Case:
        _CASES[name] = (kind, case)
        return case

    return register


def _outcome(call: Callable[[], Any]) -> Outcome:
    try:
        return "return", call()
    except BaseException as exc:
        return "raise", (type(exc).__name__, str(exc))


def _callback(value: Any, operation: str) -> Any:
    if value == "raise":
        raise ExpectedCallbackError(f"{operation} callback failed")
    return value


def _resume_against_fresh(
    run: Callable[[Choose], Any],
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
    effect_name: str,
    normalize: Callable[[Outcome], Outcome] | None = None,
) -> list[Outcome]:
    choose = effect(effect_name)
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        outcomes: list[Outcome] = []
        for decision in decisions:
            expected = _outcome(lambda decision=decision: fresh(decision))
            actual = _outcome(lambda decision=decision: k(decision))
            comparable_actual = normalize(actual) if normalize is not None else actual
            comparable_expected = normalize(expected) if normalize is not None else expected
            assert comparable_actual == comparable_expected, (
                f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            )
            outcomes.append(actual)
        return outcomes

    result = handler(lambda: run(choose))
    assert suspension_count == 1, f"{effect_name} must suspend exactly once"
    return cast(list[Outcome], result)


def _resume_repeated_callback_against_fresh(
    run: Callable[[Choose], Any],
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
    effect_name: str,
) -> list[Outcome]:
    choose = effect(effect_name)
    handler = create_handler(choose)
    branching = False
    current_decision: Any = None
    first_suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> Any:
        nonlocal branching, current_decision, first_suspension_count
        if branching:
            return k(current_decision)
        first_suspension_count += 1
        branching = True
        outcomes: list[Outcome] = []
        try:
            for decision in decisions:
                current_decision = decision
                expected = _outcome(lambda decision=decision: fresh(decision))
                actual = _outcome(lambda decision=decision: k(decision))
                assert actual == expected, (
                    f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
                )
                outcomes.append(actual)
        finally:
            branching = False
        return outcomes

    result = handler(lambda: run(choose))
    assert first_suspension_count == 1, f"{effect_name} must have one branching suspension"
    return cast(list[Outcome], result)


class _LineIO(io.IOBase):
    def __init__(self, callback: Choose, operation: str) -> None:
        self._callback = callback
        self._operation = operation
        self._line_pending = True
        self.writes: tuple[str, ...] = ()

    def readline(self, size: int | None = -1) -> str:  # pyright: ignore[reportIncompatibleMethodOverride]
        if not self._line_pending:
            return ""
        self._line_pending = False
        return _callback(self._callback(), self._operation)

    def write(self, data: str) -> int:
        result = _callback(self._callback(), self._operation)
        self.writes += (data,)
        return result


class _ClosedTruth:
    def __init__(self, callback: Choose) -> None:
        self._callback = callback

    def __bool__(self) -> bool:
        return _callback(self._callback(), "IOBase.closed truth")


class _ClosedPropertyIO(io.IOBase):
    def __init__(self, callback: Choose, *, truth_callback: bool = False) -> None:
        self._callback = callback
        self._truth_callback = truth_callback

    @property
    def closed(self) -> Any:
        if self._truth_callback:
            return _ClosedTruth(self._callback)
        return _callback(self._callback(), "IOBase.closed property")

    def disable_callback(self) -> None:
        self._callback = lambda: True


class _ClosePropertyIO(_ClosedPropertyIO):
    def __init__(self, callback: Choose, *, truth_callback: bool = False) -> None:
        super().__init__(callback, truth_callback=truth_callback)
        self.flush_count = 0

    def flush(self) -> None:
        self.flush_count += 1


def _iobase_close_closed_case(callback: Choose, *, truth_callback: bool = False) -> Any:
    stream = _ClosePropertyIO(callback, truth_callback=truth_callback)
    try:
        result = io.IOBase.close(stream)
        return result, stream.flush_count, stream.__dict__.get("__IOBase_closed", False)
    finally:
        stream.disable_callback()


@_case("corner", "iobase_close_restores_closed_property_and_truth_phases")
def _iobase_close_restores_closed_property_and_truth_phases() -> None:
    for truth_callback in (False, True):
        outcomes = _resume_against_fresh(
            lambda choose, truth_callback=truth_callback: _iobase_close_closed_case(
                lambda: choose(), truth_callback=truth_callback
            ),
            lambda decision, truth_callback=truth_callback: _iobase_close_closed_case(
                lambda: decision, truth_callback=truth_callback
            ),
            ("raise", False, True, False),
            f"iobase-close-closed-{'truth' if truth_callback else 'property'}",
        )
        assert outcomes[0] == (
            "raise",
            (
                "ExpectedCallbackError",
                f"IOBase.closed {'truth' if truth_callback else 'property'} callback failed",
            ),
        )
        assert outcomes[1] == outcomes[3] == ("return", (None, 1, True))
        assert outcomes[2] == ("return", (None, 0, False))


def _iobase_iter_closed_case(
    callback: Choose,
    *,
    truth_callback: bool = False,
    keepalive: list[_ClosedPropertyIO] | None = None,
) -> bool:
    stream = _ClosedPropertyIO(callback, truth_callback=truth_callback)
    if keepalive is not None:
        keepalive.append(stream)
    return iter(stream) is stream


def _close_iobase_streams(streams: list[_ClosedPropertyIO]) -> None:
    for stream in streams:
        stream.disable_callback()
        stream.close()
    streams.clear()


@_case("normal", "iobase_iter_checks_effectful_closed_property")
def _iobase_iter_checks_effectful_closed_property() -> None:
    streams: list[_ClosedPropertyIO] = []
    try:
        outcomes = _resume_against_fresh(
            lambda choose: _iobase_iter_closed_case(lambda: choose(), keepalive=streams),
            lambda decision: _iobase_iter_closed_case(lambda: decision, keepalive=streams),
            (False, True, False),
            "iobase-iter-closed-property",
        )
    finally:
        _close_iobase_streams(streams)
    assert outcomes == [
        ("return", True),
        ("raise", ("ValueError", "I/O operation on closed file.")),
        ("return", True),
    ]


@_case("error", "iobase_iter_restores_effectful_closed_property_errors")
def _iobase_iter_restores_effectful_closed_property_errors() -> None:
    streams: list[_ClosedPropertyIO] = []
    try:
        outcomes = _resume_against_fresh(
            lambda choose: _iobase_iter_closed_case(lambda: choose(), keepalive=streams),
            lambda decision: _iobase_iter_closed_case(lambda: decision, keepalive=streams),
            ("raise", False, "raise"),
            "iobase-iter-closed-property-error",
        )
    finally:
        _close_iobase_streams(streams)
    assert outcomes == [
        ("raise", ("ExpectedCallbackError", "IOBase.closed property callback failed")),
        ("return", True),
        ("raise", ("ExpectedCallbackError", "IOBase.closed property callback failed")),
    ]


@_case("corner", "iobase_iter_checks_effectful_closed_truth_value")
def _iobase_iter_checks_effectful_closed_truth_value() -> None:
    streams: list[_ClosedPropertyIO] = []
    try:
        outcomes = _resume_against_fresh(
            lambda choose: _iobase_iter_closed_case(
                lambda: choose(), truth_callback=True, keepalive=streams
            ),
            lambda decision: _iobase_iter_closed_case(
                lambda: decision, truth_callback=True, keepalive=streams
            ),
            (False, "raise", True, False),
            "iobase-iter-closed-truth",
        )
    finally:
        _close_iobase_streams(streams)
    assert outcomes == [
        ("return", True),
        ("raise", ("ExpectedCallbackError", "IOBase.closed truth callback failed")),
        ("raise", ("ValueError", "I/O operation on closed file.")),
        ("return", True),
    ]


@_case("normal", "iobase_iter_returns_the_stream_and_next_reads_a_line")
def _iobase_iter_returns_the_stream_and_next_reads_a_line() -> None:
    def run(choose: Choose) -> Any:
        stream = _LineIO(lambda: choose(), "IOBase.__next__")
        return iter(stream) is stream, next(stream)

    def fresh(decision: Any) -> Any:
        stream = _LineIO(lambda: decision, "IOBase.__next__")
        return iter(stream) is stream, next(stream)

    assert _resume_against_fresh(run, fresh, ("line\n", "line\n"), "iobase-next") == [
        ("return", (True, "line\n")),
        ("return", (True, "line\n")),
    ]


@_case("error", "iobase_next_restores_callback_errors")
def _iobase_next_restores_callback_errors() -> None:
    def run(choose: Choose) -> Any:
        return next(_LineIO(lambda: choose(), "IOBase.__next__"))

    def fresh(decision: Any) -> Any:
        return next(_LineIO(lambda: decision, "IOBase.__next__"))

    assert _resume_against_fresh(run, fresh, ("raise", "line\n", "raise"), "iobase-next-error") == [
        ("raise", ("ExpectedCallbackError", "IOBase.__next__ callback failed")),
        ("return", "line\n"),
        ("raise", ("ExpectedCallbackError", "IOBase.__next__ callback failed")),
    ]


@_case("corner", "iobase_readlines_cross_virtual_methods")
def _iobase_readlines_cross_virtual_methods() -> None:
    def readlines_run(choose: Choose) -> Any:
        return _LineIO(lambda: choose(), "IOBase.readlines").readlines()

    def readlines_fresh(decision: Any) -> Any:
        return _LineIO(lambda: decision, "IOBase.readlines").readlines()

    read_outcomes = _resume_against_fresh(
        readlines_run,
        readlines_fresh,
        ("first\n", "raise", "first\n"),
        "iobase-readlines",
    )
    assert read_outcomes == [
        ("return", ["first\n"]),
        ("raise", ("ExpectedCallbackError", "IOBase.readlines callback failed")),
        ("return", ["first\n"]),
    ]


@_case("corner", "iobase_writelines_isolate_mutable_state")
def _iobase_writelines_isolate_mutable_state() -> None:
    def writelines_run(choose: Choose) -> Any:
        stream = _LineIO(lambda: choose(), "IOBase.writelines")
        result = stream.writelines(cast(Any, ["written\n"]))
        return result, stream.writes

    def writelines_fresh(decision: Any) -> Any:
        stream = _LineIO(lambda: decision, "IOBase.writelines")
        result = stream.writelines(cast(Any, ["written\n"]))
        return result, stream.writes

    assert _resume_against_fresh(writelines_run, writelines_fresh, (8, "raise", 8), "iobase-writelines") == [
        ("return", (None, ("written\n",))),
        ("raise", ("ExpectedCallbackError", "IOBase.writelines callback failed")),
        ("return", (None, ("written\n",))),
    ]


def _open_with_opener(callback: Choose, path: str) -> Any:
    def opener(opener_path: str, flags: int) -> int | None:
        decision = _callback(callback(), "io.open opener")
        if decision == "fd":
            return os.open(opener_path, flags)
        return None

    try:
        stream: Any = io.open(path, "rb", opener=cast(Any, opener))
    except BaseException as exc:
        return "raise", (type(exc).__name__, str(exc))
    try:
        return "return", stream.read()
    finally:
        stream.close()


@_case("normal", "io_open_opener_multishot_matches_fresh_execution")
def _io_open_opener_multishot_matches_fresh_execution() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "opener.txt")
        with open(path, "wb") as stream:
            stream.write(b"opener")
        outcomes = _resume_against_fresh(
            lambda choose: _open_with_opener(lambda: choose(), path),
            lambda decision: _open_with_opener(lambda: decision, path),
            ("fd", "bad", "fd"),
            "io-open-opener",
        )
    assert outcomes[0] == outcomes[2] == ("return", ("return", b"opener"))
    assert outcomes[1][0] == "return"
    assert outcomes[1][1][0] == "raise"
    assert outcomes[1][1][1][0] == "TypeError"


@_case("error", "io_open_opener_callback_errors_are_isolated")
def _io_open_opener_callback_errors_are_isolated() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "opener.txt")
        with open(path, "wb") as stream:
            stream.write(b"opener")
        outcomes = _resume_against_fresh(
            lambda choose: _open_with_opener(lambda: choose(), path),
            lambda decision: _open_with_opener(lambda: decision, path),
            ("raise", "fd", "raise"),
            "io-open-opener-error",
        )
    assert outcomes[0] == outcomes[2]
    assert outcomes[0][0] == "return"
    assert outcomes[0][1][0] == "raise"
    assert outcomes[0][1][1][0] == "ExpectedCallbackError"
    assert outcomes[1] == ("return", ("return", b"opener"))


class _Raw(io.RawIOBase):
    def __init__(self, callback: Choose, target: str) -> None:
        self._callback = callback
        self._target = target
        self.armed = False
        self.events: tuple[tuple[str, Any], ...] = ()

    def readable(self) -> bool:
        return True

    def writable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return True

    def _value(self, name: str, default: Any) -> Any:
        if not self.armed:
            return default
        if name != self._target:
            self.events += ((name, default),)
            return default
        value = _callback(self._callback(), f"raw {name}")
        if value == "raise":
            raise AssertionError("unreachable")
        self.events += ((name, value),)
        return value

    def read(self, size: int = -1) -> bytes:
        value = self._value("read", b"R\n")
        return value

    def readinto(self, buffer: Any) -> int:
        if self._target == "read":
            data = self.read(len(buffer))
        else:
            value = self._value("readinto", 1)
            data = b"R\n" if value else b""
        amount = min(len(buffer), len(data))
        buffer[:amount] = data[:amount]
        return amount

    def write(self, data: Any) -> int:
        return self._value("write", len(data))

    def seek(self, offset: int, whence: int = os.SEEK_SET) -> int:
        return self._value("seek", 0)

    def tell(self) -> int:
        return self._value("tell", 0)

    def flush(self) -> None:
        self._value("flush", None)

    def close(self) -> None:
        self._value("close", None)


class _LifecycleRaw(io.RawIOBase):
    def __init__(self, callback: Choose, target: str) -> None:
        self._callback = callback
        self._target = target
        self.armed = False
        self.events: tuple[str, ...] = ()

    def readable(self) -> bool:
        return True

    def readinto(self, buffer: Any) -> int:
        return 0

    def flush(self) -> None:
        if self.armed and self._target == "flush":
            _callback(self._callback(), "raw flush")
        if self.armed:
            self.events += ("flush",)

    def close(self) -> None:
        if self.closed:
            return
        if self.armed and self._target == "close":
            _callback(self._callback(), "raw close")
        if self.armed:
            self.events += ("close",)
        super().close()


def _buffered_reader_lifecycle_case(target: str, callback: Choose) -> Any:
    raw = _LifecycleRaw(callback, target)
    stream = io.BufferedReader(raw, 1)
    raw.armed = True
    stream.close()
    return raw.events, raw.closed


def _buffer_case(buffer_kind: str, method: str, target: str, callback: Choose) -> Any:
    raw = _Raw(callback, target)
    if buffer_kind == "reader":
        stream: Any = io.BufferedReader(raw, buffer_size=1)
        raw.armed = True
        if method == "read":
            result = stream.read(1)
        elif method == "read1":
            result = stream.read1(1)
        elif method == "readinto":
            destination = bytearray(1)
            result = (stream.readinto(destination), bytes(destination))
        elif method == "readinto1":
            destination = bytearray(1)
            result = (stream.readinto1(destination), bytes(destination))
        elif method == "peek":
            result = stream.peek(1)[:1]
        elif method == "seek":
            result = stream.seek(0)
        elif method == "tell":
            result = stream.tell()
        else:
            raise AssertionError(method)
        return result, raw.events
    if buffer_kind == "writer":
        stream = io.BufferedWriter(raw, buffer_size=1)
        raw.armed = True
        if method == "write":
            result = stream.write(b"WW")
        elif method == "writelines":
            result = stream.writelines([b"WW"])
        elif method == "flush":
            stream.write(b"W")
            result = stream.flush()
        elif method == "seek":
            result = stream.seek(0)
        elif method == "tell":
            result = stream.tell()
        else:
            raise AssertionError(method)
        return result, raw.events
    if buffer_kind == "random":
        stream = io.BufferedRandom(raw, buffer_size=1)
        raw.armed = True
        if method == "read":
            result = stream.read(1)
        elif method == "readinto":
            destination = bytearray(1)
            result = (stream.readinto(destination), bytes(destination))
        elif method == "write":
            result = stream.write(b"WW")
        elif method == "seek":
            result = stream.seek(0)
        elif method == "tell":
            result = stream.tell()
        elif method == "flush":
            stream.write(b"W")
            result = stream.flush()
        else:
            raise AssertionError(method)
        return result, raw.events
    if buffer_kind == "rwpair":
        reader = _Raw(callback, target)
        writer = _Raw(callback, target)
        stream = io.BufferedRWPair(reader, writer, 1)
        reader.armed = True
        writer.armed = True
        if method == "read":
            result = stream.read(1)
        elif method == "readinto":
            destination = bytearray(1)
            result = (stream.readinto(destination), bytes(destination))
        elif method == "write":
            result = stream.write(b"WW")
        elif method == "flush":
            stream.write(b"W")
            result = stream.flush()
        else:
            raise AssertionError(method)
        return result, reader.events + writer.events
    raise AssertionError(buffer_kind)


def _buffer_outcomes(
    buffer_kind: str, method: str, target: str, decisions: tuple[Any, ...], name: str
) -> list[Outcome]:
    return _resume_against_fresh(
        lambda choose: _buffer_case(buffer_kind, method, target, lambda: choose()),
        lambda decision: _buffer_case(buffer_kind, method, target, lambda: decision),
        decisions,
        name,
    )


@_case("normal", "buffered_reader_read_and_readinto_raw_callbacks")
def _buffered_reader_read_and_readinto_raw_callbacks() -> None:
    assert _buffer_outcomes("reader", "read", "readinto", (1, 1, 1), "buffer-reader-read") == [
        ("return", (b"R", (("readinto", 1),))),
        ("return", (b"R", (("readinto", 1),))),
        ("return", (b"R", (("readinto", 1),))),
    ]
    for method, target in (("seek", "seek"), ("tell", "tell")):
        expected_event = ((target, 0),)
        assert all(
            outcome == ("return", (0, expected_event))
            for outcome in _buffer_outcomes("reader", method, target, (0, 0, 0), f"buffer-reader-{method}")
        )
    assert _buffer_outcomes("reader", "readinto", "readinto", (1, 1, 1), "buffer-reader-readinto") == [
        ("return", ((1, b"R"), (("readinto", 1),))),
        ("return", ((1, b"R"), (("readinto", 1),))),
        ("return", ((1, b"R"), (("readinto", 1),))),
    ]


@_case("corner", "buffered_reader_all_read_methods_invoke_readinto")
def _buffered_reader_all_read_methods_invoke_readinto() -> None:
    expected_results = {"read1": b"R", "readinto1": (1, b"R"), "peek": b"R"}
    for method in ("read1", "readinto1", "peek"):
        outcomes = _buffer_outcomes("reader", method, "readinto", (1, "raise", 1), f"buffer-reader-{method}")
        expected = ("return", (expected_results[method], (("readinto", 1),)))
        assert outcomes[0] == outcomes[2] == expected
        assert outcomes[1][0] == "raise"
        assert "ExpectedCallbackError" in repr(outcomes[1])


@_case("error", "buffered_reader_read_restores_raw_readinto_errors")
def _buffered_reader_read_restores_raw_readinto_errors() -> None:
    outcomes = _buffer_outcomes(
        "reader", "read", "readinto", ("raise", 1, "raise"), "buffer-reader-read-error"
    )
    assert outcomes == [
        ("raise", ("ExpectedCallbackError", "raw readinto callback failed")),
        ("return", (b"R", (("readinto", 1),))),
        ("raise", ("ExpectedCallbackError", "raw readinto callback failed")),
    ]


@_case("error", "buffered_writer_methods_restore_raw_callback_errors")
def _buffered_writer_methods_restore_raw_callback_errors() -> None:
    for method, target in (
        ("write", "write"),
        ("writelines", "write"),
        ("flush", "write"),
        ("seek", "seek"),
        ("tell", "tell"),
    ):
        normal_decision = 1 if method == "flush" else 2 if target == "write" else 0
        outcomes = _buffer_outcomes(
            "writer", method, target, ("raise", normal_decision, "raise"), f"buffer-writer-{method}"
        )
        assert outcomes[0] == outcomes[2]
        assert outcomes[0][0] == "raise"
        expected_result = None if method in ("flush", "writelines") else normal_decision
        assert outcomes[1] == ("return", (expected_result, ((target, normal_decision),)))


@_case("normal", "buffered_rw_pair_read_write_and_flush_callbacks")
def _buffered_rw_pair_read_write_and_flush_callbacks() -> None:
    for method, target, decision, expected in (
        ("read", "readinto", 1, b"R"),
        ("readinto", "readinto", 1, (1, b"R")),
        ("write", "write", 2, 2),
        ("flush", "write", 1, None),
    ):
        outcomes = _buffer_outcomes("rwpair", method, target, (decision, decision, decision), f"buffer-rwpair-{method}")
        assert outcomes[0] == outcomes[1] == outcomes[2] == ("return", (expected, ((target, decision),)))


@_case("normal", "buffered_random_seek_tell_and_read_write_callbacks")
def _buffered_random_seek_tell_and_read_write_callbacks() -> None:
    for method, target, decisions in (
        ("read", "readinto", (1, 1, 1)),
        ("readinto", "readinto", (1, 1, 1)),
        ("write", "write", (2, 2, 2)),
        ("seek", "seek", (0, 0, 0)),
        ("tell", "tell", (0, 0, 0)),
        ("flush", "write", (1, 1, 1)),
    ):
        outcomes = _buffer_outcomes("random", method, target, decisions, f"buffer-random-{method}")
        expected_result: Any
        if method == "read":
            expected_result = b"R"
        elif method == "readinto":
            expected_result = (1, b"R")
        elif method == "write":
            expected_result = 2
        elif method == "flush":
            expected_result = None
        else:
            expected_result = 0
        expected_events = ((target, decisions[0]),)
        if method in ("read", "readinto"):
            expected_events = (("seek", 0), (target, decisions[0]))
        if method == "flush":
            expected_events = ((target, decisions[0]), ("seek", 0))
        assert outcomes[0] == outcomes[1] == outcomes[2] == ("return", (expected_result, expected_events))


@_case("error", "buffered_random_compound_phases_restore_after_raw_errors")
def _buffered_random_compound_phases_restore_after_raw_errors() -> None:
    read_outcomes = _buffer_outcomes(
        "random", "read", "seek", ("raise", 0, "raise"), "buffer-random-read-seek-error"
    )
    assert read_outcomes == [
        ("raise", ("ExpectedCallbackError", "raw seek callback failed")),
        ("return", (b"R", (("seek", 0), ("readinto", 1)))),
        ("raise", ("ExpectedCallbackError", "raw seek callback failed")),
    ]

    flush_outcomes = _buffer_outcomes(
        "random", "flush", "write", ("raise", 1, "raise"), "buffer-random-flush-write-error"
    )
    assert flush_outcomes == [
        ("raise", ("ExpectedCallbackError", "raw write callback failed")),
        ("return", (None, (("write", 1), ("seek", 0)))),
        ("raise", ("ExpectedCallbackError", "raw write callback failed")),
    ]


@_case("corner", "buffered_rw_pair_write_flush_phase_isolated_per_shot")
def _buffered_rw_pair_write_flush_phase_isolated_per_shot() -> None:
    outcomes = _buffer_outcomes(
        "rwpair", "flush", "write", ("raise", 1, "raise"), "buffer-rwpair-flush-write-error"
    )
    assert outcomes == [
        ("raise", ("ExpectedCallbackError", "raw write callback failed")),
        ("return", (None, (("write", 1),))),
        ("raise", ("ExpectedCallbackError", "raw write callback failed")),
    ]


@_case("error", "buffered_close_restores_raw_close_errors")
def _buffered_close_restores_raw_close_errors() -> None:
    for buffer_kind in ("reader", "writer", "random"):

        def run(choose: Choose, buffer_kind: str = buffer_kind) -> Any:
            raw = _Raw(lambda: choose(), "close")
            if buffer_kind == "reader":
                stream: Any = io.BufferedReader(raw, 1)
            elif buffer_kind == "writer":
                stream = io.BufferedWriter(raw, 1)
            else:
                stream = io.BufferedRandom(raw, 1)
            raw.armed = True
            return stream.close()

        def fresh(decision: Any, buffer_kind: str = buffer_kind) -> Any:
            raw = _Raw(lambda: decision, "close")
            if buffer_kind == "reader":
                stream: Any = io.BufferedReader(raw, 1)
            elif buffer_kind == "writer":
                stream = io.BufferedWriter(raw, 1)
            else:
                stream = io.BufferedRandom(raw, 1)
            raw.armed = True
            return stream.close()

        outcomes = _resume_against_fresh(run, fresh, ("raise", None, "raise"), f"buffer-{buffer_kind}-close")
        assert outcomes == [
            ("raise", ("ExpectedCallbackError", "raw close callback failed")),
            ("return", None),
            ("raise", ("ExpectedCallbackError", "raw close callback failed")),
        ]

    for side in ("reader", "writer"):

        def pair_run(choose: Choose, side: str = side) -> Any:
            reader = _Raw(lambda: choose(), "close" if side == "reader" else "never")
            writer = _Raw(lambda: choose(), "close" if side == "writer" else "never")
            reader.armed = True
            writer.armed = True
            return io.BufferedRWPair(reader, writer, 1).close()

        def pair_fresh(decision: Any, side: str = side) -> Any:
            reader = _Raw(lambda: decision, "close" if side == "reader" else "never")
            writer = _Raw(lambda: decision, "close" if side == "writer" else "never")
            reader.armed = True
            writer.armed = True
            return io.BufferedRWPair(reader, writer, 1).close()

        outcomes = _resume_against_fresh(
            pair_run, pair_fresh, ("raise", None, "raise"), f"buffer-rwpair-{side}-close"
        )
        assert outcomes == [
            ("raise", ("ExpectedCallbackError", "raw close callback failed")),
            ("return", None),
            ("raise", ("ExpectedCallbackError", "raw close callback failed")),
        ]


@_case("corner", "buffered_close_restores_raw_flush_and_close_phases")
def _buffered_close_restores_raw_flush_and_close_phases() -> None:
    for target, expected_events in (
        ("flush", ("flush", "close", "flush")),
        ("close", ("flush", "close", "flush")),
    ):
        outcomes = _resume_repeated_callback_against_fresh(
            lambda choose, target=target: _buffered_reader_lifecycle_case(target, lambda: choose()),
            lambda decision, target=target: _buffered_reader_lifecycle_case(target, lambda: decision),
            ("raise", None, "raise"),
            f"buffer-reader-{target}-lifecycle",
        )
        assert outcomes == [
            ("raise", ("ExpectedCallbackError", f"raw {target} callback failed")),
            ("return", (expected_events, True)),
            ("raise", ("ExpectedCallbackError", f"raw {target} callback failed")),
        ]


class _TextBuffer(io.BytesIO):
    def __init__(self, callback: Choose, target: str, payload: bytes) -> None:
        super().__init__(b"line\n")
        self._callback = callback
        self._target = target
        self._payload = payload
        self.armed = False
        self.events: tuple[str, ...] = ()

    def _hook(self, name: str) -> None:
        if self.armed and name == self._target:
            self.armed = False
            _callback(self._callback(), f"text buffer {name}")
            self.events += (name,)

    def read1(self, size: int | None = -1) -> bytes:
        self._hook("read1")
        return self._payload if size != 0 else b""

    def write(self, data: Any) -> int:
        self._hook("write")
        return len(data)

    def seek(self, offset: int, whence: int = os.SEEK_SET) -> int:
        self._hook("seek")
        return 0

    def tell(self) -> int:
        self._hook("tell")
        return 0

    def flush(self) -> None:
        self._hook("flush")

    def close(self) -> None:
        self._hook("close")


def _text_case(method: str, callback: Choose) -> Any:
    payload = b"line\n" if method == "readline" else b"X"
    buffer = _TextBuffer(
        callback,
        {
            "read": "read1",
            "readline": "read1",
            "write": "write",
            "seek": "seek",
            "tell": "tell",
            "flush": "flush",
            "close": "close",
        }[method],
        payload,
    )
    stream = io.TextIOWrapper(buffer, encoding="ascii", write_through=True)
    buffer.armed = True
    if method == "read":
        result = stream.read(1)
    elif method == "readline":
        result = stream.readline()
    elif method == "write":
        result = stream.write("W")
    elif method == "seek":
        result = stream.seek(0)
    elif method == "tell":
        result = stream.tell()
    elif method == "flush":
        result = stream.flush()
    elif method == "close":
        result = stream.close()
    else:
        raise AssertionError(method)
    return result, buffer.events


@_case("normal", "text_iowrapper_all_wrapped_buffer_callbacks")
def _text_iowrapper_all_wrapped_buffer_callbacks() -> None:
    for method in ("read", "readline", "write", "seek", "tell", "flush"):
        outcomes = _resume_against_fresh(
            lambda choose, method=method: _text_case(method, lambda: choose()),
            lambda decision, method=method: _text_case(method, lambda: decision),
            (None, None, None),
            f"text-{method}",
        )
        assert outcomes[0] == outcomes[1] == outcomes[2]
        expected_result: Any
        if method == "read":
            expected_result = "X"
        elif method == "readline":
            expected_result = "line\n"
        elif method == "write":
            expected_result = 1
        elif method in ("seek", "tell"):
            expected_result = 0
        else:
            expected_result = None
        expected_event = {
            "read": "read1",
            "readline": "read1",
            "write": "write",
            "seek": "seek",
            "tell": "tell",
            "flush": "flush",
        }[method]
        assert outcomes[0] == ("return", (expected_result, (expected_event,)))


@_case("error", "text_iowrapper_close_callback_errors_are_multishot_safe")
def _text_iowrapper_close_callback_errors_are_multishot_safe() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _text_case("close", lambda: choose()),
        lambda decision: _text_case("close", lambda: decision),
        ("raise", "raise", "raise"),
        "text-close",
    )
    assert outcomes[0] == outcomes[1] == outcomes[2]
    assert outcomes[0][0] == "raise"


@_case("error", "text_iowrapper_methods_restore_after_wrapped_buffer_errors")
def _text_iowrapper_methods_restore_after_wrapped_buffer_errors() -> None:
    expected_results: dict[str, Any] = {
        "read": "X",
        "readline": "line\n",
        "write": 1,
        "seek": 0,
        "tell": 0,
        "flush": None,
        "close": None,
    }
    expected_events = {
        "read": "read1",
        "readline": "read1",
        "write": "write",
        "seek": "seek",
        "tell": "tell",
        "flush": "flush",
        "close": "close",
    }
    for method in expected_results:
        outcomes = _resume_against_fresh(
            lambda choose, method=method: _text_case(method, lambda: choose()),
            lambda decision, method=method: _text_case(method, lambda: decision),
            ("raise", None, "raise"),
            f"text-{method}-error-restore",
        )
        operation = expected_events[method]
        assert outcomes == [
            ("raise", ("ExpectedCallbackError", f"text buffer {operation} callback failed")),
            ("return", (expected_results[method], (operation,))),
            ("raise", ("ExpectedCallbackError", f"text buffer {operation} callback failed")),
        ]


def _codec_info(name: str) -> codecs.CodecInfo:
    def encode(value: str, errors: str = "strict") -> tuple[bytes, int]:
        return value.encode("ascii"), len(value)

    def decode(value: Any, errors: str = "strict") -> tuple[str, int]:
        return value.decode("ascii"), len(value)

    return codecs.CodecInfo(encode=encode, decode=decode, name="issue56_search_codec")


def _lookup_with_registered_search(callback: Choose) -> tuple[str, str]:
    number = next(_CODEC_NAMES)
    name = f"issue56_search_{number}"

    def search(encoding: str) -> codecs.CodecInfo | None:
        if encoding == name:
            decision = _callback(callback(), "codec search")
            if decision is None:
                return None
            return _codec_info(name)
        return None

    codecs.register(search)
    try:
        return "found", codecs.lookup(name).name
    except LookupError as exc:
        return "missing", type(exc).__name__


def _searched_codec_operation(kind: str, callback: Choose) -> Any:
    number = next(_CODEC_NAMES)
    name = f"issue56_searched_operation_{number}"

    def search(encoding: str) -> Any:
        if encoding != name:
            return None
        decision = _callback(callback(), f"codecs.{kind} search")
        if decision == "codec":
            return _codec_info(name)
        if decision == "invalid":
            return 42
        return None

    codecs.register(search)
    if kind == "encode":
        return codecs.encode("input", name)
    return codecs.decode(b"input", name)


@_case("normal", "codecs_lookup_registered_search_function_is_multishot")
def _codecs_lookup_registered_search_function_is_multishot() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _lookup_with_registered_search(lambda: choose()),
        lambda decision: _lookup_with_registered_search(lambda: decision),
        (None, "ok", None),
        "codecs-lookup-search",
    )
    assert outcomes == [
        ("return", ("missing", "LookupError")),
        ("return", ("found", "issue56_search_codec")),
        ("return", ("missing", "LookupError")),
    ]


@_case("error", "codecs_encode_decode_search_callbacks_cover_all_results")
def _codecs_encode_decode_search_callbacks_cover_all_results() -> None:
    def normalize_lookup_error(outcome: Outcome) -> Outcome:
        if outcome[0] == "raise" and outcome[1][0] == "LookupError":
            return "raise", ("LookupError",)
        return outcome

    for kind, expected in (("encode", b"input"), ("decode", "input")):
        outcomes = _resume_against_fresh(
            lambda choose, kind=kind: _searched_codec_operation(kind, lambda: choose()),
            lambda decision, kind=kind: _searched_codec_operation(kind, lambda: decision),
            (None, "codec", "raise", "invalid", "codec"),
            f"codecs-{kind}-search-results",
            normalize_lookup_error,
        )
        assert outcomes[0][0] == "raise"
        assert outcomes[0][1][0] == "LookupError"
        assert outcomes[1] == outcomes[4] == ("return", expected)
        assert outcomes[2] == (
            "raise",
            ("ExpectedCallbackError", f"codecs.{kind} search callback failed"),
        )
        assert outcomes[3][0] == "raise"
        assert outcomes[3][1][0] == "TypeError"


def _registered_codec_operation(kind: str, callback: Choose) -> Any:
    number = next(_CODEC_NAMES)
    name = f"issue56_codec_{number}"

    def encode(value: str, errors: str = "strict") -> tuple[bytes, int]:
        return _callback(callback(), "codec encode").encode("ascii"), len(value)

    def decode(value: Any, errors: str = "strict") -> tuple[str, int]:
        return _callback(callback(), "codec decode"), len(value)

    info = codecs.CodecInfo(encode=encode, decode=decode, name=name)
    codecs.register(lambda encoding: info if encoding == name else None)
    return codecs.encode("input", name) if kind == "encode" else codecs.decode(b"input", name)


@_case("corner", "codecs_encode_invoke_registered_codec_function")
def _codecs_encode_invoke_registered_codec_function() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _registered_codec_operation("encode", lambda: choose()),
        lambda decision: _registered_codec_operation("encode", lambda: decision),
        ("A", "raise", "A"),
        "codecs-encode",
    )
    assert outcomes[0] == outcomes[2] == ("return", b"A")
    assert outcomes[1][0] == "raise"
    assert outcomes[1][1][0] == "ExpectedCallbackError"


@_case("corner", "codecs_decode_invoke_registered_codec_function")
def _codecs_decode_invoke_registered_codec_function() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _registered_codec_operation("decode", lambda: choose()),
        lambda decision: _registered_codec_operation("decode", lambda: decision),
        ("A", "raise", "A"),
        "codecs-decode",
    )
    assert outcomes[0] == outcomes[2] == ("return", "A")
    assert outcomes[1][0] == "raise"
    assert outcomes[1][1][0] == "ExpectedCallbackError"


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "normal"])
def test_io_codecs_continuation_normal(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "error"])
def test_io_codecs_continuation_error_isolated_per_shot(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "corner"])
def test_io_codecs_continuation_corner(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_io_codecs.py --case CASE_NAME")
    _CASES[sys.argv[2]][1]()
