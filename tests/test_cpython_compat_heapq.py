"""CPython compatibility tests for heapq public APIs."""

import sysconfig

import pytest

from cpython_compat_support import assert_cpython_compatible


IS_FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))


def test_heapq_public_surface_and_normal_mutations_match_cpython() -> None:
    assert_cpython_compatible(
        """
import heapq

print("public", sorted(heapq.__all__))
heap = [5, 1, 3, 2]
print("heapify", heapq.heapify(heap), heap)
print("push", heapq.heappush(heap, 0), heap)
print("pop", heapq.heappop(heap), heap)
print("pushpop", heapq.heappushpop(heap, 4), heap)
print("replace", heapq.heapreplace(heap, 6), heap)
print("drain", [heapq.heappop(heap) for _ in range(len(heap))])
print("empty", heapq.nlargest(3, []), heapq.nsmallest(3, []))
print("sizes", [heapq.nlargest(n, [4, 1, 3, 2]) for n in (0, 1, 2, 10)])
print("small_sizes", [heapq.nsmallest(n, [4, 1, 3, 2]) for n in (0, 1, 2, 10)])
"""
    )


def test_heapq_comparison_callbacks_and_mutation_match_cpython() -> None:
    assert_cpython_compatible(
        """
import heapq

class Item:
    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return str(self.value)

    def __lt__(self, other):
        comparisons.append((self.value, other.value))
        return self.value < other.value

def values(heap):
    return [item.value for item in heap]

for name, operation, initial in (
    ("heapify", lambda heap: heapq.heapify(heap), [3, 1, 2]),
    ("heappush", lambda heap: heapq.heappush(heap, Item(0)), [2, 4]),
    ("heappop", lambda heap: heapq.heappop(heap), [1, 2, 3]),
    ("heappushpop", lambda heap: heapq.heappushpop(heap, Item(0)), [2, 3]),
    ("heapreplace", lambda heap: heapq.heapreplace(heap, Item(4)), [1, 2]),
):
    comparisons = []
    heap = [Item(value) for value in initial]
    try:
        result = operation(heap)
    except BaseException as exc:
        result = (type(exc).__name__, str(exc))
    print(name, result, values(heap), comparisons)
"""
    )


def test_heappushpop_comparison_size_mutation_match_cpython() -> None:
    assert_cpython_compatible(
        """
import heapq

mutated = False


class Item:
    def __init__(self, value):
        self.value = value

    def __lt__(self, other):
        global mutated
        if not mutated:
            mutated = True
            heap.append(Item(9))
        return self.value < other.value


heap = [Item(1), Item(2)]
result = heapq.heappushpop(heap, Item(3))
print(result.value, [item.value for item in heap])
""".strip()
    )


def test_heappushpop_comparison_mutation_branch_matrix_match_cpython() -> None:
    assert_cpython_compatible(
        """
import heapq


def report(label, operation):
    try:
        result, heap = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", result.value, [item.value for item in heap])


def run(name, decision, mutation):
    state = {"mutated": False}

    class Item:
        def __init__(self, value):
            self.value = value

        def __lt__(self, other):
            if not state["mutated"]:
                state["mutated"] = True
                if mutation == "append":
                    heap.append(Item(9))
                elif mutation == "clear":
                    heap.clear()
                else:
                    heap[:] = [Item(4)]
                return decision
            return self.value < other.value

    is_max = name.endswith("_max")
    heap = [Item(3), Item(2)] if is_max else [Item(1), Item(2)]
    item = Item(1) if is_max else Item(3)
    result = getattr(heapq, name)(heap, item)
    return result, heap


names = ["heappushpop"]
if hasattr(heapq, "heappushpop_max"):
    names.append("heappushpop_max")

for name in names:
    for decision in (False, True):
        for mutation in ("append", "clear", "replace"):
            report(
                f"{name}-{decision}-{mutation}",
                lambda name=name, decision=decision, mutation=mutation: run(
                    name,
                    decision,
                    mutation,
                ),
            )
""".strip()
    )


def test_heappushpop_sift_comparison_size_mutation_match_cpython() -> None:
    assert_cpython_compatible(
        """
import heapq

calls = 0


class Item:
    def __init__(self, value):
        self.value = value

    def __lt__(self, other):
        global calls
        calls += 1
        if calls == 2:
            heap.append(Item(9))
        return self.value < other.value


heap = [Item(1), Item(2), Item(4)]
try:
    result = heapq.heappushpop(heap, Item(3))
except BaseException as exc:
    print("raise", type(exc).__name__, str(exc), [item.value for item in heap])
else:
    print("return", result.value, [item.value for item in heap])
""".strip()
    )


def test_heapq_merge_is_lazy_and_key_callbacks_match_cpython() -> None:
    assert_cpython_compatible(
        """
import heapq

events = []

def source(label, values):
    for value in values:
        events.append(("yield", label, value))
        yield value

def key(value):
    events.append(("key", value))
    return value[1]

merged = heapq.merge(
    source("left", [("left", 1), ("left", 4)]),
    source("right", [("right", 2), ("right", 3)]),
    key=key,
)
print("created", events)
print("first", next(merged), events)
print("second", next(merged), events)
print("rest", list(merged), events)

reverse = heapq.merge([5, 3, 1], [4, 2], reverse=True)
print("reverse", list(reverse))
"""
    )


def test_heapq_selection_key_callbacks_stability_and_corner_cases_match_cpython() -> None:
    assert_cpython_compatible(
        """
import heapq

items = [(2, "first"), (1, "low"), (2, "second"), (3, "high")]
calls = []

def key(item):
    calls.append(item[1])
    return item[0]

print("largest", heapq.nlargest(3, items, key=key), calls)
calls.clear()
print("smallest", heapq.nsmallest(3, items, key=key), calls)
print("ties", heapq.nlargest(3, [(1, "a"), (1, "b"), (1, "c")], key=lambda item: item[0]))
print("negative", heapq.nlargest(-1, [1, 2]), heapq.nsmallest(-1, [1, 2]))
"""
    )


def test_heapq_callback_errors_and_partial_mutations_match_cpython() -> None:
    assert_cpython_compatible(
        """
import heapq

def report(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", repr(value))

report("pop_empty", lambda: heapq.heappop([]))
report("replace_empty", lambda: heapq.heapreplace([], 1))
report("push_bad_heap", lambda: heapq.heappush((), 1))
report("heapify_bad_heap", lambda: heapq.heapify((2, 1)))
report("merge_bad_source", lambda: next(heapq.merge(1)))
report("largest_bad_key", lambda: heapq.nlargest(2, [1, 2], key=1))
report("smallest_bad_key", lambda: heapq.nsmallest(2, [1, 2], key=1))

class Failing:
    def __init__(self, value):
        self.value = value

    def __lt__(self, other):
        if self.value == 1:
            raise ValueError("comparison failed")
        return self.value < other.value

heap = [Failing(2), Failing(1), Failing(3)]
report("comparison", lambda: heapq.heapify(heap))
print("comparison_heap", [item.value for item in heap])

def failing_key(value):
    if value == 2:
        raise LookupError("key failed")
    return value

report("merge_key", lambda: next(heapq.merge([1], [2], key=failing_key)))
report("largest_key", lambda: heapq.nlargest(2, [1, 2, 3], key=failing_key))
report("smallest_key", lambda: heapq.nsmallest(2, [1, 2, 3], key=failing_key))
"""
    )


def test_heapq_public_max_apis_match_cpython_when_available() -> None:
    assert_cpython_compatible(
        """
import heapq

names = ("heapify_max", "heappush_max", "heappop_max", "heappushpop_max", "heapreplace_max")
print("available", [(name, hasattr(heapq, name)) for name in names])

if hasattr(heapq, "heapify_max"):
    heap = [1, 5, 3, 4, 2]
    print("heapify", heapq.heapify_max(heap), heap)
    heapq.heappush_max(heap, 6)
    print("push", heap)
    print("pop", heapq.heappop_max(heap), heap)
    print("pushpop", heapq.heappushpop_max(heap, 0), heap)
    print("replace", heapq.heapreplace_max(heap, 7), heap)
    print("drain", [heapq.heappop_max(heap) for _ in range(len(heap))])

    class Item:
        def __init__(self, value):
            self.value = value

        def __lt__(self, other):
            comparisons.append((self.value, other.value))
            return self.value < other.value

    comparisons = []
    custom = [Item(1), Item(3), Item(2)]
    heapq.heapify_max(custom)
    print("comparison", [item.value for item in custom], comparisons)
"""
    )


def test_heapq_call_signatures_errors_and_metadata_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import heapq


def report(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", value)


one_argument = {"heapify", "heappop", "heapify_max", "heappop_max"}
names = (
    "heapify", "heappush", "heappop", "heappushpop", "heapreplace",
    "heapify_max", "heappush_max", "heappop_max", "heappushpop_max",
    "heapreplace_max",
)
for name in names:
    if not hasattr(heapq, name):
        continue
    operation = getattr(heapq, name)
    print(
        "metadata",
        name,
        operation.__name__,
        operation.__module__,
        type(operation.__self__).__name__,
        operation.__text_signature__,
    )
    report(name + "_missing", lambda operation=operation: operation())
    if name in one_argument:
        report(name + "_extra", lambda operation=operation: operation([], 1))
        report(name + "_keyword", lambda operation=operation: operation(heap=[]))
    else:
        report(name + "_extra", lambda operation=operation: operation([], 1, 2))
        report(
            name + "_keyword",
            lambda operation=operation: operation(heap=[], item=1),
        )
""".strip()
    )


def test_heapq_list_subclass_uses_c_list_storage_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import heapq


class Heap(list):
    def __len__(self):
        raise AssertionError("overridden __len__ called")

    def __getitem__(self, index):
        raise AssertionError("overridden __getitem__ called")

    def __setitem__(self, index, value):
        raise AssertionError("overridden __setitem__ called")

    def append(self, value):
        raise AssertionError("overridden append called")


heap = Heap([3, 1, 2])
print("heapify", heapq.heapify(heap), list.copy(heap))
print("push", heapq.heappush(heap, 0), list.copy(heap))
print("pop", heapq.heappop(heap), list.copy(heap))
print("pushpop", heapq.heappushpop(heap, 4), list.copy(heap))
print("replace", heapq.heapreplace(heap, 5), list.copy(heap))
""".strip()
    )


@pytest.mark.skipif(
    not IS_FREE_THREADED,
    reason="requires a free-threaded CPython build",
)
def test_heapq_holds_heap_lock_during_comparison_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import heapq
import threading
import time


state = {
    "entered": False,
    "attempting": False,
    "finished": False,
    "finished_during_comparison": None,
}


class Item:
    def __init__(self, value):
        self.value = value

    def __lt__(self, other):
        if not state["entered"]:
            state["entered"] = True

            deadline = time.monotonic() + 2
            while not state["attempting"] and time.monotonic() < deadline:
                pass
            if not state["attempting"]:
                raise RuntimeError("worker did not attempt heap mutation")

            deadline = time.monotonic() + 0.2
            while not state["finished"] and time.monotonic() < deadline:
                pass
            state["finished_during_comparison"] = state["finished"]

        return self.value < other.value


heap = [Item(2), Item(4)]


def mutate_heap():
    while not state["entered"]:
        pass
    state["attempting"] = True
    heap.append(Item(9))
    state["finished"] = True


worker = threading.Thread(target=mutate_heap)
worker.start()
try:
    result = ("return", heapq.heappush(heap, Item(1)))
except BaseException as exc:
    result = ("raise", type(exc).__name__, str(exc))
worker.join(timeout=2)

print("operation", result)
print("finished_during_comparison", state["finished_during_comparison"])
print("worker", state["finished"], worker.is_alive())
print("heap", [item.value for item in heap])
""".strip(),
        timeout=15,
    )
