"""Strict multi-shot continuation coverage for heapq C heap operations."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import heapq
from pathlib import Path
import subprocess
import sys
import sysconfig
import threading
import time
from typing import Any, cast

import pytest

from aleff import create_async_handler, create_handler, effect


Choose = Callable[[], Any]
Run = Callable[[Choose], Any]
Case = Callable[[], None]
Outcome = tuple[str, Any]
_CASES: dict[str, Case] = {}


def _case(name: str) -> Callable[[Case], Case]:
    def register(case: Case) -> Case:
        _CASES[name] = case
        return case

    return register


def _outcome(run: Callable[[], Any]) -> Outcome:
    try:
        return "return", run()
    except Exception as exc:
        return "raise", type(exc).__name__


def _fresh_outcome(run: Run, decision: Any) -> Outcome:
    """Run the operation from scratch with one fixed callback decision."""

    return _outcome(lambda: run(lambda: decision))


def _resume_outcomes(run: Run, decisions: tuple[Any, ...]) -> list[Outcome]:
    """Compare every resume with a fresh ordinary execution of the operation."""

    choose = effect("heapq-choice")
    suspension_count = 0
    handler = create_handler(choose)

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        outcomes: list[Outcome] = []
        for decision in decisions:
            expected = _fresh_outcome(run, decision)
            actual = _outcome(lambda: k(decision))
            assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            outcomes.append(actual)
        return outcomes

    result = handler(lambda: run(choose))
    assert suspension_count == 1, "the scenario must suspend at exactly one comparison callback"
    return cast(list[Outcome], result)


def _heap_values(heap: list[Any]) -> tuple[int, ...]:
    return tuple(item.value for item in heap)


_HEAP_COMPARISON_PAIRS = {
    "heapify": (1, 2),
    "heappush": (1, 2),
    "heappop": (2, 3),
    "heappushpop": (2, 1),
    "heapreplace": (2, 3),
}


def _heap_run(name: str, choose: Choose) -> tuple[Any, tuple[int, ...]]:
    triggered = False
    comparison_pair = _HEAP_COMPARISON_PAIRS[name]

    class Item:
        def __init__(self, value: int) -> None:
            self.value = value

        def __lt__(self, other: Any) -> bool:
            nonlocal triggered
            if not triggered and (self.value, other.value) == comparison_pair:
                triggered = True
                decision = choose()
                if decision == "raise":
                    raise ValueError("heap comparison failed")
                return bool(decision)
            return self.value < other.value

    if name == "heapify":
        heap = [Item(3), Item(1), Item(2)]
        result = heapq.heapify(heap)
    elif name == "heappush":
        heap = [Item(2), Item(4)]
        result = heapq.heappush(heap, Item(1))
    elif name == "heappop":
        heap = [Item(1), Item(2), Item(3), Item(4)]
        result = heapq.heappop(heap)
    elif name == "heappushpop":
        heap = [Item(2), Item(3)]
        result = heapq.heappushpop(heap, Item(1))
    else:
        heap = [Item(1), Item(2), Item(3), Item(4)]
        result = heapq.heapreplace(heap, Item(5))

    result_value = None if result is None else result.value
    return result_value, _heap_values(heap)


def _assert_normal_multishot(run: Run) -> None:
    outcomes = _resume_outcomes(run, (True, False, True))
    assert outcomes[0] == outcomes[2]
    assert outcomes[0] != outcomes[1]


def _assert_exception_resume(run: Run) -> None:
    outcomes = _resume_outcomes(run, ("raise", True))
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "return"


for _heap_name in _HEAP_COMPARISON_PAIRS:
    _case(f"{_heap_name}_comparison_multishot")(
        lambda name=_heap_name: _assert_normal_multishot(lambda choose: _heap_run(name, choose))
    )
    _case(f"{_heap_name}_comparison_exception_isolated")(
        lambda name=_heap_name: _assert_exception_resume(lambda choose: _heap_run(name, choose))
    )


def _nested_list_comparison_run(choose: Choose) -> tuple[None, tuple[int, ...]]:
    triggered = False

    class Item:
        def __init__(self, value: int) -> None:
            self.value = value

        def __eq__(self, other: object) -> bool:
            return isinstance(other, Item) and self.value == other.value

        def __lt__(self, other: Any) -> bool:
            nonlocal triggered
            if not triggered and (self.value, other.value) == (1, 2):
                triggered = True
                decision = choose()
                if decision == "raise":
                    raise ValueError("nested comparison failed")
                return bool(decision)
            return self.value < other.value

    heap = [[Item(2)], [Item(4)]]
    result = heapq.heappush(heap, [Item(1)])
    return result, tuple(entry[0].value for entry in heap)


@_case("heappush_nested_list_comparison_multishot")
def _heappush_nested_list_comparison_multishot() -> None:
    _assert_normal_multishot(_nested_list_comparison_run)


@_case("heappush_nested_list_comparison_exception_isolated")
def _heappush_nested_list_comparison_exception_isolated() -> None:
    _assert_exception_resume(_nested_list_comparison_run)


def _heapify_size_mutation_run(
    choose: Choose,
) -> tuple[Outcome, tuple[int, ...]]:
    triggered = False

    class Item:
        def __init__(self, value: int) -> None:
            self.value = value

        def __lt__(self, other: Any) -> bool:
            nonlocal triggered
            if not triggered and (self.value, other.value) == (1, 2):
                triggered = True
                decision = choose()
                heap.append(Item(99))
                return bool(decision)
            return self.value < other.value

    heap = [Item(3), Item(1), Item(2)]
    try:
        result = heapq.heapify(heap)
    except Exception as exc:
        outcome: Outcome = (type(exc).__name__, str(exc))
    else:
        outcome = ("return", result)
    return outcome, _heap_values(heap)


@_case("heapify_comparison_callback_size_mutation_multishot")
def _heapify_comparison_callback_size_mutation_multishot() -> None:
    decisions = (True, False, True)
    choose = effect("heapq-size-mutation-choice")
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(
        k: Any,
    ) -> list[
        tuple[
            tuple[Outcome, tuple[int, ...]],
            tuple[Outcome, tuple[int, ...]],
        ]
    ]:
        nonlocal suspension_count
        suspension_count += 1
        comparisons = []
        for decision in decisions:
            actual = cast(tuple[Outcome, tuple[int, ...]], k(decision))
            expected = _heapify_size_mutation_run(lambda decision=decision: decision)
            comparisons.append((actual, expected))
        return comparisons

    comparisons = cast(
        list[
            tuple[
                tuple[Outcome, tuple[int, ...]],
                tuple[Outcome, tuple[int, ...]],
            ]
        ],
        handler(lambda: _heapify_size_mutation_run(choose)),
    )
    assert suspension_count == 1
    assert all(actual == expected for actual, expected in comparisons), comparisons
    for actual, _expected in comparisons:
        assert actual[0] == ("RuntimeError", "list changed size during iteration")
    assert comparisons[0][0] == comparisons[2][0]


def _heappushpop_size_mutation_run(
    choose: Choose,
    *,
    clear: bool = False,
    is_max: bool = False,
) -> tuple[Outcome, tuple[int, ...]]:
    triggered = False

    class Item:
        def __init__(self, value: int) -> None:
            self.value = value

        def __lt__(self, other: Any) -> bool:
            nonlocal triggered
            if not triggered:
                triggered = True
                decision = choose()
                if decision == "raise":
                    raise ValueError("pushpop comparison failed")
                if clear:
                    heap.clear()
                else:
                    heap.append(Item(9))
                return bool(decision)
            return self.value < other.value

    heap = [Item(3), Item(2)] if is_max else [Item(1), Item(2)]
    try:
        if is_max:
            result = heapq.heappushpop_max(heap, Item(1))
        else:
            result = heapq.heappushpop(heap, Item(3))
    except Exception as exc:
        outcome: Outcome = (type(exc).__name__, str(exc))
    else:
        outcome = ("return", result.value)
    return outcome, _heap_values(heap)


@_case("heappushpop_comparison_callback_size_mutation_multishot")
def _heappushpop_comparison_callback_size_mutation_multishot() -> None:
    _resume_outcomes(
        _heappushpop_size_mutation_run,
        (True, False, "raise", True),
    )


@_case("heappushpop_comparison_callback_clear_multishot")
def _heappushpop_comparison_callback_clear_multishot() -> None:
    _resume_outcomes(
        lambda choose: _heappushpop_size_mutation_run(choose, clear=True),
        (True, False, "raise", True),
    )


if hasattr(heapq, "heappushpop_max"):

    @_case("heappushpop_max_comparison_callback_size_mutation_multishot")
    def _heappushpop_max_comparison_callback_size_mutation_multishot() -> None:
        _resume_outcomes(
            lambda choose: _heappushpop_size_mutation_run(choose, is_max=True),
            (True, False, "raise", True),
        )

    @_case("heappushpop_max_comparison_callback_clear_multishot")
    def _heappushpop_max_comparison_callback_clear_multishot() -> None:
        _resume_outcomes(
            lambda choose: _heappushpop_size_mutation_run(
                choose,
                clear=True,
                is_max=True,
            ),
            (True, False, "raise", True),
        )


_MAX_NAMES = ("heapify_max", "heappush_max", "heappop_max", "heappushpop_max", "heapreplace_max")
_MAX_COMPARISON_PAIRS = {
    "heapify_max": (2, 3),
    "heappush_max": (2, 3),
    "heappop_max": (1, 2),
    "heappushpop_max": (3, 2),
    "heapreplace_max": (1, 2),
}


def _max_heap_run(name: str, choose: Choose) -> tuple[Any, tuple[int, ...]]:
    triggered = False
    comparison_pair = _MAX_COMPARISON_PAIRS[name]

    class Item:
        def __init__(self, value: int) -> None:
            self.value = value

        def __lt__(self, other: Any) -> bool:
            nonlocal triggered
            if not triggered and (self.value, other.value) == comparison_pair:
                triggered = True
                decision = choose()
                if decision == "raise":
                    raise ValueError("max-heap comparison failed")
                return bool(decision)
            return self.value < other.value

    if name == "heapify_max":
        heap = [Item(1), Item(3), Item(2)]
        result = heapq.heapify_max(heap)
    elif name == "heappush_max":
        heap = [Item(2)]
        result = heapq.heappush_max(heap, Item(3))
    elif name == "heappop_max":
        heap = [Item(3), Item(2), Item(1), Item(0)]
        result = heapq.heappop_max(heap)
    elif name == "heappushpop_max":
        heap = [Item(2), Item(1)]
        result = heapq.heappushpop_max(heap, Item(3))
    else:
        heap = [Item(3), Item(2), Item(1), Item(0)]
        result = heapq.heapreplace_max(heap, Item(-1))

    result_value = None if result is None else result.value
    return result_value, _heap_values(heap)


for _max_name in _MAX_NAMES:
    if hasattr(heapq, _max_name):
        _case(f"{_max_name}_comparison_multishot")(
            lambda name=_max_name: _assert_normal_multishot(lambda choose: _max_heap_run(name, choose))
        )
        _case(f"{_max_name}_comparison_exception_isolated")(
            lambda name=_max_name: _assert_exception_resume(lambda choose: _max_heap_run(name, choose))
        )


if sysconfig.get_config_var("Py_GIL_DISABLED"):

    @_case("heapify_resume_holds_heap_lock")
    def _heapify_resume_holds_heap_lock() -> None:
        choose = effect("heapq-resume-lock-choice")
        handler = create_handler(choose)
        state = {
            "comparisons": 0,
            "second_entered": False,
            "attempting": False,
            "finished": False,
            "finished_during_comparison": None,
        }

        class Item:
            def __init__(self, value: int) -> None:
                self.value = value

            def __lt__(self, other: Any) -> bool:
                state["comparisons"] += 1
                if state["comparisons"] == 1:
                    return bool(choose())
                if state["comparisons"] == 2:
                    state["second_entered"] = True

                    deadline = time.monotonic() + 2
                    while not state["attempting"] and time.monotonic() < deadline:
                        pass
                    assert state["attempting"], "worker did not attempt heap mutation"

                    deadline = time.monotonic() + 0.2
                    while not state["finished"] and time.monotonic() < deadline:
                        pass
                    state["finished_during_comparison"] = state["finished"]

                return self.value < other.value

        heap = [Item(4), Item(3), Item(2), Item(1)]

        def mutate_heap() -> None:
            while not state["second_entered"]:
                pass
            state["attempting"] = True
            heap.append(Item(9))
            state["finished"] = True

        worker = threading.Thread(target=mutate_heap)
        worker.start()

        @handler.on(choose)
        def resume(k: Any) -> Any:
            return k(True)

        result = handler(lambda: heapq.heapify(heap))
        worker.join(timeout=2)

        assert result is None
        assert state["finished_during_comparison"] is False
        assert state["finished"] is True
        assert not worker.is_alive()


@_case("heappush_async_multishot")
def _heappush_async_multishot() -> None:
    def fresh(decision: Any) -> Outcome:
        class Item:
            def __init__(self, value: int) -> None:
                self.value = value

            def __lt__(self, other: Any) -> bool:
                if decision == "raise":
                    raise ValueError("async heap comparison failed")
                return bool(decision)

        heap = [Item(2)]
        try:
            heapq.heappush(heap, Item(1))
        except Exception as exc:
            return "raise", type(exc).__name__
        return "return", tuple(item.value for item in heap)

    async def exercise() -> list[Outcome]:
        choose = effect("heapq-async-choice")
        handler = create_async_handler(choose)

        class Item:
            def __init__(self, value: int) -> None:
                self.value = value

            def __lt__(self, other: Any) -> bool:
                decision = choose()
                if decision == "raise":
                    raise ValueError("async heap comparison failed")
                return bool(decision)

        async def run() -> tuple[int, ...]:
            heap = [Item(2)]
            heapq.heappush(heap, Item(1))
            return tuple(item.value for item in heap)

        @handler.on(choose)
        async def resume(k: Any) -> list[Outcome]:
            outcomes = []
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
def test_heapq_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_heapq.py --case CASE_NAME")
    _CASES[sys.argv[2]]()
