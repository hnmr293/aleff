"""Strict continuation tests for callable regular-expression substitutions."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, cast

import pytest

from aleff import create_async_handler, create_handler, effect


Choose = Callable[[], Any]
Case = Callable[[], None]
Outcome = tuple[str, Any]
_CASES: dict[str, Case] = {}
_OPERATIONS = ("sub", "subn", "pattern_sub", "pattern_subn")


class ExpectedCallbackError(Exception):
    """An exception used to verify callback-exception restoration."""


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


def _replacement(value: Any) -> Any:
    if value == "raise":
        raise ExpectedCallbackError("replacement failed")
    return value


def _resume_against_fresh(
    run: Callable[[Choose], Any],
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
) -> None:
    choose = effect("re-replacement-choice")
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
    assert suspension_count == 1, "the scenario must suspend at exactly one callback"
    assert all(actual == expected for actual, expected in comparisons), comparisons


def _substitution(
    operation: str,
    source: str | bytes,
    pattern_text: str | bytes,
    callback_index: int,
    callback: Callable[[], Any],
    *,
    count: int = 0,
) -> Any:
    pattern = re.compile(pattern_text)
    calls = 0
    fixed = b"<fixed>" if isinstance(source, bytes) else "<fixed>"

    def replace(_match: re.Match[Any]) -> Any:
        nonlocal calls
        calls += 1
        if calls == callback_index:
            return callback()
        return fixed

    if operation == "sub":
        return cast(
            Any,
            re.sub(cast(Any, pattern), cast(Any, replace), cast(Any, source), count=count),
        )
    if operation == "subn":
        return cast(
            Any,
            re.subn(cast(Any, pattern), cast(Any, replace), cast(Any, source), count=count),
        )
    if operation == "pattern_sub":
        return cast(Any, pattern).sub(replace, source, count=count)
    if operation == "pattern_subn":
        return cast(Any, pattern).subn(replace, source, count=count)
    raise AssertionError(operation)


def _substitution_case(
    operation: str,
    source: str | bytes,
    pattern: str | bytes,
    callback_index: int,
    decisions: tuple[Any, ...],
    *,
    count: int = 0,
) -> None:
    _resume_against_fresh(
        lambda choose: _substitution(
            operation,
            source,
            pattern,
            callback_index,
            lambda: _replacement(choose()),
            count=count,
        ),
        lambda decision: _substitution(
            operation,
            source,
            pattern,
            callback_index,
            lambda: _replacement(decision),
            count=count,
        ),
        decisions,
    )


for _operation in _OPERATIONS:
    _case(f"{_operation}_str_first_callback_multishot")(
        lambda operation=_operation: _substitution_case(
            operation, "a-a-a", "a", 1, ("<one>", "<two>", "raise", "<one>")
        )
    )
    _case(f"{_operation}_str_middle_callback_multishot")(
        lambda operation=_operation: _substitution_case(
            operation, "a-a-a", "a", 2, ("<one>", "", "raise", "<one>")
        )
    )
    _case(f"{_operation}_str_final_callback_multishot")(
        lambda operation=_operation: _substitution_case(
            operation, "a-a-a", "a", 3, ("<one>", "<two>", "raise", "<one>")
        )
    )
    _case(f"{_operation}_bytes_callback_multishot")(
        lambda operation=_operation: _substitution_case(
            operation,
            b"a-a-a",
            b"a",
            2,
            (b"<one>", b"", "raise", b"<one>"),
        )
    )
    _case(f"{_operation}_zero_width_callback_multishot")(
        lambda operation=_operation: _substitution_case(
            operation, "ab", "", 2, ("X", "Y", "raise", "X")
        )
    )
    _case(f"{_operation}_empty_then_nonempty_same_position_multishot")(
        lambda operation=_operation: _substitution_case(
            operation, "x", "|x", 2, ("X", "Y", "raise", "X")
        )
    )
    _case(f"{_operation}_count_limited_callback_multishot")(
        lambda operation=_operation: _substitution_case(
            operation, "a-a-a", "a", 1, ("X", "Y", "raise", "X"), count=2
        )
    )


@_case("callable_return_validation_multishot")
def _callable_return_validation_multishot() -> None:
    for operation in _OPERATIONS:
        _substitution_case(operation, "a-a", "a", 1, ("X", None, 42, "X"))
        _substitution_case(
            operation,
            b"a-a",
            b"a",
            1,
            (b"X", None, "wrong type", b"X"),
        )


@_case("mutable_bytes_input_multishot")
def _mutable_bytes_input_multishot() -> None:
    def execute(callback: Callable[[], bytes]) -> tuple[bytes, bytes]:
        source = bytearray(b"a-a")
        calls = 0

        def replace(_match: re.Match[bytes]) -> bytes:
            nonlocal calls
            calls += 1
            if calls == 1:
                replacement = callback()
                source[2] = ord("b")
                return replacement
            return b"<fixed>"

        return re.compile(b"a").sub(replace, source), bytes(source)

    _resume_against_fresh(
        lambda choose: execute(lambda: cast(bytes, _replacement(choose()))),
        lambda decision: execute(lambda: cast(bytes, _replacement(decision))),
        (b"X", b"Y", "raise", b"X"),
    )


@_case("nested_substitution_multishot")
def _nested_substitution_multishot() -> None:
    def execute(callback: Callable[[], Any]) -> str:
        calls = 0

        def outer_replace(_match: re.Match[str]) -> str:
            def inner_replace(_inner: re.Match[str]) -> str:
                nonlocal calls
                calls += 1
                return cast(str, callback()) if calls == 1 else "<inner>"

            return re.sub("b", inner_replace, "b-b")

        return re.sub("a", outer_replace, "a-a")

    _resume_against_fresh(
        lambda choose: execute(lambda: _replacement(choose())),
        lambda decision: execute(lambda: _replacement(decision)),
        ("X", "Y", "raise", "X"),
    )


@_case("pattern_sub_async_multishot")
def _pattern_sub_async_multishot() -> None:
    async def exercise() -> list[Outcome]:
        choose = effect("re-async-choice")
        handler = create_async_handler(choose)

        async def run() -> str:
            return cast(
                str,
                _substitution(
                    "pattern_sub",
                    "a-a",
                    "a",
                    1,
                    lambda: _replacement(choose()),
                ),
            )

        @handler.on(choose)
        async def resume(k: Any) -> list[Outcome]:
            outcomes: list[Outcome] = []
            for decision in ("X", "Y", "raise", "X"):
                actual = _outcome(lambda decision=decision: k(decision))
                if actual[0] == "return" and hasattr(actual[1], "__await__"):
                    try:
                        value = await actual[1]
                    except Exception as exc:
                        actual = ("raise", (type(exc).__name__, str(exc)))
                    else:
                        actual = ("return", value)
                expected = _outcome(
                    lambda decision=decision: _substitution(
                        "pattern_sub",
                        "a-a",
                        "a",
                        1,
                        lambda: _replacement(decision),
                    )
                )
                assert actual == expected
                outcomes.append(actual)
            return outcomes

        return cast(list[Outcome], await handler(run))

    outcomes = asyncio.run(exercise())
    assert outcomes[0] == outcomes[3]
    assert outcomes[0] != outcomes[1]
    assert outcomes[2][0] == "raise"


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=15,
        check=False,
    )


@pytest.mark.parametrize("case_name", list(_CASES))
def test_re_callable_substitution_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_re.py --case CASE")
    _CASES[sys.argv[2]]()
