"""Continuation coverage for the itertools C accelerators.

Each case is run in a fresh process: an unsupported C continuation can abort
the interpreter instead of producing a Python exception.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator
import gc
import itertools
from pathlib import Path
import subprocess
import sys
from typing import Any, cast
import weakref
from decimal import Decimal
from fractions import Fraction

import pytest

from aleff import create_handler, effect


Choose = Callable[[], Any]
Case = Callable[[], None]
_CASES: dict[str, Case] = {}


def _case(name: str) -> Callable[[Case], Case]:
    def register(case: Case) -> Case:
        _CASES[name] = case
        return case

    return register


def _resume_outcomes(run: Callable[[Choose], Any], values: tuple[Any, ...] = (1, 10)) -> list[tuple[str, Any]]:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in values:
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    return cast(list[tuple[str, Any]], handler(lambda: run(choose)))


def _assert_equal(actual: Any, expected: Any) -> None:
    assert actual == expected, f"actual: {actual!r}, expected: {expected!r}"


class _EffectfulIterable:
    def __init__(self, choose: Choose, values: tuple[Any, ...] = (1, 2, 3)) -> None:
        self.choose = choose
        self.values = values

    def __iter__(self) -> Iterator[Any]:
        self.choose()
        return iter(self.values)


def _iter_case(factory: Callable[[Any], Any], expected: Callable[[Any], Any]) -> None:
    def run(choose: Choose) -> Any:
        return list(factory(map(lambda _item: choose(), (None,))))

    outcomes = _resume_outcomes(run)
    _assert_equal(outcomes, [("return", expected(1)), ("return", expected(10))])


@_case("accumulate")
def _accumulate() -> None:
    def run(choose: Choose) -> list[int]:
        return list(itertools.accumulate((1, 2), lambda left, right: left + cast(int, choose())))

    _assert_equal(_resume_outcomes(run), [("return", [1, 2]), ("return", [1, 11])])


@_case("batched")
def _batched() -> None:
    if not hasattr(itertools, "batched"):
        return

    def run(choose: Choose) -> list[tuple[int, ...]]:
        class Items:
            def __iter__(self) -> Iterator[int]:
                return iter((cast(int, choose()), 2, 3))

        return list(itertools.batched(Items(), 2))

    _assert_equal(_resume_outcomes(run), [("return", [(1, 2), (3,)]), ("return", [(10, 2), (3,)])])


@_case("chain")
def _chain() -> None:
    _iter_case(lambda source: itertools.chain(source, (20,)), lambda value: [value, 20])


@_case("chain_from_iterable")
def _chain_from_iterable() -> None:
    def run(choose: Choose) -> list[int]:
        return list(itertools.chain.from_iterable(_EffectfulIterable(choose, ((1,), (2,)))))

    _assert_equal(_resume_outcomes(run), [("return", [1, 2]), ("return", [1, 2])])


@_case("combinations")
def _combinations() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(itertools.combinations(_EffectfulIterable(choose), 2))

    _assert_equal(_resume_outcomes(run), [("return", [(1, 2), (1, 3), (2, 3)]), ("return", [(1, 2), (1, 3), (2, 3)])])


@_case("combinations_with_replacement")
def _combinations_with_replacement() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(itertools.combinations_with_replacement(_EffectfulIterable(choose), 2))

    expected = [(1, 1), (1, 2), (1, 3), (2, 2), (2, 3), (3, 3)]
    _assert_equal(_resume_outcomes(run), [("return", expected), ("return", expected)])


@_case("compress")
def _compress() -> None:
    _iter_case(lambda source: itertools.compress(source, (1,)), lambda value: [value])


@_case("count")
def _count() -> None:
    choose = effect("count-step")
    handler = create_handler(choose)

    class Step(int):
        def __new__(cls, callback: Choose) -> "Step":
            value = int.__new__(cls, 2)
            value.callback = callback
            return value

        def __radd__(self, other: object) -> int:
            if other == 0:
                self.callback()
            return int(other) + 2

    @handler.on(choose)
    def handle_count_step(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in (1, 10):
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    def run() -> list[int | str]:
        return list(itertools.islice(itertools.count(0, Step(choose)), 3)) + ["tail"]

    _assert_equal(
        handler(run),
        [("return", [0, 2, 4, "tail"]), ("return", [0, 2, 4, "tail"])],
    )

    assert list(itertools.islice(itertools.count(3, 2), 3)) == [3, 5, 7]
    assert list(itertools.islice(itertools.count(2.5, 0.5), 3)) == [2.5, 3.0, 3.5]
    assert list(itertools.islice(itertools.count(Fraction(1, 3), Fraction(1, 3)), 3)) == [
        Fraction(1, 3), Fraction(2, 3), Fraction(1, 1)
    ]
    assert list(itertools.islice(itertools.count(Decimal("1.2"), Decimal("1.2")), 3)) == [
        Decimal("1.2"), Decimal("2.4"), Decimal("3.6")
    ]
    assert list(itertools.islice(itertools.count(0, 0), 3)) == [0, 0, 0]
    assert list(itertools.islice(itertools.count(0, -2), 3)) == [0, -2, -4]

    class FailingStep(int):
        def __new__(cls) -> "FailingStep":
            return int.__new__(cls, 2)

        def __radd__(self, other: object) -> int:
            raise ValueError("count step failure")

    iterator = itertools.count(0, FailingStep())
    with pytest.raises(ValueError, match="count step failure"):
        next(iterator)
    with pytest.raises(ValueError, match="count step failure"):
        next(iterator)


@_case("cycle")
def _cycle() -> None:
    def run(choose: Choose) -> list[int]:
        return list(itertools.islice(itertools.cycle(map(lambda _item: choose(), (None,))), 3))

    _assert_equal(_resume_outcomes(run), [("return", [1, 1, 1]), ("return", [10, 10, 10])])


@_case("dropwhile")
def _dropwhile() -> None:
    def run(choose: Choose) -> list[int]:
        decision = bool(choose())
        return list(itertools.dropwhile(lambda _value: decision, (1, 2, 3)))

    _assert_equal(_resume_outcomes(run, (0, 1)), [("return", [1, 2, 3]), ("return", [])])


@_case("filterfalse")
def _filterfalse() -> None:
    _iter_case(
        lambda source: itertools.filterfalse(lambda value: value == 1, source),
        lambda value: [] if value == 1 else [value],
    )


@_case("groupby")
def _groupby() -> None:
    def run(choose: Choose) -> list[tuple[int, list[int]]]:
        return [
            (key, list(group))
            for key, group in itertools.groupby(_EffectfulIterable(choose, (1, 1, 2)))
        ]

    expected = [(1, [1, 1]), (2, [2])]
    _assert_equal(_resume_outcomes(run), [("return", expected), ("return", expected)])


@_case("groupby_key_default")
def _groupby_key_default() -> None:
    _assert_equal(
        [(key, list(group)) for key, group in itertools.groupby((1, 1, 2))],
        [(1, [1, 1]), (2, [2])],
    )


@_case("groupby_key_error")
def _groupby_key_error() -> None:
    def bad_key(_value: int) -> int:
        raise ValueError("key failed")

    groups = itertools.groupby((1,), bad_key)
    with pytest.raises(ValueError, match="key failed"):
        next(groups)
    _assert_equal(
        [(key, list(group)) for key, group in itertools.groupby((1, 1, 2), key=None)],
        [(1, [1, 1]), (2, [2])],
    )


@_case("islice")
def _islice() -> None:
    _iter_case(lambda source: itertools.islice(source, 2), lambda value: [value])


@_case("pairwise")
def _pairwise() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(itertools.pairwise(_EffectfulIterable(choose, (1, 2, 3))))

    expected = [(1, 2), (2, 3)]
    _assert_equal(_resume_outcomes(run), [("return", expected), ("return", expected)])


@_case("permutations")
def _permutations() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(itertools.permutations(_EffectfulIterable(choose, (1, 2)), 2))

    expected = [(1, 2), (2, 1)]
    _assert_equal(_resume_outcomes(run), [("return", expected), ("return", expected)])


@_case("product")
def _product() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(itertools.product(_EffectfulIterable(choose, (1, 2)), (3,)))

    _assert_equal(_resume_outcomes(run), [("return", [(1, 3), (2, 3)]), ("return", [(1, 3), (2, 3)])])


@_case("repeat")
def _repeat() -> None:
    choose = effect("repeat-times")
    handler = create_handler(choose)

    class Times:
        def __index__(self) -> int:
            return cast(int, choose())

    def run() -> list[str]:
        return list(itertools.repeat("x", Times())) + ["tail"]

    @handler.on(choose)
    def handle_repeat_times(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in (1, 2):
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    _assert_equal(
        handler(run),
        [("return", ["x", "tail"]), ("return", ["x", "x", "tail"])],
    )

    assert list(itertools.repeat("x", 3)) == ["x", "x", "x"]
    assert list(itertools.repeat("x", 0)) == []
    assert list(itertools.repeat("x", -1)) == []
    assert list(itertools.repeat("x", times=2)) == ["x", "x"]

    class InvalidIndex:
        def __index__(self) -> str:
            return "not an int"

    with pytest.raises(TypeError):
        itertools.repeat("x", InvalidIndex())
    with pytest.raises(TypeError):
        itertools.repeat("x", 1.5)
    with pytest.raises(OverflowError):
        itertools.repeat("x", 10**100)


@_case("starmap")
def _starmap() -> None:
    def run(choose: Choose) -> list[int]:
        return list(itertools.starmap(lambda left, right: left + right + choose(), ((1, 2),)))

    _assert_equal(_resume_outcomes(run), [("return", [4]), ("return", [13])])


@_case("takewhile")
def _takewhile() -> None:
    def run(choose: Choose) -> list[int]:
        decision = bool(choose())
        return list(itertools.takewhile(lambda _value: decision, (1, 2, 3)))

    _assert_equal(_resume_outcomes(run, (0, 1)), [("return", []), ("return", [1, 2, 3])])


@_case("tee_empty")
def _tee_empty() -> None:
    assert itertools.tee((1,), 0) == ()


@_case("unconsumed_iterator_gc")
def _unconsumed_iterator_gc() -> None:
    class Source:
        def __iter__(self) -> "Source":
            return self

        def __next__(self) -> int:
            return 1

    source = Source()
    source_ref = weakref.ref(source)
    iterator = itertools.cycle(source)
    del source, iterator
    gc.collect()
    assert source_ref() is None


@_case("zip_longest")
def _zip_longest() -> None:
    _iter_case(lambda source: itertools.zip_longest(source, (2,)), lambda value: [(value, 2)])


@_case("zip_longest_fill")
def _zip_longest_fill() -> None:
    assert list(itertools.zip_longest((1,), (2, 3), fillvalue=99)) == [(1, 2), (99, 3)]
    assert list(itertools.zip_longest()) == []


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=10,
    )


@pytest.mark.parametrize("case_name", list(_CASES))
def test_itertools_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_itertools.py --case CASE_NAME")
    _CASES[sys.argv[2]]()
