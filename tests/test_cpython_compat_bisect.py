"""CPython compatibility regressions for the bisect C accelerators."""

from cpython_compat_support import assert_cpython_compatible


def test_bisect_all_apis_duplicates_bounds_empty_and_mutation_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import bisect


values = [1, 2, 2, 4]
for name in ("bisect_left", "bisect_right", "bisect", "insort_left", "insort_right", "insort"):
    operation = getattr(bisect, name)
    if name.startswith("insort"):
        current = values.copy()
        result = operation(current, 2)
        print(name, "return", result, current)
    else:
        print(name, "return", operation(values, 2))

print("aliases", bisect.bisect is bisect.bisect_right, bisect.insort is bisect.insort_right)
print("bounds", bisect.bisect_left(values, 2, 1, 3), bisect.bisect_right(values, 2, 1, 3))
print("empty", bisect.bisect_left([], 1), bisect.bisect_right([], 1))
for operation in (bisect.insort_left, bisect.insort_right, bisect.insort):
    current = []
    print("empty_insert", operation.__name__, operation(current, 1), current)
""".strip()
    )


def test_bisect_key_semantics_and_callback_order_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import bisect


class Item:
    def __init__(self, name, value):
        self.name = name
        self.value = value


def exercise(name):
    events = []
    entry = Item("entry", 1)
    probe = Item("probe", 2)

    def key(value):
        events.append(value.name)
        return value.value

    operation = getattr(bisect, name)
    if name.startswith("insort"):
        values = [entry]
        result = operation(values, probe, key=key)
        print(name, result, [item.name for item in values], events)
    else:
        result = operation([entry], 2, key=key)
        print(name, result, events)


for name in ("bisect_left", "bisect_right", "bisect", "insort_left", "insort_right", "insort"):
    exercise(name)
""".strip()
    )


def test_bisect_comparison_direction_and_truthiness_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import bisect


events = []


class Entry:
    def __lt__(self, other):
        events.append(("entry", type(other).__name__))
        return False


class Probe:
    def __lt__(self, other):
        events.append(("probe", type(other).__name__))
        return True


entry = Entry()
probe = Probe()
print("left", bisect.bisect_left([entry], probe), events)
events.clear()
print("right", bisect.bisect_right([entry], probe), events)


class Truth:
    def __bool__(self):
        return True


class TruthEntry:
    def __lt__(self, other):
        return Truth()


print("truth", bisect.bisect_left([TruthEntry()], object()))
""".strip()
    )


def test_bisect_callback_exceptions_argument_errors_and_list_preservation_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import bisect


def report(label, callback):
    try:
        value = callback()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", value)


class ComparisonError(Exception):
    pass


class Entry:
    def __lt__(self, other):
        raise ComparisonError("comparison failed")


class Probe:
    def __lt__(self, other):
        raise ComparisonError("comparison failed")


for name in ("bisect_left", "bisect_right", "bisect", "insort_left", "insort_right", "insort"):
    operation = getattr(bisect, name)
    values = [Entry()]
    if name.startswith("insort"):
        report(name + "_comparison", lambda operation=operation, values=values: operation(values, Probe()))
        print(name + "_comparison_values", len(values))
    elif name.endswith("_left"):
        report(name + "_comparison", lambda operation=operation, values=values: operation(values, Probe()))
    else:
        report(name + "_comparison", lambda operation=operation, values=values: operation(values, Probe()))


class KeyErrorFromCallback(Exception):
    pass


def bad_key(value):
    raise KeyErrorFromCallback("key failed")


for name in ("bisect_left", "bisect_right", "bisect", "insort_left", "insort_right", "insort"):
    operation = getattr(bisect, name)
    values = [1]
    if name.startswith("insort"):
        report(name + "_key", lambda operation=operation, values=values: operation(values, 0, key=bad_key))
        print(name + "_key_values", values)
    else:
        report(name + "_key", lambda operation=operation: operation([1], 0, key=bad_key))


report("negative_lo", lambda: bisect.bisect_left([1], 1, -1))
report("non_integer_lo", lambda: bisect.bisect_right([1], 1, "0"))
report("missing", lambda: bisect.bisect_left())
report("too_many", lambda: bisect.insort([], 1, 0, 1, 2))
report("unknown_keyword", lambda: bisect.bisect([], 1, unknown=True))
""".strip()
    )


def test_bisect_recursive_comparison_match_cpython() -> None:
    assert_cpython_compatible(
        """
import bisect
import sys

sys.setrecursionlimit(80)
calls = 0


class Item:
    def __lt__(self, other):
        global calls
        calls += 1
        return bisect.bisect_left([self], other) == 0


try:
    bisect.bisect_left([Item()], Item())
except BaseException as exc:
    print(type(exc).__name__, str(exc), calls)
""".strip()
    )
