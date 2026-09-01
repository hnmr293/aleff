"""Strict continuation tests for marshal serialization callbacks."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import marshal
from pathlib import Path
import subprocess
import sys
from typing import Any, cast

import pytest

from aleff import create_async_handler, create_handler, effect


Case = Callable[[], None]
Choose = Callable[[], Any]
Outcome = tuple[str, Any]
_CASES: dict[str, Case] = {}


class ExpectedCallbackError(Exception):
    """An exception used to verify marshal callback restoration."""


def _case(name: str) -> Callable[[Case], Case]:
    def register(test_case: Case) -> Case:
        _CASES[name] = test_case
        return test_case

    return register


def _outcome(call: Callable[[], Any]) -> Outcome:
    try:
        return "return", call()
    except Exception as exc:
        return "raise", (type(exc).__name__, str(exc))


def _callback(value: Any) -> Any:
    if value == "raise":
        raise ExpectedCallbackError("marshal callback failed")
    return value


def _resume_against_fresh(
    run: Callable[[Choose], Any],
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
) -> list[tuple[Outcome, Outcome]]:
    choose = effect("marshal-choice")
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> list[tuple[Outcome, Outcome]]:
        nonlocal suspension_count
        suspension_count += 1
        return [
            (
                _outcome(lambda decision=decision: k(decision)),
                _outcome(lambda decision=decision: fresh(decision)),
            )
            for decision in decisions
        ]

    comparisons = cast(list[tuple[Outcome, Outcome]], handler(lambda: run(choose)))
    assert suspension_count == 1
    assert all(actual == expected for actual, expected in comparisons), comparisons
    return comparisons


def _dump_write(callback: Callable[[], Any]) -> tuple[Any, tuple[bytes, ...]]:
    writes: list[bytes] = []

    class Writer:
        def write(self, data: bytes) -> Any:
            writes.append(data)
            return callback()

    result = marshal.dump((1, "two", [3]), Writer())
    return result, tuple(writes)


def _load_read_zero(callback: Callable[[], Any]) -> tuple[Any, tuple[tuple[str, Any], ...]]:
    encoded = marshal.dumps(1)
    events: list[tuple[str, Any]] = []
    offset = 0

    class Reader:
        def read(self, size: int) -> Any:
            nonlocal offset
            assert size == 0
            events.append(("read", size))
            result = callback()
            offset = 0
            events[:] = [("read", size)]
            events.append(("read-return", result))
            return result

        def readinto(self, buffer: Any) -> int:
            nonlocal offset
            size = len(buffer)
            events.append(("readinto", size))
            chunk = encoded[offset : offset + size]
            buffer[: len(chunk)] = chunk
            offset += len(chunk)
            events.append(("readinto-return", len(chunk)))
            return len(chunk)

    return marshal.load(cast(Any, Reader())), tuple(events)


@_case("load_read_zero_multishot")
def _load_read_zero_multishot() -> None:
    decisions = (b"", b"ignored", "raise", b"")
    comparisons = _resume_against_fresh(
        lambda choose: _load_read_zero(lambda: _callback(choose())),
        lambda decision: _load_read_zero(lambda: _callback(decision)),
        decisions,
    )

    assert [actual[0] for actual, _expected in comparisons] == [
        "return",
        "return",
        "raise",
        "return",
    ]
    for index in (0, 1, 3):
        actual = comparisons[index][0]
        assert actual[0] == "return"
        value, events = cast(tuple[int, tuple[tuple[str, Any], ...]], actual[1])
        assert value == 1
        assert events == (
            ("read", 0),
            ("read-return", decisions[index]),
            ("readinto", 1),
            ("readinto-return", 1),
            ("readinto", 4),
            ("readinto-return", 4),
        )
    assert comparisons[2][0] == (
        "raise",
        ("ExpectedCallbackError", "marshal callback failed"),
    )
    assert comparisons[2][0] == comparisons[2][1]


def _load_readinto(callback: Callable[[], Any]) -> tuple[Any, tuple[tuple[str, Any], ...]]:
    encoded = marshal.dumps(1)
    events: list[tuple[str, Any]] = []
    offset = 0
    readinto_calls = 0

    class Reader:
        def read(self, size: int) -> bytes:
            assert size == 0
            events.append(("read", size))
            events.append(("read-return", b""))
            return b""

        def readinto(self, buffer: Any) -> int:
            nonlocal offset, readinto_calls
            readinto_calls += 1
            size = len(buffer)
            events.append(("readinto", size))
            chunk = encoded[offset : offset + size]
            buffer[: len(chunk)] = chunk
            offset += len(chunk)
            if readinto_calls == 2:
                result = callback()
                offset = 1
                readinto_calls = 2
                events[:] = [
                    ("read", 0),
                    ("read-return", b""),
                    ("readinto", 1),
                    ("readinto-return", 1),
                    ("readinto", size),
                ]
            else:
                result = size
            events.append(("readinto-return", result))
            return result

    return marshal.load(cast(Any, Reader())), tuple(events)


def _load_after_indexed_readinto(
    callback: Callable[[], Any],
) -> tuple[Any, tuple[tuple[str, Any], ...]]:
    encoded = marshal.dumps(0x01020304)
    events: list[tuple[str, Any]] = []
    offset = 0
    readinto_calls = 0

    class Count:
        def __init__(self, value: int) -> None:
            self.value = value

        def __index__(self) -> int:
            events.append(("index", self.value))
            return self.value

    class Reader:
        def read(self, size: int) -> bytes:
            assert size == 0
            events.append(("read", size))
            return b""

        def readinto(self, buffer: Any) -> Any:
            nonlocal offset, readinto_calls
            readinto_calls += 1
            size = len(buffer)
            events.append(("readinto", size))
            chunk = encoded[offset : offset + size]
            buffer[: len(chunk)] = chunk
            offset += len(chunk)
            if readinto_calls == 1:
                result: Any = Count(size)
            else:
                result = callback()
                offset = 1
                readinto_calls = 2
                events[:] = [
                    ("read", 0),
                    ("readinto", 1),
                    ("index", 1),
                    ("readinto", size),
                ]
            events.append(("readinto-return", result))
            return result

    return marshal.load(cast(Any, Reader())), tuple(events)


@_case("load_after_indexed_readinto_multishot")
def _load_after_indexed_readinto_multishot() -> None:
    decisions = (4, 0, "raise", 4)
    comparisons = _resume_against_fresh(
        lambda choose: _load_after_indexed_readinto(
            lambda: _callback(choose()),
        ),
        lambda decision: _load_after_indexed_readinto(
            lambda: _callback(decision),
        ),
        decisions,
    )

    assert comparisons[0][0][0] == comparisons[3][0][0] == "return"
    assert comparisons[0][0] == comparisons[3][0]
    assert comparisons[1][0][0] == "raise"
    assert comparisons[1][0][1][0] == "EOFError"
    assert comparisons[2][0] == (
        "raise",
        ("ExpectedCallbackError", "marshal callback failed"),
    )


def _load_readinto_index_callback(callback: Callable[[], Any]) -> int:
    encoded = marshal.dumps(0x01020304)
    offset = 0
    readinto_calls = 0

    class Count:
        def __index__(self) -> int:
            return cast(int, _callback(callback()))

    class Reader:
        def read(self, size: int) -> bytes:
            assert size == 0
            return b""

        def readinto(self, buffer: Any) -> Any:
            nonlocal offset, readinto_calls
            readinto_calls += 1
            size = len(buffer)
            chunk = encoded[offset : offset + size]
            buffer[: len(chunk)] = chunk
            offset += len(chunk)
            if readinto_calls == 2:
                return Count()
            return size

    return cast(int, marshal.load(cast(Any, Reader())))


@_case("load_readinto_return_index_callback_multishot")
def _load_readinto_return_index_callback_multishot() -> None:
    decisions = (4, 0, 5, "raise", 4)
    comparisons = _resume_against_fresh(
        lambda choose: _load_readinto_index_callback(choose),
        lambda decision: _load_readinto_index_callback(lambda: decision),
        decisions,
    )

    assert comparisons[0][0] == comparisons[4][0] == ("return", 0x01020304)
    assert comparisons[1][0] == (
        "raise",
        ("EOFError", "EOF read where not expected"),
    )
    assert comparisons[2][0] == (
        "raise",
        ("ValueError", "read() returned too much data: 4 bytes requested, 5 returned"),
    )
    assert comparisons[3][0] == (
        "raise",
        ("ExpectedCallbackError", "marshal callback failed"),
    )


def _load_with_post_effect_write(callback: Callable[[], bytes]) -> int:
    encoded = marshal.dumps(0x01020304)
    offset = 0
    readinto_calls = 0

    class Reader:
        def read(self, size: int) -> bytes:
            assert size == 0
            return b""

        def readinto(self, buffer: Any) -> int:
            nonlocal offset, readinto_calls
            readinto_calls += 1
            size = len(buffer)
            if readinto_calls == 1:
                chunk = encoded[offset : offset + size]
            else:
                buffer[:] = b"\x00" * size
                chunk = callback()
                offset = 1
                readinto_calls = 2
            buffer[: len(chunk)] = chunk
            offset += size
            return size

    return cast(int, marshal.load(cast(Any, Reader())))


@_case("load_readinto_buffer_isolation_multishot")
def _load_readinto_buffer_isolation_multishot() -> None:
    encoded_tail = marshal.dumps(0x01020304)[1:]
    decisions = (b"\x05", encoded_tail, b"\x05")
    comparisons = _resume_against_fresh(
        lambda choose: _load_with_post_effect_write(
            lambda: cast(bytes, choose()),
        ),
        lambda decision: _load_with_post_effect_write(lambda: decision),
        decisions,
    )

    assert [actual for actual, _expected in comparisons] == [
        ("return", 5),
        ("return", 0x01020304),
        ("return", 5),
    ]


@_case("load_readinto_multishot")
def _load_readinto_multishot() -> None:
    decisions = (4, 0, 5, "raise", 4)
    comparisons = _resume_against_fresh(
        lambda choose: _load_readinto(lambda: _callback(choose())),
        lambda decision: _load_readinto(lambda: _callback(decision)),
        decisions,
    )

    assert [actual[0] for actual, _expected in comparisons] == [
        "return",
        "raise",
        "raise",
        "raise",
        "return",
    ]
    expected_prefix = (
        ("read", 0),
        ("read-return", b""),
        ("readinto", 1),
        ("readinto-return", 1),
        ("readinto", 4),
    )
    for index in (0, 4):
        actual = comparisons[index][0]
        value, events = cast(tuple[int, tuple[tuple[str, Any], ...]], actual[1])
        assert value == 1
        assert events == expected_prefix + (("readinto-return", 4),)

    assert comparisons[1][0] == (
        "raise",
        ("EOFError", "EOF read where not expected"),
    )
    assert comparisons[1][0][1] == comparisons[1][1][1]
    assert comparisons[2][0] == (
        "raise",
        ("ValueError", "read() returned too much data: 4 bytes requested, 5 returned"),
    )
    assert comparisons[2][0][1] == comparisons[2][1][1]
    assert comparisons[3][0] == (
        "raise",
        ("ExpectedCallbackError", "marshal callback failed"),
    )
    assert comparisons[3][0] == comparisons[3][1]


def _load_code_with_allow_code(
    callback: Callable[[], Any],
) -> tuple[str, tuple[tuple[str, Any], ...]]:
    encoded = marshal.dumps(_code_payload())
    events: list[tuple[str, Any]] = []
    offset = 0

    class AllowCode:
        def __bool__(self) -> bool:
            nonlocal offset
            events.append(("allow-code", None))
            result = callback()
            offset = 0
            events[:] = [("allow-code", None)]
            events.append(("allow-code-return", result))
            return result

    class Reader:
        def read(self, size: int) -> bytes:
            assert size == 0
            events.append(("read", size))
            events.append(("read-return", b""))
            return b""

        def readinto(self, buffer: Any) -> int:
            nonlocal offset
            size = len(buffer)
            events.append(("readinto", size))
            chunk = encoded[offset : offset + size]
            buffer[: len(chunk)] = chunk
            offset += len(chunk)
            events.append(("readinto-return", len(chunk)))
            return len(chunk)

    result = cast(Any, marshal.load)(Reader(), allow_code=AllowCode())
    return result.co_filename, tuple(events)


@_case("dump_write_multishot")
def _dump_write_multishot() -> None:
    decisions = (None, 0, "sentinel", "raise", "sentinel")
    comparisons = _resume_against_fresh(
        lambda choose: _dump_write(lambda: _callback(choose())),
        lambda decision: _dump_write(lambda: _callback(decision)),
        decisions,
    )
    expected_data = marshal.dumps((1, "two", [3]))
    for index, decision in enumerate(decisions):
        actual = comparisons[index][0]
        if decision == "raise":
            assert actual == (
                "raise",
                ("ExpectedCallbackError", "marshal callback failed"),
            )
        else:
            assert actual == ("return", (decision, (expected_data,)))


def _version_conversion(operation: str, callback: Callable[[], Any]) -> Any:
    class Version:
        def __index__(self) -> int:
            return cast(int, callback())

    if operation == "dumps":
        return marshal.dumps((1, 2), cast(Any, Version()))
    if operation == "dump":

        class Writer:
            def __init__(self) -> None:
                self.data: bytes | None = None

            def write(self, data: bytes) -> int:
                self.data = data
                return len(data)

        writer = Writer()
        result = marshal.dump((1, 2), writer, cast(Any, Version()))
        return result, writer.data
    raise AssertionError(operation)


for _operation in ("dump", "dumps"):

    @_case(f"{_operation}_version_index_multishot")
    def _version_index_multishot(operation: str = _operation) -> None:
        decisions = (4, 3, "not-an-index", "raise", 4)
        comparisons = _resume_against_fresh(
            lambda choose: _version_conversion(
                operation,
                lambda: _callback(choose()),
            ),
            lambda decision: _version_conversion(
                operation,
                lambda: _callback(decision),
            ),
            decisions,
        )
        assert comparisons[0][0] == comparisons[4][0]
        assert comparisons[0][0][0] == comparisons[1][0][0] == "return"
        assert comparisons[2][0][0] == "raise"
        assert comparisons[2][0][1][0] == "TypeError"
        assert comparisons[3][0] == (
            "raise",
            ("ExpectedCallbackError", "marshal callback failed"),
        )


def _code_payload() -> Any:
    return compile("value = 1", "<marshal-test>", "exec")


def _allow_code(operation: str, callback: Callable[[], Any]) -> Any:
    class AllowCode:
        def __bool__(self) -> bool:
            return cast(bool, callback())

    allow = AllowCode()
    code = _code_payload()
    encoded = marshal.dumps(code)
    if operation == "dumps":
        return cast(Any, marshal.dumps)(code, allow_code=allow)
    if operation == "loads":
        return cast(Any, marshal.loads)(encoded, allow_code=allow).co_filename
    if operation == "dump":

        class Writer:
            def __init__(self) -> None:
                self.data = b""

            def write(self, data: bytes) -> int:
                self.data = data
                return len(data)

        writer = Writer()
        result = cast(Any, marshal.dump)(code, writer, allow_code=allow)
        return result, marshal.loads(writer.data).co_filename
    raise AssertionError(operation)


if sys.version_info >= (3, 13):

    @_case("load_allow_code_multishot")
    def _load_allow_code_multishot() -> None:
        decisions = (True, False, "not-a-bool", "raise", True)
        comparisons = _resume_against_fresh(
            lambda choose: _load_code_with_allow_code(
                lambda: _callback(choose()),
            ),
            lambda decision: _load_code_with_allow_code(
                lambda: _callback(decision),
            ),
            decisions,
        )

        assert [actual[0] for actual, _expected in comparisons] == [
            "return",
            "raise",
            "raise",
            "raise",
            "return",
        ]
        for index in (0, 4):
            actual = comparisons[index][0]
            filename, events = cast(tuple[str, tuple[tuple[str, Any], ...]], actual[1])
            assert filename == "<marshal-test>"
            assert events[:4] == (
                ("allow-code", None),
                ("allow-code-return", decisions[index]),
                ("read", 0),
                ("read-return", b""),
            )
        assert comparisons[1][0] == (
            "raise",
            ("ValueError", "unmarshalling code objects is disallowed"),
        )
        assert comparisons[1][0][1] == comparisons[1][1][1]
        assert comparisons[2][0][0] == "raise"
        assert comparisons[2][0][1][0] == "TypeError"
        assert comparisons[3][0] == (
            "raise",
            ("ExpectedCallbackError", "marshal callback failed"),
        )
        assert comparisons[3][0] == comparisons[3][1]

    for _operation in ("dump", "dumps", "loads"):
        _case(f"{_operation}_allow_code_multishot")(
            lambda operation=_operation: _resume_against_fresh(
                lambda choose: _allow_code(
                    operation,
                    lambda: _callback(choose()),
                ),
                lambda decision: _allow_code(
                    operation,
                    lambda: _callback(decision),
                ),
                (True, False, "not-a-bool", "raise", True),
            )
        )


@_case("dump_write_async_multishot")
def _dump_write_async_multishot() -> None:
    async def exercise() -> list[Outcome]:
        choose = effect("marshal-async-write")
        handler = create_async_handler(choose)

        async def run() -> tuple[Any, tuple[bytes, ...]]:
            return _dump_write(lambda: _callback(choose()))

        @handler.on(choose)
        async def resume(k: Any) -> list[Outcome]:
            outcomes: list[Outcome] = []
            for decision in (None, "sentinel", "raise", "sentinel"):
                try:
                    value = await k(decision)
                except Exception as exc:
                    actual: Outcome = ("raise", (type(exc).__name__, str(exc)))
                else:
                    actual = ("return", value)
                expected = _outcome(lambda decision=decision: _dump_write(lambda: _callback(decision)))
                assert actual == expected
                outcomes.append(actual)
            return outcomes

        return cast(list[Outcome], await handler(run))

    outcomes = asyncio.run(exercise())
    expected_data = marshal.dumps((1, "two", [3]))
    assert outcomes[0] == ("return", (None, (expected_data,)))
    assert outcomes[1] == ("return", ("sentinel", (expected_data,)))
    assert outcomes[1] == outcomes[3]
    assert outcomes[2] == (
        "raise",
        ("ExpectedCallbackError", "marshal callback failed"),
    )


@_case("load_readinto_async_multishot")
def _load_readinto_async_multishot() -> None:
    async def exercise() -> list[Outcome]:
        choose = effect("marshal-async-readinto")
        handler = create_async_handler(choose)

        async def run() -> tuple[Any, tuple[tuple[str, Any], ...]]:
            return _load_readinto(lambda: _callback(choose()))

        @handler.on(choose)
        async def resume(k: Any) -> list[Outcome]:
            outcomes: list[Outcome] = []
            for decision in (4, 0, "raise", 4):
                try:
                    value = await k(decision)
                except Exception as exc:
                    actual: Outcome = ("raise", (type(exc).__name__, str(exc)))
                else:
                    actual = ("return", value)
                expected = _outcome(
                    lambda decision=decision: _load_readinto(
                        lambda: _callback(decision),
                    )
                )
                assert actual == expected
                outcomes.append(actual)
            return outcomes

        return cast(list[Outcome], await handler(run))

    outcomes = asyncio.run(exercise())
    assert outcomes[0][0] == outcomes[3][0] == "return"
    assert outcomes[0] == outcomes[3]
    assert outcomes[1] == (
        "raise",
        ("EOFError", "EOF read where not expected"),
    )
    assert outcomes[2] == (
        "raise",
        ("ExpectedCallbackError", "marshal callback failed"),
    )


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=15,
        check=False,
    )


@pytest.mark.parametrize("case_name", list(_CASES))
def test_marshal_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_marshal.py --case CASE")
    _CASES[sys.argv[2]]()
