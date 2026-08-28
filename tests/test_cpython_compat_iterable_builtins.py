"""CPython compatibility regressions for iterable-consuming built-ins."""

from __future__ import annotations

import subprocess
import sys

import pytest

from cpython_compat_support import assert_cpython_compatible


def test_all_and_any_normal_behavior_and_short_circuit() -> None:
    assert_cpython_compatible(
        """
events = []

class Probe:
    def __init__(self, name, value):
        self.name = name
        self.value = value

    def __bool__(self):
        events.append(self.name)
        return self.value

print(type(all([])).__name__, all([]), events)
events.clear()
print(all([Probe("a", True), Probe("b", False), Probe("c", True)]), events)
events.clear()
print(any([Probe("a", False), Probe("b", True), Probe("c", True)]), events)
events.clear()
print(any([]), all([True, 1, object()]))
        """
    )


def test_all_and_any_errors_match_cpython() -> None:
    assert_cpython_compatible(
        """
def describe(call):
    try:
        call()
    except Exception as exc:
        print(type(exc).__name__, str(exc))
    else:
        print("no error")

describe(lambda: all(1))
describe(lambda: any(None))

class FailingTruth:
    def __bool__(self):
        raise LookupError("truth failed")

describe(lambda: all([True, FailingTruth()]))
describe(lambda: any([False, FailingTruth()]))
        """
    )


@pytest.mark.parametrize("name", ["min", "max"])
def test_min_max_normal_behavior_and_corner_cases(name: str) -> None:
    assert_cpython_compatible(
        f"""
pick = {name}

class Item:
    def __init__(self, value, label):
        self.value = value
        self.label = label

    def __repr__(self):
        return self.label

items = [Item(2, "two-a"), Item(1, "one"), Item(2, "two-b")]
calls = []

def key(item):
    calls.append(item.label)
    return item.value

print(pick(items, key=key), calls)
print(pick([], default="fallback"))
print(pick(3, 1, 2))

for call in (
    lambda: pick([]),
    lambda: pick([1, "x"]),
    lambda: pick([1], default="fallback", key=None),
    lambda: pick(1, 2, default="fallback"),
):
    try:
        call()
    except Exception as exc:
        print(type(exc).__name__, str(exc))
    else:
        print("no error")
        """
    )


def test_sum_normal_numeric_behavior_and_corner_cases() -> None:
    assert_cpython_compatible(
        """
print(sum([]), type(sum([])).__name__)
print(sum([True, False, True]), type(sum([True, False, True])).__name__)
print(sum([1, 2, 3], 10), sum((1.5, 2.5), 0.5))
print(sum([1.0e16, 1.0, -1.0e16]))
print(sum([1 + 2j, 3 + 4j], 10 + 20j))

def describe(call):
    try:
        call()
    except Exception as exc:
        print(type(exc).__name__, str(exc))
    else:
        print("no error")

describe(lambda: sum([1], ""))
describe(lambda: sum([1], b""))
describe(lambda: sum([1], bytearray()))
describe(lambda: sum(1, ""))
describe(lambda: sum(iterable=[1]))
        """
    )


def test_sum_does_not_reenter_a_numeric_fast_path_after_generic_fallback() -> None:
    assert_cpython_compatible(
        """
class F:
    def __radd__(self, other):
        return 0.0

print(sum([F(), 1.0e16, 1.0, -1.0e16], 1.0))
        """
    )


def test_sorted_normal_behavior_stability_and_non_mutation() -> None:
    assert_cpython_compatible(
        """
items = [(2, "first"), (1, "low"), (2, "second"), (1, "last")]
original = items[:]
calls = []

def key(item):
    calls.append(item[1])
    return item[0]

ascending = sorted(items, key=key)
descending = sorted(items, key=lambda item: item[0], reverse=True)
print(ascending, calls)
print(descending, items == original, ascending is items)
print(sorted((value for value in (3, 1, 2))))
        """
    )


def test_sorted_errors_and_evaluation_order_match_cpython() -> None:
    assert_cpython_compatible(
        """
def describe(call):
    try:
        call()
    except Exception as exc:
        print(type(exc).__name__, str(exc))
    else:
        print("no error")

describe(lambda: sorted(iterable=[2, 1]))
describe(lambda: sorted([1, "x"]))
describe(lambda: sorted([1], reverse=None, key=1))

events = []

class Items:
    def __iter__(self):
        events.append("iter")
        return iter(())

class Reverse:
    def __bool__(self):
        events.append("bool")
        return False

sorted(Items(), reverse=Reverse())
print(events)

comparisons = []

class K:
    def __init__(self, value):
        self.value = value

    def __lt__(self, other):
        comparisons.append((self.value, other.value))
        return self.value < other.value

sorted([1, 3, 2], key=K)
print(comparisons)
        """
    )


def test_sorted_timsort_run_detection_comparison_order_matches_cpython() -> None:
    assert_cpython_compatible(
        """
import sys

def trace(values):
    comparisons = []

    class Key:
        def __init__(self, value, index):
            self.value = value
            self.index = index

        def __lt__(self, other):
            comparisons.append((self.value, self.index, other.value, other.index))
            return self.value < other.value

    result = sorted(
        enumerate(values),
        key=lambda item: Key(item[1], item[0]),
    )
    return [value for _, value in result], comparisons

rise_and_drop = trace([1, 3, 2])
expected = (
    [(3, 1, 1, 0), (2, 2, 3, 1), (2, 2, 3, 1), (2, 2, 1, 0)]
    if sys.version_info < (3, 13)
    else [
        (3, 1, 1, 0),
        (2, 2, 3, 1),
        (1, 0, 3, 1),
        (2, 2, 3, 1),
        (2, 2, 1, 0),
    ]
)
assert rise_and_drop == ([1, 2, 3], expected), rise_and_drop
print("rise_and_drop", rise_and_drop)

for label, values in (
    ("equal_descending_run", [3, 2, 2, 1]),
    ("descending_then_ascending_suffix", [3, 2, 1, 3, 4, 0]),
):
    print(label, trace(values))
        """
    )


def test_sorted_timsort_reverse_stability_and_boundaries_match_cpython() -> None:
    assert_cpython_compatible(
        """
import hashlib

def trace(values, *, reverse=False):
    comparisons = []

    class Key:
        def __init__(self, value, index):
            self.value = value
            self.index = index

        def __lt__(self, other):
            comparisons.append((self.value, self.index, other.value, other.index))
            return self.value < other.value

    result = sorted(
        enumerate(values),
        key=lambda item: Key(item[1], item[0]),
        reverse=reverse,
    )
    digest = hashlib.sha256(repr(comparisons).encode()).hexdigest()
    return result, len(comparisons), digest

for label, values, reverse in (
    ("empty", [], False),
    ("single", [1], False),
    ("all_equal", [2] * 8, False),
    ("stable_forward", [2, 1, 2, 1], False),
    ("stable_reverse", [2, 1, 2, 1], True),
    ("minrun_63", [index % 7 for index in range(63, 0, -1)], False),
    ("minrun_64", [index % 7 for index in range(64, 0, -1)], False),
    ("minrun_65", [index % 7 for index in range(65, 0, -1)], False),
    ("multiple_runs_merge", list(range(40)) + list(range(-40, 0)), False),
):
    print(label, trace(values, reverse=reverse))
        """,
        timeout=30,
    )


def test_sorted_timsort_callback_failures_and_mutation_match_cpython() -> None:
    assert_cpython_compatible(
        """
def exercise(mode):
    values = [1, 3, 2]
    comparisons = []

    class TruthFailure:
        def __bool__(self):
            raise LookupError("truth failed")

    class Key:
        def __init__(self, value):
            self.value = value

        def __lt__(self, other):
            pair = (self.value, other.value)
            comparisons.append(pair)
            if pair == (1, 3):
                if mode == "comparison_error":
                    raise RuntimeError("comparison failed")
                if mode == "truth_error":
                    return TruthFailure()
                if mode == "mutation":
                    values.append(99)
            return self.value < other.value

    try:
        result = sorted(values, key=Key)
    except Exception as exc:
        outcome = ("raise", type(exc).__name__, str(exc))
    else:
        outcome = ("return", result)
    print(mode, outcome, values, comparisons)

for mode in ("comparison_error", "truth_error", "mutation"):
    exercise(mode)
        """
    )


def test_sorted_and_list_sort_timsort_traces_are_identical() -> None:
    assert_cpython_compatible(
        """
import hashlib

def trace(operation, values, *, reverse=False):
    comparisons = []

    class Key:
        def __init__(self, value, index):
            self.value = value
            self.index = index

        def __lt__(self, other):
            comparisons.append((self.value, self.index, other.value, other.index))
            return self.value < other.value

    indexed = list(enumerate(values))
    if operation == "sorted":
        result = sorted(
            indexed,
            key=lambda item: Key(item[1], item[0]),
            reverse=reverse,
        )
    else:
        result = indexed
        result.sort(
            key=lambda item: Key(item[1], item[0]),
            reverse=reverse,
        )
    return result, comparisons

for label, values, reverse in (
    ("rise_and_drop", [1, 3, 2], False),
    ("equal_descending_run", [3, 2, 2, 1], False),
    ("descending_suffix", [3, 2, 1, 3, 4, 0], False),
    ("stable_reverse", [2, 1, 2, 1], True),
    ("minrun_63", [index % 7 for index in range(63, 0, -1)], False),
    ("minrun_64", [index % 7 for index in range(64, 0, -1)], False),
    ("minrun_65", [index % 7 for index in range(65, 0, -1)], False),
    ("multiple_runs_merge", list(range(40)) + list(range(-40, 0)), False),
):
    sorted_trace = trace("sorted", values, reverse=reverse)
    list_trace = trace("list.sort", values, reverse=reverse)
    assert sorted_trace == list_trace, (label, sorted_trace, list_trace)
    result, comparisons = sorted_trace
    digest = hashlib.sha256(repr(comparisons).encode()).hexdigest()
    print(label, result, len(comparisons), digest)
        """,
        timeout=30,
    )


def _effect_case_source(operation: str) -> str:
    if operation in {"all", "any"}:
        return (
            "from aleff import create_handler, effect\n"
            f"choose = effect('choose')\nhandler = create_handler(choose)\n"
            "class Value:\n"
            "    def __bool__(self):\n"
            "        return bool(choose())\n"
            "@handler.on(choose)\n"
            "def handle(k):\n"
            "    return [k(value) for value in (0, 1)]\n"
            f"print(handler(lambda: {operation}([Value()])))\n"
        )
    if operation == "sum":
        return (
            "from aleff import create_handler, effect\n"
            "choose = effect('choose')\nhandler = create_handler(choose)\n"
            "class Items:\n"
            "    def __iter__(self):\n"
            "        return iter((choose(), 2))\n"
            "@handler.on(choose)\n"
            "def handle(k):\n"
            "    return [k(value) for value in (1, 10)]\n"
            "result = handler(lambda: sum(Items()))\n"
            "iterator_type = type(iter(()))\n"
            'print(["<tuple_iterator>" if type(value) is iterator_type else value for value in result])\n'
        )
    return (
        "from aleff import create_handler, effect\n"
        "choose = effect('choose')\nhandler = create_handler(choose)\n"
        "class Items:\n"
        "    def __iter__(self):\n"
        "        return iter((choose(), 2))\n"
        "@handler.on(choose)\n"
        "def handle(k):\n"
        "    return [k(value) for value in (1, 10)]\n"
        f"result = handler(lambda: {operation}(Items()))\n"
        "iterator_type = type(iter(()))\n"
        'print(["<tuple_iterator>" if type(value) is iterator_type else value for value in result])\n'
    )


@pytest.mark.parametrize("operation", ["all", "any", "min", "max", "sum"])
def test_effectful_iterator_or_truthiness_resumes_per_shot(operation: str) -> None:
    source = _effect_case_source(operation)
    completed = subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    assert completed.returncode == 0, completed.stderr
    assert completed.stderr == ""
    expected = {
        "all": "[False, True]\n",
        "any": "[False, True]\n",
        "min": "[1, 2]\n",
        "max": "[2, 10]\n",
        "sum": "[3, 12]\n",
    }
    assert completed.stdout == expected[operation]


def test_sum_preserves_numeric_subclass_on_resume() -> None:
    source = """
from aleff import create_handler, effect

choose = effect("choose")
handler = create_handler(choose)

class WeirdInt(int):
    def __radd__(self, other):
        return "weird"

@handler.on(choose)
def handle(k):
    return [k(WeirdInt(1))]

print(handler(lambda: sum(map(lambda _item: choose(), (None,)))))
"""
    completed = subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    assert completed.returncode == 0, completed.stderr
    assert completed.stdout == "['weird']\n"
