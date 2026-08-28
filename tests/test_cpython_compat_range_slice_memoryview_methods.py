"""Differential compatibility tests for range, slice, and memoryview."""

from cpython_compat_support import assert_cpython_compatible


def test_tuple_count_and_index_normal_errors_and_corner_cases() -> None:
    assert_cpython_compatible(
        r"""
def record(label, operation):
    try:
        value = operation()
    except Exception as exc:
        print((label, "error", type(exc).__name__, str(exc)))
    else:
        print((label, "return", type(value).__name__, value))

items = (0, 1, 2, 1, True)
record("count_duplicate_and_bool", lambda: items.count(1))
record("count_empty", lambda: ().count(None))
record("index_first", lambda: items.index(1))
record("index_start", lambda: items.index(1, 2))
record("index_stop", lambda: items.index(1, 0, 3))
record("index_negative_bounds", lambda: items.index(True, -2, -1))
record("index_missing", lambda: items.index(99))
record("index_invalid_start", lambda: items.index(1, "start"))

class Needle:
    def __init__(self):
        self.comparisons = []

    def __eq__(self, other):
        self.comparisons.append(other)
        return other == 2

needle = Needle()
record("count_custom_equality", lambda: (0, 1, 2, 3).count(needle))
print(("custom_equality_comparisons", needle.comparisons))
"""
    )


def test_range_count_and_index_accept_cpython_targets() -> None:
    assert_cpython_compatible(
        r"""
def record(label, operation):
    try:
        value = operation()
    except Exception as exc:
        print((label, "error", type(exc).__name__, str(exc)))
    else:
        print((label, "return", type(value).__name__, value))

values = range(0, 10, 2)
record("count_int", lambda: values.count(4))
record("count_float_equal", lambda: values.count(4.0))
record("count_bool_equal", lambda: values.count(True))
record("count_float_missing", lambda: values.count(3.0))
record("index_int", lambda: values.index(6))
record("index_float_equal", lambda: values.index(6.0))
record("index_missing_int", lambda: values.index(7))
record("index_missing_bool", lambda: values.index(True))
record("index_missing_float", lambda: values.index(7.0))

class Needle:
    def __init__(self):
        self.comparisons = []

    def __eq__(self, other):
        self.comparisons.append(other)
        return other == 4

needle = Needle()
record("count_custom_equality", lambda: values.count(needle))
print(("custom_equality_comparisons", needle.comparisons))

class MissingNeedle:
    def __eq__(self, other):
        return False

record("index_missing_custom", lambda: values.index(MissingNeedle()))

large_start = 10**100
large_values = range(large_start, large_start + 8, 2)
record("large_integer_count", lambda: large_values.count(large_start + 4))
record("large_integer_index", lambda: large_values.index(large_start + 6))
"""
    )


def test_slice_indices_normal_errors_and_integer_corner_cases() -> None:
    assert_cpython_compatible(
        r"""
def record(label, operation):
    try:
        value = operation()
    except Exception as exc:
        print((label, "error", type(exc).__name__, str(exc)))
    else:
        print((label, "return", type(value).__name__, value))

record("ordinary_positive", lambda: slice(1, 8, 2).indices(10))
record("ordinary_negative", lambda: slice(None, None, -2).indices(7))
record("clamped_bounds", lambda: slice(-100, 100, 3).indices(5))
record("none_components", lambda: slice(None).indices(5))
record("zero_step", lambda: slice(1, 8, 0).indices(10))
record("negative_length", lambda: slice(None).indices(-1))
record("huge_length", lambda: slice(None).indices(2**100))
record("invalid_length", lambda: slice(None).indices("length"))
record("invalid_component", lambda: slice("start", 5, 1).indices(10))
"""
    )


def test_slice_indices_converts_components_in_cpython_order() -> None:
    assert_cpython_compatible(
        r"""
class Indexable:
    def __init__(self, name, log):
        self.name = name
        self.log = log

    def __index__(self):
        self.log.append(self.name)
        return 1

log = []
result = slice(
    Indexable("start", log),
    Indexable("stop", log),
    Indexable("step", log),
).indices(5)
print((log, result))
"""
    )


def test_slice_hash_normal_errors_and_corner_cases() -> None:
    assert_cpython_compatible(
        r"""
def record(label, operation):
    try:
        value = operation()
    except Exception as exc:
        print((label, "error", type(exc).__name__, str(exc)))
    else:
        print((label, "return", type(value).__name__, value))

record("hash_none", lambda: hash(slice(None)))
record("hash_components", lambda: hash(slice(1, 10, 2)))
record("hash_unhashable_component", lambda: hash(slice([], None, None)))
"""
    )
