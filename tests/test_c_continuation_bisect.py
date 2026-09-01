"""Strict continuation tests for the bisect C accelerators."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import bisect
from pathlib import Path
import subprocess
import sys
from typing import Any, cast

import pytest

from aleff import create_async_handler, create_handler, effect


Choose = Callable[[], Any]
Case = Callable[[], None]
Outcome = tuple[str, Any]
_CASES: dict[str, Case] = {}

_OPERATIONS = (
    "bisect_left",
    "bisect_right",
    "bisect",
    "insort_left",
    "insort_right",
    "insort",
)


class ExpectedCallbackError(Exception):
    """An exception used to verify that callback exceptions resume cleanly."""


def _case(name: str) -> Callable[[Case], Case]:
    def register(test_case: Case) -> Case:
        _CASES[name] = test_case
        return test_case

    return register


def _is_left(operation: str) -> bool:
    return operation.endswith("_left")


def _is_insert(operation: str) -> bool:
    return operation.startswith("insort")


def _names(values: list[Any]) -> tuple[str, ...]:
    return tuple(value.name for value in values)


def _outcome(call: Callable[[], Any]) -> Outcome:
    try:
        return "return", call()
    except Exception as exc:
        return "raise", type(exc).__name__


def _callback(decision: Any) -> Any:
    if decision == "raise":
        raise ExpectedCallbackError("callback failed")
    return decision


def _resume_against_fresh(
    run: Callable[[Choose], Any],
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
) -> None:
    """Compare every resume with a new ordinary execution of the same case."""

    choose = effect("bisect-choice")
    suspension_count = 0
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[Outcome, Outcome]]:
        nonlocal suspension_count
        suspension_count += 1
        comparisons: list[tuple[Outcome, Outcome]] = []
        for decision in decisions:
            actual = _outcome(lambda decision=decision: k(decision))
            expected = _outcome(lambda decision=decision: fresh(decision))
            comparisons.append((actual, expected))
        return comparisons

    comparisons = cast(list[tuple[Outcome, Outcome]], handler(lambda: run(choose)))
    assert suspension_count == 1, "the scenario must suspend at exactly one callback"
    assert all(actual == expected for actual, expected in comparisons), comparisons


def _comparison_execution(operation: str, callback: Callable[[], Any]) -> tuple[Any, tuple[str, ...]]:
    effect_side = "entry" if _is_left(operation) else "probe"

    class Entry:
        name = "entry"

        def __lt__(self, _other: object) -> bool:
            if effect_side == "entry":
                return cast(bool, callback())
            return False

    class Probe:
        name = "probe"

        def __lt__(self, _other: object) -> bool:
            if effect_side == "probe":
                return cast(bool, callback())
            return False

    target: list[Any] = [Entry()]
    result = getattr(bisect, operation)(target, Probe())
    return (type(result).__name__, result), _names(target)


def _comparison_case(operation: str, decisions: tuple[Any, ...]) -> None:
    _resume_against_fresh(
        lambda choose: _comparison_execution(operation, lambda: _callback(choose())),
        lambda decision: _comparison_execution(operation, lambda: _callback(decision)),
        decisions,
    )


for _operation in _OPERATIONS:
    _case(f"{_operation}_comparison_multishot")(
        lambda operation=_operation: _comparison_case(operation, (True, False, True))
    )
    _case(f"{_operation}_comparison_exception")(
        lambda operation=_operation: _comparison_case(operation, ("raise", False))
    )


def _key_execution(
    operation: str,
    phase: str,
    callback: Callable[[], Any],
) -> tuple[Any, tuple[str, ...]]:
    entry = type("Entry", (), {"name": "entry"})()
    probe = type("Probe", (), {"name": "probe"})()
    target: list[Any] = [entry]

    def key(value: object) -> int:
        is_effect_value = (phase == "entry" and value is entry) or (phase == "probe" and value is probe)
        if is_effect_value:
            return cast(int, callback())
        return 1

    if _is_insert(operation):
        result = getattr(bisect, operation)(target, probe, key=key)
        return (type(result).__name__, result), _names(target)

    result = getattr(bisect, operation)(target, 1, key=key)
    return (type(result).__name__, result), _names(target)


def _key_case(operation: str, phase: str, decisions: tuple[Any, ...]) -> None:
    _resume_against_fresh(
        lambda choose: _key_execution(operation, phase, lambda: _callback(choose())),
        lambda decision: _key_execution(operation, phase, lambda: _callback(decision)),
        decisions,
    )


for _operation in _OPERATIONS:
    _case(f"{_operation}_key_entry_multishot")(lambda operation=_operation: _key_case(operation, "entry", (0, 2, 0)))
    _case(f"{_operation}_key_exception")(lambda operation=_operation: _key_case(operation, "entry", ("raise", 0)))

for _operation in ("insort_left", "insort_right", "insort"):
    _case(f"{_operation}_key_probe_multishot")(lambda operation=_operation: _key_case(operation, "probe", (0, 2, 0)))
    _case(f"{_operation}_key_probe_exception")(lambda operation=_operation: _key_case(operation, "probe", ("raise", 0)))


def _insort_callback_mutates_receiver(
    operation: str,
    choose: Callable[[], tuple[bool, str]],
) -> tuple[str, Any, tuple[str, ...]]:
    triggered = False

    class Item:
        def __init__(self, name: str, value: int) -> None:
            self.name = name
            self.value = value

        def __lt__(self, other: Any) -> bool:
            nonlocal triggered
            if not triggered:
                triggered = True
                decision, label = choose()
                # This mutation happens immediately after the effect resumes.
                target.append(Item(f"callback-{label}", 99))
                return decision
            return self.value < other.value

    target: list[Item] = [Item("entry", 1)]
    result = getattr(bisect, operation)(target, Item("probe", 2))
    return type(result).__name__, result, _names(target)


@_case("insort_exact_list_comparison_callback_receiver_multishot")
def _insort_exact_list_comparison_callback_receiver_multishot() -> None:
    for operation in ("insort_left", "insort_right", "insort"):
        decisions = ((True, "first"), (False, "second"), (True, "third"))
        choose = effect(f"bisect-{operation}-receiver-choice")
        handler = create_handler(choose)
        suspension_count = 0

        @handler.on(choose)
        def resume(
            k: Any,
        ) -> list[
            tuple[
                tuple[str, Any, tuple[str, ...]],
                tuple[str, Any, tuple[str, ...]],
            ]
        ]:
            nonlocal suspension_count
            suspension_count += 1
            comparisons: list[
                tuple[
                    tuple[str, Any, tuple[str, ...]],
                    tuple[str, Any, tuple[str, ...]],
                ]
            ] = []
            for decision in decisions:
                actual = cast(
                    tuple[str, Any, tuple[str, ...]],
                    k(decision),
                )
                expected = _insort_callback_mutates_receiver(operation, lambda decision=decision: decision)
                comparisons.append((actual, expected))
            return comparisons

        comparisons = cast(
            list[
                tuple[
                    tuple[str, Any, tuple[str, ...]],
                    tuple[str, Any, tuple[str, ...]],
                ]
            ],
            handler(lambda: _insort_callback_mutates_receiver(operation, lambda: choose())),
        )
        assert suspension_count == 1
        assert all(actual == expected for actual, expected in comparisons), comparisons


def _sequence_execution(
    operation: str,
    site: str,
    callback: Callable[[], Any],
) -> tuple[Any, tuple[int, ...]]:
    class Sequence:
        def __init__(self) -> None:
            self.values = [1, 2, 3] if site == "len" else [1]

        def __len__(self) -> int:
            if site == "len":
                return cast(int, callback())
            return 1

        def __getitem__(self, index: int) -> int:
            if site == "getitem":
                return cast(int, callback())
            return self.values[index]

        def insert(self, index: int, value: int) -> None:
            if site == "insert":
                callback()
            # A custom sequence owns its own mutation semantics.  Keep this
            # callback side-effect free so the test covers the surrounding
            # C continuation without requiring generic object-graph rollback.
            del index, value

    target = Sequence()
    probe = 0 if site == "len" else 1
    result = getattr(bisect, operation)(target, probe)
    return (type(result).__name__, result), tuple(target.values)


def _sequence_case(operation: str, site: str, decisions: tuple[Any, ...]) -> None:
    _resume_against_fresh(
        lambda choose: _sequence_execution(operation, site, lambda: _callback(choose())),
        lambda decision: _sequence_execution(operation, site, lambda: _callback(decision)),
        decisions,
    )


for _operation in _OPERATIONS:
    _case(f"{_operation}_len_boundary")(lambda operation=_operation: _sequence_case(operation, "len", (1, 3, 1)))
    _case(f"{_operation}_getitem_boundary")(
        lambda operation=_operation: _sequence_case(operation, "getitem", (0, 2, 0))
    )
    _case(f"{_operation}_len_exception")(lambda operation=_operation: _sequence_case(operation, "len", ("raise", 3)))
    _case(f"{_operation}_getitem_exception")(
        lambda operation=_operation: _sequence_case(operation, "getitem", ("raise", 0))
    )

for _operation in ("insort_left", "insort_right", "insort"):
    _case(f"{_operation}_insert_boundary")(lambda operation=_operation: _sequence_case(operation, "insert", (0, 1, 0)))
    _case(f"{_operation}_insert_exception")(
        lambda operation=_operation: _sequence_case(operation, "insert", ("raise", 0))
    )


def _bound_execution(
    operation: str,
    site: str,
    callback: Callable[[], Any],
) -> tuple[Any, tuple[int, ...]]:
    class Bound:
        def __index__(self) -> int:
            return cast(int, callback())

    target = [1, 2, 3]
    lo: Any = Bound() if site == "lo" else 0
    hi: Any = Bound() if site == "hi" else 3
    probe = 4 if site == "lo" else 0
    result = getattr(bisect, operation)(target, probe, lo, hi)
    return (type(result).__name__, result), tuple(target)


def _bound_case(operation: str, site: str, decisions: tuple[Any, ...]) -> None:
    _resume_against_fresh(
        lambda choose: _bound_execution(operation, site, lambda: _callback(choose())),
        lambda decision: _bound_execution(operation, site, lambda: _callback(decision)),
        decisions,
    )


for _operation in _OPERATIONS:
    _case(f"{_operation}_lo_index_boundary")(lambda operation=_operation: _bound_case(operation, "lo", (0, 1, 0)))
    _case(f"{_operation}_hi_index_boundary")(lambda operation=_operation: _bound_case(operation, "hi", (1, 3, 1)))
    _case(f"{_operation}_lo_index_exception")(lambda operation=_operation: _bound_case(operation, "lo", ("raise", 0)))
    _case(f"{_operation}_hi_index_exception")(lambda operation=_operation: _bound_case(operation, "hi", ("raise", 3)))


@_case("ordinary_bounds_empty_and_aliases")
def _ordinary_bounds_empty_and_aliases() -> None:
    # These ordinary calls cover API corners; they are not continuation tests.
    assert bisect.bisect is bisect.bisect_right
    assert bisect.insort is bisect.insort_right

    values = [1, 2, 2, 4]
    assert bisect.bisect_left(values, 2, 1, 3) == 1
    assert bisect.bisect_right(values, 2, 1, 3) == 3
    assert bisect.bisect(values, 2, 1, 3) == 3
    for name in ("insort_left", "insort_right", "insort"):
        current = values.copy()
        assert getattr(bisect, name)(current, 2, 1, 3) is None
        assert current == [1, 2, 2, 2, 4]

    for name in ("bisect_left", "bisect_right", "bisect"):
        assert getattr(bisect, name)([], 1) == 0
    for name in ("insort_left", "insort_right", "insort"):
        current: list[int] = []
        assert getattr(bisect, name)(current, 1) is None
        assert current == [1]

    for name in _OPERATIONS:
        operation = getattr(bisect, name)
        current = [1]
        with pytest.raises(ValueError, match="lo must be non-negative"):
            operation(current, 1, -1)
        assert current == [1]


@_case("insort_left_async_multishot")
def _insort_left_async_multishot() -> None:
    def fresh(decision: Any) -> Outcome:
        class Item:
            def __init__(self, value: int) -> None:
                self.value = value

            def __lt__(self, other: Any) -> bool:
                if decision == "raise":
                    raise ValueError("async bisect comparison failed")
                return bool(decision)

        values = [Item(2)]
        try:
            bisect.insort_left(values, Item(1))
        except Exception as exc:
            return "raise", type(exc).__name__
        return "return", tuple(item.value for item in values)

    async def exercise() -> list[Outcome]:
        choose = effect("bisect-async-choice")
        handler = create_async_handler(choose)

        class Item:
            def __init__(self, value: int) -> None:
                self.value = value

            def __lt__(self, other: Any) -> bool:
                decision = choose()
                if decision == "raise":
                    raise ValueError("async bisect comparison failed")
                return bool(decision)

        async def run() -> tuple[int, ...]:
            values = [Item(2)]
            bisect.insort_left(values, Item(1))
            return tuple(item.value for item in values)

        @handler.on(choose)
        async def resume(k: Any) -> list[Outcome]:
            outcomes: list[Outcome] = []
            for decision in (True, False, "raise", True):
                try:
                    result = await k(decision)
                except Exception as exc:
                    outcome: Outcome = ("raise", type(exc).__name__)
                else:
                    outcome = ("return", result)
                assert outcome == fresh(decision)
                outcomes.append(outcome)
            return outcomes

        return cast(list[Outcome], await handler(run))

    outcomes = asyncio.run(exercise())
    assert outcomes[0] == outcomes[3]
    assert outcomes[0] != outcomes[1]
    assert outcomes[2] == ("raise", "ValueError")


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=10,
    )


@pytest.mark.parametrize("case_name", list(_CASES))
def test_bisect_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_bisect.py --case CASE")
    _CASES[sys.argv[2]]()
