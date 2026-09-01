"""Strict multi-shot continuation tests for JSON C-accelerator callbacks."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import io
import json
from json import encoder, scanner
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

assert cast(Any, encoder).c_make_encoder is not None
assert cast(Any, scanner).c_make_scanner is not None


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
    except BaseException as exc:
        return "raise", (type(exc).__name__, str(exc))


def _callback_result(value: Any, callback_name: str) -> Any:
    if value == "raise":
        raise ExpectedCallbackError(f"{callback_name} callback failed")
    return value


def _resume_against_fresh(
    run: Callable[[Choose], Any],
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
) -> list[Outcome]:
    """Compare every continuation shot with a fresh ordinary execution."""

    choose = effect("json-choice")
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
            assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            outcomes.append(actual)
        return outcomes

    result = handler(lambda: run(choose))
    assert suspension_count == 1, "the scenario must suspend at exactly one JSON callback"
    return cast(list[Outcome], result)


def _assert_normal_error_corner(outcomes: list[Outcome], callback_name: str) -> None:
    """Require normal, boundary, callback-error, and repeated-shot outcomes."""

    assert outcomes[0][0] == "return"
    assert outcomes[1][0] == "return"
    assert outcomes[2] == (
        "raise",
        ("ExpectedCallbackError", f"{callback_name} callback failed"),
    )
    assert outcomes[0] == outcomes[3]


class _Unsupported:
    pass


def _encode(operation: str, callback: Callable[[Any], Any]) -> Any:
    value = _Unsupported()
    if operation == "dump":
        stream = io.StringIO()
        result = json.dump(value, stream, default=callback)
        return result, stream.getvalue()
    if operation == "dumps":
        return json.dumps(value, default=callback)
    raise AssertionError(operation)


def _decode(
    operation: str,
    source: str,
    *,
    object_hook: Callable[[dict[str, Any]], Any] | None = None,
    object_pairs_hook: Callable[[list[tuple[str, Any]]], Any] | None = None,
    parse_float: Callable[[str], Any] | None = None,
    parse_int: Callable[[str], Any] | None = None,
    parse_constant: Callable[[str], Any] | None = None,
) -> Any:
    kwargs: dict[str, Any] = {
        "object_hook": object_hook,
        "object_pairs_hook": object_pairs_hook,
        "parse_float": parse_float,
        "parse_int": parse_int,
        "parse_constant": parse_constant,
    }
    if operation == "loads":
        return json.loads(source, **kwargs)
    if operation == "load":
        return json.load(io.StringIO(source), **kwargs)
    raise AssertionError(operation)


def _default_case(operation: str) -> None:
    def run(choose: Choose) -> Any:
        def default(value: Any) -> Any:
            assert isinstance(value, _Unsupported)
            return _callback_result(choose(), "default")

        return _encode(operation, default)

    def fresh(decision: Any) -> Any:
        def default(_value: Any) -> Any:
            assert isinstance(_value, _Unsupported)
            return _callback_result(decision, "default")

        return _encode(operation, default)

    outcomes = _resume_against_fresh(run, fresh, ({"encoded": 1}, None, "raise", {"encoded": 1}))
    _assert_normal_error_corner(outcomes, "default")


for _operation in ("dump", "dumps"):
    _case(f"default_{_operation}_multishot")(lambda operation=_operation: _default_case(operation))


def _object_hook_case(operation: str) -> None:
    source = '{"value": 1}'

    def run(choose: Choose) -> Any:
        def hook(obj: dict[str, Any]) -> Any:
            assert obj == {"value": 1}
            return {"hook": _callback_result(choose(), "object_hook"), "value": obj["value"]}

        return _decode(
            operation,
            source,
            object_hook=hook,
        )

    def fresh(decision: Any) -> Any:
        def hook(obj: dict[str, Any]) -> Any:
            assert obj == {"value": 1}
            return {"hook": _callback_result(decision, "object_hook"), "value": obj["value"]}

        return _decode(
            operation,
            source,
            object_hook=hook,
        )

    outcomes = _resume_against_fresh(run, fresh, ("normal", None, "raise", "normal"))
    _assert_normal_error_corner(outcomes, "object_hook")


def _object_pairs_hook_case(operation: str) -> None:
    source = '{"value": 1}'

    def run(choose: Choose) -> Any:
        def hook(pairs: list[tuple[str, Any]]) -> Any:
            assert pairs == [("value", 1)]
            return {"hook": _callback_result(choose(), "object_pairs_hook"), "pairs": pairs}

        return _decode(
            operation,
            source,
            object_pairs_hook=hook,
        )

    def fresh(decision: Any) -> Any:
        def hook(pairs: list[tuple[str, Any]]) -> Any:
            assert pairs == [("value", 1)]
            return {"hook": _callback_result(decision, "object_pairs_hook"), "pairs": pairs}

        return _decode(
            operation,
            source,
            object_pairs_hook=hook,
        )

    outcomes = _resume_against_fresh(run, fresh, ("normal", None, "raise", "normal"))
    _assert_normal_error_corner(outcomes, "object_pairs_hook")


def _parse_callback_case(operation: str, callback_name: str, source: str) -> None:
    expected_token = {
        "parse_float": "1.25",
        "parse_int": "123456789",
        "parse_constant": "NaN",
    }[callback_name]

    def run(choose: Choose) -> Any:
        def callback(value: str) -> Any:
            assert value == expected_token
            return _callback_result(choose(), callback_name)

        kwargs: dict[str, Any] = {callback_name: callback}
        return _decode(operation, source, **kwargs)

    def fresh(decision: Any) -> Any:
        def callback(value: str) -> Any:
            assert value == expected_token
            return _callback_result(decision, callback_name)

        kwargs: dict[str, Any] = {callback_name: callback}
        return _decode(operation, source, **kwargs)

    outcomes = _resume_against_fresh(run, fresh, ("normal", None, "raise", "normal"))
    _assert_normal_error_corner(outcomes, callback_name)


for _operation in ("load", "loads"):
    _case(f"object_hook_{_operation}_multishot")(lambda operation=_operation: _object_hook_case(operation))
    _case(f"object_pairs_hook_{_operation}_multishot")(lambda operation=_operation: _object_pairs_hook_case(operation))
    _case(f"parse_float_{_operation}_multishot")(
        lambda operation=_operation: _parse_callback_case(operation, "parse_float", "[1.25]")
    )
    _case(f"parse_int_{_operation}_multishot")(
        lambda operation=_operation: _parse_callback_case(operation, "parse_int", "[123456789]")
    )
    _case(f"parse_constant_{_operation}_multishot")(
        lambda operation=_operation: _parse_callback_case(operation, "parse_constant", "[NaN]")
    )


def _nested_default_case(operation: str) -> None:
    values = [
        _Unsupported(),
        {"nested": [_Unsupported(), _Unsupported()]},
    ]

    def execute(callback: Callable[[], Any]) -> Any:
        calls = 0

        def default(_value: Any) -> Any:
            nonlocal calls
            calls += 1
            if calls == 2:
                result = _callback_result(callback(), "default")
                calls = 2
                return result
            return {"fixed": calls}

        if operation == "dump":
            stream = io.StringIO()
            result = json.dump(values, stream, default=default, sort_keys=True)
            return result, stream.getvalue(), calls
        return json.dumps(values, default=default, sort_keys=True), calls

    outcomes = _resume_against_fresh(
        lambda choose: execute(choose),
        lambda decision: execute(lambda: decision),
        ({"chosen": 1}, None, "raise", {"chosen": 1}),
    )
    _assert_normal_error_corner(outcomes, "default")


for _operation in ("dump", "dumps"):
    _case(f"default_{_operation}_nested_middle_callback_multishot")(
        lambda operation=_operation: _nested_default_case(operation)
    )


def _nested_decode_callback_case(operation: str, callback_name: str) -> None:
    sources = {
        "object_hook": '{"outer": {"middle": {"leaf": 1}}}',
        "object_pairs_hook": '{"outer": {"middle": {"leaf": 1}}}',
        "parse_float": '[1.25, {"nested": [2.5, 3.75]}]',
        "parse_int": '[1, {"nested": [2, 3]}]',
        "parse_constant": '[NaN, {"nested": [Infinity, -Infinity]}]',
    }
    source = sources[callback_name]

    def execute(callback: Callable[[], Any]) -> Any:
        calls = 0

        def hook(value: Any) -> Any:
            nonlocal calls
            calls += 1
            if calls == 2:
                result = _callback_result(callback(), callback_name)
                calls = 2
                return result
            return value

        kwargs: dict[str, Any] = {callback_name: hook}
        return _decode(operation, source, **kwargs), calls

    outcomes = _resume_against_fresh(
        lambda choose: execute(choose),
        lambda decision: execute(lambda: decision),
        ("nested-a", None, "raise", "nested-a"),
    )
    _assert_normal_error_corner(outcomes, callback_name)


for _operation in ("load", "loads"):
    for _callback_name in (
        "object_hook",
        "object_pairs_hook",
        "parse_float",
        "parse_int",
        "parse_constant",
    ):
        _case(f"{_callback_name}_{_operation}_nested_middle_callback_multishot")(
            lambda operation=_operation, callback_name=_callback_name: _nested_decode_callback_case(
                operation,
                callback_name,
            )
        )


@_case("object_pairs_hook_precedes_object_hook_multishot")
def _object_pairs_hook_precedes_object_hook_multishot() -> None:
    source = '{"value": 1}'

    def run(choose: Choose) -> Any:
        def unexpected_object_hook(_obj: dict[str, Any]) -> Any:
            raise AssertionError("object_hook must not run when object_pairs_hook is set")

        return _decode(
            "loads",
            source,
            object_hook=unexpected_object_hook,
            object_pairs_hook=lambda pairs: _callback_result(choose(), "object_pairs_hook"),
        )

    def fresh(decision: Any) -> Any:
        def unexpected_object_hook(_obj: dict[str, Any]) -> Any:
            raise AssertionError("object_hook ran")

        return _decode(
            "loads",
            source,
            object_hook=unexpected_object_hook,
            object_pairs_hook=lambda pairs: _callback_result(decision, "object_pairs_hook"),
        )

    outcomes = _resume_against_fresh(run, fresh, ([], None, "raise", []))
    _assert_normal_error_corner(outcomes, "object_pairs_hook")


async def _async_resume_against_fresh(
    run: Callable[[Choose], Any],
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
) -> list[Outcome]:
    choose = effect("json-async-choice")
    handler = create_async_handler(choose)
    suspension_count = 0

    async def run_outcome(call: Callable[[], Any]) -> Outcome:
        try:
            result = call()
            if hasattr(result, "__await__"):
                result = await result
            return "return", result
        except BaseException as exc:
            return "raise", (type(exc).__name__, str(exc))

    @handler.on(choose)
    async def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        outcomes: list[Outcome] = []
        for decision in decisions:
            expected = _outcome(lambda decision=decision: fresh(decision))
            actual = await run_outcome(lambda decision=decision: k(decision))
            assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            outcomes.append(actual)
        return outcomes

    async def caller() -> Any:
        return await run(choose)

    result = await handler(caller)
    assert suspension_count == 1, "the async scenario must suspend at exactly one JSON callback"
    return cast(list[Outcome], result)


@_case("default_dumps_async_multishot")
def _default_dumps_async_multishot() -> None:
    async def exercise() -> list[Outcome]:
        async def run(choose: Choose) -> Any:
            return _encode("dumps", lambda _value: _callback_result(choose(), "default"))

        def fresh(decision: Any) -> Any:
            return _encode("dumps", lambda _value: _callback_result(decision, "default"))

        return await _async_resume_against_fresh(run, fresh, ({"encoded": 1}, None, "raise", {"encoded": 1}))

    outcomes = asyncio.run(exercise())
    _assert_normal_error_corner(outcomes, "default")


@_case("parse_float_loads_async_multishot")
def _parse_float_loads_async_multishot() -> None:
    async def exercise() -> list[Outcome]:
        async def run(choose: Choose) -> Any:
            def callback(value: str) -> Any:
                assert value == "1.25"
                return _callback_result(choose(), "parse_float")

            return _decode("loads", "[1.25]", parse_float=callback)

        def fresh(decision: Any) -> Any:
            def callback(value: str) -> Any:
                assert value == "1.25"
                return _callback_result(decision, "parse_float")

            return _decode("loads", "[1.25]", parse_float=callback)

        return await _async_resume_against_fresh(run, fresh, ("normal", None, "raise", "normal"))

    outcomes = asyncio.run(exercise())
    _assert_normal_error_corner(outcomes, "parse_float")


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=20,
        check=False,
    )


@pytest.mark.parametrize("case_name", list(_CASES))
def test_json_callback_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_json.py --case CASE")
    _CASES[sys.argv[2]]()
