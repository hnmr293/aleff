"""CPython compatibility coverage for the itertools adapters in Issue #55."""

from __future__ import annotations

from collections.abc import Callable, Iterator
import itertools
from pathlib import Path
import subprocess
import sys
from typing import Any, cast

import pytest

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


ACCUMULATE_BEHAVIOR = """
import inspect
import itertools

def report(label, operation):
    try:
        value = operation()
    except Exception as exc:
        print(label, "ERR", type(exc).__name__, repr(str(exc)))
    else:
        print(label, "OK", type(value).__name__, repr(value))

report("default", lambda: list(itertools.accumulate([1, 2, 3, 4])))
report("initial", lambda: list(itertools.accumulate([1, 2, 3], initial=10)))
report("explicit-none", lambda: list(itertools.accumulate([1, 2], initial=None)))
report("none-function", lambda: list(itertools.accumulate([1, 2], None, initial=5)))
report(
    "function-and-initial",
    lambda: list(itertools.accumulate([1, 2, 3], lambda left, right: left * 10 + right, initial=0)),
)
report("strings", lambda: list(itertools.accumulate(["a", "b", "c"])))
report("empty", lambda: list(itertools.accumulate([], initial=42)))

iterator = itertools.accumulate([5])
print("iterator", type(iterator).__name__, next(iterator), next(iterator, "exhausted"))

class Failing:
    def __call__(self, _left, _right):
        raise ValueError("accumulate callback failed")

failing = itertools.accumulate([1, 2], Failing())
report("callback-error-1", lambda: next(failing))
report("callback-error-2", lambda: next(failing))
report("bad-function", lambda: list(itertools.accumulate([1, 2], 3)))
report("bad-iterable", lambda: list(itertools.accumulate(3)))
report("unexpected-keyword", lambda: itertools.accumulate([], nope=True))
print("signature", str(inspect.signature(itertools.accumulate)))
"""


ACCUMULATE_LIFETIME = """
import gc
import itertools
import weakref

class Token:
    pass

token = Token()
reference = weakref.ref(token)
iterator = itertools.accumulate((), initial=token)
assert next(iterator) is token
del iterator
del token
gc.collect()
print(reference() is None)
"""


BATCHED_BEHAVIOR = """
import inspect
import itertools
import sys

def report(label, operation):
    try:
        value = operation()
    except Exception as exc:
        print(label, "ERR", type(exc).__name__, repr(str(exc)))
    else:
        print(label, "OK", type(value).__name__, repr(value))

report("full", lambda: list(itertools.batched(range(6), 3)))
report("partial", lambda: list(itertools.batched(range(5), 2)))
report("singleton", lambda: list(itertools.batched([9], 1)))
report("empty", lambda: list(itertools.batched([], 4)))
report("boolean-size", lambda: list(itertools.batched([1, 2], True)))
report("zero-size", lambda: list(itertools.batched([], 0)))
report("negative-size", lambda: list(itertools.batched([], -1)))
report("float-size", lambda: list(itertools.batched([], 1.5)))
report("string-size", lambda: list(itertools.batched([], "2")))
report("bad-iterable", lambda: list(itertools.batched(3, 2)))
report("unexpected-keyword", lambda: itertools.batched([], 2, nope=True))

if sys.version_info >= (3, 13):
    report("strict-full", lambda: list(itertools.batched(range(4), 2, strict=True)))
    report("strict-partial", lambda: list(itertools.batched(range(3), 2, strict=True)))
    report("strict-default", lambda: list(itertools.batched(range(3), 2, strict=False)))

print("signature", str(inspect.signature(itertools.batched)))
"""


CHAIN_BEHAVIOR = """
import itertools

def report(label, operation):
    try:
        value = operation()
    except Exception as exc:
        print(label, "ERR", type(exc).__name__, repr(str(exc)))
    else:
        print(label, "OK", type(value).__name__, repr(value))

report("chain", lambda: list(itertools.chain((1, 2), (), [3, 4])))
report("chain-empty", lambda: list(itertools.chain()))
report("from-iterable", lambda: list(itertools.chain.from_iterable(((1, 2), (), [3]))))
report("from-empty", lambda: list(itertools.chain.from_iterable(())))
report("from-bad-outer", lambda: list(itertools.chain.from_iterable(3)))
report("chain-keyword", lambda: itertools.chain(iterable=()))
report("from-too-few", lambda: itertools.chain.from_iterable())
report("from-too-many", lambda: itertools.chain.from_iterable((), ()))
report("from-keyword", lambda: itertools.chain.from_iterable(iterable=()))

def chain_bad_inner():
    iterator = itertools.chain((1,), (2,), 3)
    values = [next(iterator), next(iterator)]
    try:
        next(iterator)
    except Exception as exc:
        return values, type(exc).__name__, str(exc)

report("chain-lazy-error", chain_bad_inner)

def from_bad_inner():
    iterator = itertools.chain.from_iterable(((1,), 3))
    first = next(iterator)
    try:
        next(iterator)
    except Exception as exc:
        return first, type(exc).__name__, str(exc)

report("from-lazy-error", from_bad_inner)
"""


CHAIN_FROM_ITERABLE_INTROSPECTION = """
import inspect
import itertools

descriptor = itertools.chain.__dict__["from_iterable"]
print(type(descriptor).__name__)
print(repr(itertools.chain.from_iterable.__doc__))
print(repr(itertools.chain.from_iterable.__text_signature__))
print(str(inspect.signature(itertools.chain.from_iterable)))
"""


@pytest.mark.parametrize(
    "source",
    (
        ACCUMULATE_BEHAVIOR,
        ACCUMULATE_LIFETIME,
        BATCHED_BEHAVIOR,
        CHAIN_BEHAVIOR,
        CHAIN_FROM_ITERABLE_INTROSPECTION,
    ),
    ids=(
        "accumulate behavior",
        "accumulate initial lifetime",
        "batched behavior",
        "chain behavior",
        "chain.from_iterable introspection",
    ),
)
def test_itertools_matches_cpython(source: str) -> None:
    """Compare deterministic output before and after importing aleff."""

    assert_cpython_compatible(source)


Choose = Callable[[], Any]
RunWithChoose = Callable[[Choose], Any]
_EFFECT_CASES: dict[str, Callable[[], None]] = {}


def _effect_case(name: str) -> Callable[[Callable[[], None]], Callable[[], None]]:
    def register(case: Callable[[], None]) -> Callable[[], None]:
        _EFFECT_CASES[name] = case
        return case

    return register


def _resume_outcomes(run: RunWithChoose) -> list[tuple[str, Any]]:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in (1, 10):
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    return cast(list[tuple[str, Any]], handler(lambda: run(choose)))


@_effect_case("accumulate_constructor")
def _accumulate_constructor_effect() -> None:
    def run(choose: Choose) -> list[int]:
        class Items:
            def __iter__(self) -> Iterator[int]:
                value = cast(int, choose())
                return iter((value, 2))

        return list(itertools.accumulate(Items()))

    assert _resume_outcomes(run) == [("return", [1, 3]), ("return", [10, 12])]


@_effect_case("batched_input_next")
def _batched_input_next_effect() -> None:
    def run(choose: Choose) -> list[tuple[int, ...]]:
        class Source:
            def __init__(self) -> None:
                self.finished = False

            def __iter__(self) -> "Source":
                return self

            def __next__(self) -> int:
                if self.finished:
                    raise StopIteration
                self.finished = True
                return cast(int, choose())

        return list(itertools.batched(Source(), 1))

    assert _resume_outcomes(run) == [("return", [(1,)]), ("return", [(10,)])]


def _run_effect_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--effect-case", name],
        text=True,
        capture_output=True,
        timeout=10,
    )


@pytest.mark.parametrize("case", tuple(_EFFECT_CASES))
def test_itertools_effect_resume_boundaries(case: str) -> None:
    result = _run_effect_case(case)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--effect-case":
        raise SystemExit("usage: test_cpython_compat_itertools_accumulate_chain.py --effect-case CASE")
    _EFFECT_CASES[sys.argv[2]]()
