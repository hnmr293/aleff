"""CPython compatibility probes for iterator-related builtins.

The compatibility helper runs each source once on pristine CPython and once
after importing aleff, then compares their deterministic process results.
The effect cases separately exercise the multi-shot behavior expected at
iterator constructor and protocol boundaries.
"""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
from textwrap import dedent
from typing import Any, Callable, cast

import pytest

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def test_map_normal_behavior_and_iterator_protocol() -> None:
    assert_cpython_compatible(
        dedent(
            """
            result = list(map(lambda left, right: left + right, [1, 2, 3], (10, 20, 30)))
            print(result)

            mapped = map(str, [1, 2])
            print(type(mapped).__name__, iter(mapped) is mapped, list(mapped), list(mapped))
            print(list(map(abs, [-3, 0, 4, -8])))
            print(list(map(lambda value: value * 2, ())))
            """
        )
    )


def test_map_errors_and_corner_cases() -> None:
    assert_cpython_compatible(
        dedent(
            """
            def show(label, operation):
                try:
                    print(label, "ok", repr(operation()))
                except BaseException as exc:
                    print(label, "error", type(exc).__name__, str(exc))

            show("missing", lambda: map())
            show("bad_keyword", lambda: map(str, (), bogus=True))
            show("non_callable", lambda: list(map(42, (1,))))

            class BadIterable:
                def __iter__(self):
                    raise RuntimeError("iter failed")

            show("bad_iterable", lambda: map(str, BadIterable()))
            class BadIterator:
                def __iter__(self):
                    return self

                def __next__(self):
                    raise RuntimeError("next failed")

            show("bad_next", lambda: list(map(str, BadIterator())))
            print(list(map(pow, (2, 3), (3, 2))))
            """
        )
    )


def test_filter_normal_behavior_and_truthiness() -> None:
    assert_cpython_compatible(
        dedent(
            """
            values = [0, 1, False, True, "", "text", [], [9], None]
            print(list(filter(None, values)))
            print(list(filter(lambda value: value % 2, range(8))))

            filtered = filter(lambda value: value > 1, [0, 1, 2, 3])
            print(type(filtered).__name__, iter(filtered) is filtered, list(filtered), list(filtered))
            print(list(filter(lambda value: value, ())))
            """
        )
    )


def test_filter_errors_and_source_exceptions() -> None:
    assert_cpython_compatible(
        dedent(
            """
            def show(label, operation):
                try:
                    print(label, "ok", repr(operation()))
                except BaseException as exc:
                    print(label, "error", type(exc).__name__, str(exc))

            show("missing", lambda: filter())
            show("bad_keyword", lambda: filter(function=None, iterable=()))
            show("too_many", lambda: filter(None, (), ()))
            show("bad_iterable", lambda: filter(None, 42))

            def predicate(value):
                if value == 2:
                    raise LookupError("predicate failed")
                return True

            show("predicate", lambda: list(filter(predicate, (1, 2, 3))))

            class BadIterable:
                def __iter__(self):
                    raise RuntimeError("iter failed")

            show("iterable", lambda: list(filter(None, BadIterable())))
            """
        )
    )


def test_zip_normal_behavior_and_empty_inputs() -> None:
    assert_cpython_compatible(
        dedent(
            """
            print(list(zip()))
            print(list(zip([1, 2, 3], (10, 20))))
            print(list(zip([1, 2], (10, 20), "ab")))
            zipped = zip((1,), (2,))
            print(type(zipped).__name__, iter(zipped) is zipped, list(zipped), list(zipped))
            print(list(zip([], [1, 2])))
            """
        )
    )


def test_zip_valid_and_strict_length_errors() -> None:
    assert_cpython_compatible(
        dedent(
            """
            def show(label, operation):
                try:
                    print(label, "ok", repr(operation()))
                except BaseException as exc:
                    print(label, "error", type(exc).__name__, str(exc))

            show("short_three", lambda: list(zip((1, 2), (3, 4), (5,), strict=True)))
            show("long_three", lambda: list(zip((1,), (2,), (3, 4), strict=True)))
            show("short_two", lambda: list(zip((1, 2), (3,), strict=True)))
            show("long_two", lambda: list(zip((1,), (2, 3), strict=True)))
            show("truthy_strict", lambda: list(zip((1,), (2,), strict=1)))
            """
        )
    )


def test_zip_keyword_validation_precedes_iteration() -> None:
    assert_cpython_compatible(
        dedent(
            """
            events = []

            class Iterable:
                def __iter__(self):
                    events.append("iter")
                    return iter(())

            try:
                zip(Iterable(), bogus=True)
            except TypeError as exc:
                print(type(exc).__name__, str(exc))
            print(events)

            events.clear()
            try:
                zip(Iterable())
            except TypeError as exc:
                print(type(exc).__name__, str(exc))
            print(events)
            """
        )
    )


def test_zip_strict_truth_validation_precedes_iteration() -> None:
    assert_cpython_compatible(
        dedent(
            """
            events = []

            class Strict:
                def __bool__(self):
                    events.append("strict-bool")
                    return False

            class Iterable:
                def __iter__(self):
                    events.append("iter")
                    return iter(())

            print(list(zip(Iterable(), strict=Strict())))
            print(events)
            """
        )
    )


def test_enumerate_normal_behavior_keywords_and_large_indices() -> None:
    assert_cpython_compatible(
        dedent(
            """
            print(list(enumerate(["a", "b"])))
            print(list(enumerate(("x", "y"), start=-2)))
            print(list(enumerate(iter(("value",)), start=10**100)))
            print(list(enumerate(iterable=(1, 2), start=7)))
            enumerated = enumerate(())
            print(type(enumerated).__name__, iter(enumerated) is enumerated, list(enumerated))
            """
        )
    )


def test_enumerate_errors_and_index_conversion() -> None:
    assert_cpython_compatible(
        dedent(
            """
            def show(label, operation):
                try:
                    print(label, "ok", repr(operation()))
                except BaseException as exc:
                    print(label, "error", type(exc).__name__, str(exc))

            show("missing", lambda: enumerate())
            show("too_many", lambda: enumerate((), 1, 2))
            show("bad_keyword", lambda: enumerate((), bogus=1))
            show("duplicate_iterable", lambda: enumerate((), iterable=()))
            show("bad_start", lambda: enumerate((), object()))
            show("bad_iterable", lambda: enumerate(42))

            class Start:
                def __index__(self):
                    return 3

            print(list(enumerate(("item",), Start())))
            """
        )
    )


def test_enumerate_calls_iter_once() -> None:
    assert_cpython_compatible(
        dedent(
            """
            events = []

            class Iterator:
                def __iter__(self):
                    events.append("iter")
                    return self

                def __next__(self):
                    raise StopIteration

            print(list(enumerate(Iterator())))
            print(events)
            """
        )
    )


def test_reversed_normal_fallback_and_custom_protocol() -> None:
    assert_cpython_compatible(
        dedent(
            """
            print(list(reversed([1, 2, 3])))
            print(list(reversed(("a", "b"))))

            class Sequence:
                def __len__(self):
                    return 3

                def __getitem__(self, index):
                    if index < 0 or index >= 3:
                        raise IndexError
                    return index * 10

            print(list(reversed(Sequence())))

            class Custom:
                def __reversed__(self):
                    return iter(("custom", 2, 1))

            print(list(reversed(Custom())))
            """
        )
    )


def test_reversed_errors_and_fallback_corner_cases() -> None:
    assert_cpython_compatible(
        dedent(
            """
            def show(label, operation):
                try:
                    print(label, "ok", repr(operation()))
                except BaseException as exc:
                    print(label, "error", type(exc).__name__, str(exc))

            show("missing", lambda: reversed())
            show("too_many", lambda: reversed((), ()))
            show("not_reversible", lambda: reversed(42))

            class NegativeLength:
                def __len__(self):
                    return -1

                def __getitem__(self, index):
                    return index

            show("negative_length", lambda: reversed(NegativeLength()))

            class BadItem:
                def __len__(self):
                    return 1

                def __getitem__(self, index):
                    raise ValueError("getitem failed")

            show("bad_item", lambda: list(reversed(BadItem())))
            """
        )
    )


Choose = Callable[[], Any]


def _resume_outcomes(run: Callable[[Choose], Any], values: tuple[Any, ...] = (1, 10)) -> list[Any]:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[Any]:
        outcomes: list[Any] = []
        for value in values:
            try:
                outcomes.append(k(value))
            except BaseException as exc:
                outcomes.append((type(exc).__name__, str(exc)))
        return outcomes

    return cast(list[Any], handler(lambda: run(choose)))


_EFFECT_CASES: dict[str, Callable[[], None]] = {}


def _effect_case(name: str) -> Callable[[Callable[[], None]], Callable[[], None]]:
    def register(case: Callable[[], None]) -> Callable[[], None]:
        _EFFECT_CASES[name] = case
        return case

    return register


@_effect_case("map_constructor")
def _map_constructor_effect() -> None:
    def run(choose: Choose) -> list[int]:
        class Iterable:
            def __iter__(self):
                choose()
                return iter((1, 2))

        return list(map(lambda value: value + 1, Iterable()))

    assert _resume_outcomes(run) == [[2, 3], [2, 3]]


@_effect_case("filter_constructor")
def _filter_constructor_effect() -> None:
    def run(choose: Choose) -> list[int]:
        class Iterable:
            def __iter__(self):
                choose()
                return iter((0, 1, 2))

        return list(filter(lambda value: value, Iterable()))

    assert _resume_outcomes(run) == [[1, 2], [1, 2]]


@_effect_case("zip_strict_truth")
def _zip_strict_truth_effect() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        class Strict:
            def __bool__(self) -> bool:
                return bool(choose())

        return list(zip((1, 2), (3,), strict=Strict()))  # pyright: ignore[reportArgumentType, reportUnknownArgumentType]

    actual = _resume_outcomes(run, (False, True))
    expected = [
        [(1, 3)],
        ("ValueError", "zip() argument 2 is shorter than argument 1"),
    ]
    assert actual == expected, f"actual: {actual!r}, expected: {expected!r}"


@_effect_case("reversed_custom_protocol")
def _reversed_custom_protocol_effect() -> None:
    def run(choose: Choose) -> list[int]:
        class Reversible:
            def __reversed__(self):
                choose()
                return iter((3, 2, 1))

        return list(reversed(Reversible()))  # pyright: ignore[reportArgumentType, reportCallIssue, reportUnknownArgumentType]

    assert _resume_outcomes(run) == [[3, 2, 1], [3, 2, 1]]


@_effect_case("reversed_error_state")
def _reversed_resume_finalizes_after_non_index_error() -> None:
    def run(choose: Choose) -> str:
        class Sequence:
            def __len__(self) -> int:
                return 2

            def __getitem__(self, index: int) -> int:
                if index == 1:
                    choose()
                    raise ValueError("getitem failed")
                return index

        iterator = reversed(Sequence())
        try:
            next(iterator)
        except ValueError:
            pass
        try:
            next(iterator)
        except BaseException as exc:
            return type(exc).__name__
        return "returned"

    assert _resume_outcomes(run) == ["StopIteration", "StopIteration"]


def _run_effect_case_in_subprocess(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--effect-case", name],
        text=True,
        capture_output=True,
        timeout=10,
    )


@pytest.mark.parametrize("name", sorted(_EFFECT_CASES))
def test_effectful_constructor_and_protocol_boundaries(name: str) -> None:
    result = _run_effect_case_in_subprocess(name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--effect-case":
        raise SystemExit("usage: test_cpython_compat_map_filter_zip_enumerate.py --effect-case CASE_NAME")
    _EFFECT_CASES[sys.argv[2]]()
