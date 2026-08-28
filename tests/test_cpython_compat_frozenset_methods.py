"""CPython compatibility tests for frozenset methods and operators."""

from __future__ import annotations

import subprocess
import sys

from cpython_compat_support import assert_cpython_compatible


def test_frozenset_normal_methods_and_operators_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def show(value):
    if isinstance(value, (set, frozenset)):
        return (type(value).__name__, tuple(sorted(value)))
    return (type(value).__name__, value)

f = frozenset((1, 2, 3))
cases = (
    ("constructor_empty", frozenset()),
    ("constructor_generator", frozenset(value * 2 for value in range(3))),
    ("constructor_set", frozenset({2, 4, 6})),
    ("union", f.union((3, 4), [5, 6])),
    ("intersection", f.intersection((0, 2, 4), [2, 3])),
    ("difference", f.difference((2,), [3])),
    ("symmetric_difference", f.symmetric_difference((3, 4))),
    ("isdisjoint_true", f.isdisjoint((4, 5))),
    ("isdisjoint_false", f.isdisjoint((3, 5))),
    ("issubset_true", frozenset((1, 2)).issubset((0, 1, 2, 3))),
    ("issubset_false", frozenset((1, 4)).issubset((0, 1, 2, 3))),
    ("issuperset_true", f.issuperset((1, 3))),
    ("issuperset_false", f.issuperset((1, 4))),
    ("operator_or", f | frozenset((3, 4))),
    ("operator_and", f & frozenset((2, 4))),
    ("operator_sub", f - frozenset((2, 4))),
    ("operator_xor", f ^ frozenset((3, 4))),
    ("reverse_operator_or", {4, 5} | f),
    ("reverse_operator_and", {2, 4} & f),
    ("reverse_operator_sub", {2, 4} - f),
    ("reverse_operator_xor", {3, 4} ^ f),
)
for name, value in cases:
    print(name, show(value))
"""
    )


def test_frozenset_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def report(name, operation):
    try:
        result = operation()
    except Exception as exc:
        print(name, type(exc).__name__, str(exc))
    else:
        print(name, "return", type(result).__name__)

f = frozenset((1, 2))
report("constructor_non_iterable", lambda: frozenset(42))
report("constructor_unhashable", lambda: frozenset(([1],)))
report("union_non_iterable", lambda: f.union(42))
report("union_unhashable", lambda: f.union(([1],)))
report("intersection_non_iterable", lambda: f.intersection(42))
report("difference_non_iterable", lambda: f.difference(42))
report("symmetric_difference_non_iterable", lambda: f.symmetric_difference(42))
report("isdisjoint_non_iterable", lambda: f.isdisjoint(42))
report("issubset_non_iterable", lambda: f.issubset(42))
report("issuperset_non_iterable", lambda: f.issuperset(42))
report("symmetric_difference_missing", lambda: f.symmetric_difference())
report("symmetric_difference_extra", lambda: f.symmetric_difference((3,), (4,)))
report("isdisjoint_missing", lambda: f.isdisjoint())
report("isdisjoint_extra", lambda: f.isdisjoint((1,), (2,)))
report("issubset_missing", lambda: f.issubset())
report("issuperset_extra", lambda: f.issuperset((1,), (2,)))
report("union_keyword", lambda: f.union(other=(3,)))
report("operator_or_iterable", lambda: f | [3])
report("operator_and_iterable", lambda: f & [3])
report("operator_sub_iterable", lambda: f - [3])
report("operator_xor_iterable", lambda: f ^ [3])
"""
    )


def test_frozenset_hashing_stops_before_a_bad_iterator_tail() -> None:
    assert_cpython_compatible(
        r"""
class BadTail:
    def __iter__(self):
        yield []
        raise RuntimeError("tail reached")

def report(name, operation):
    try:
        operation()
    except Exception as exc:
        print(name, type(exc).__name__)
    else:
        print(name, "return")

f = frozenset((1,))
report("constructor", lambda: frozenset(BadTail()))
report("union", lambda: f.union(BadTail()))
report("intersection", lambda: f.intersection(BadTail()))
report("difference", lambda: f.difference(BadTail()))
report("symmetric_difference", lambda: f.symmetric_difference(BadTail()))
report("isdisjoint", lambda: f.isdisjoint(BadTail()))
report("issubset", lambda: f.issubset(BadTail()))
report("issuperset", lambda: f.issuperset(BadTail()))
"""
    )


def test_frozenset_hashing_can_affect_what_a_generator_yields() -> None:
    assert_cpython_compatible(
        r"""
state = {"hashed": False}

class Key:
    def __hash__(self):
        state["hashed"] = True
        return 1

class Iterable:
    def __iter__(self):
        yield Key()
        if state["hashed"]:
            yield 2

def show(value):
    return tuple(sorted((type(item).__name__ for item in value)))

state["hashed"] = False
print("constructor", show(frozenset(Iterable())))
state["hashed"] = False
print("union", show(frozenset().union(Iterable())))
"""
    )


def test_frozenset_set_like_subclasses_do_not_use_overridden_iter() -> None:
    assert_cpython_compatible(
        r"""
class NoIter(frozenset):
    def __iter__(self):
        raise RuntimeError("iter called")

operand = NoIter((1, 2))
receiver = frozenset((1, 2, 3))

def report(name, operation):
    try:
        result = operation()
    except Exception as exc:
        print(name, type(exc).__name__)
    else:
        if isinstance(result, (set, frozenset)):
            print(name, type(result).__name__, len(result))
        else:
            print(name, type(result).__name__, result)

report("constructor", lambda: frozenset(operand))
report("union", lambda: receiver.union(operand))
report("intersection", lambda: receiver.intersection(operand))
report("difference", lambda: receiver.difference(operand))
report("symmetric_difference", lambda: receiver.symmetric_difference(operand))
report("isdisjoint", lambda: receiver.isdisjoint(operand))
report("issubset", lambda: receiver.issubset(operand))
report("issuperset", lambda: receiver.issuperset(operand))
report("identity_isdisjoint", lambda: operand.isdisjoint(operand))
"""
    )


def test_frozenset_intersection_preserves_iterable_representative() -> None:
    assert_cpython_compatible(
        r"""
class Key:
    def __init__(self, name, value):
        self.name = name
        self.value = value

    def __hash__(self):
        return self.value

    def __eq__(self, other):
        return isinstance(other, Key) and self.value == other.value

receiver = Key("receiver", 1)
iterated = Key("iterated", 1)
extra = Key("extra", 2)
result = frozenset((receiver,)).intersection((iterated, extra))
print(tuple(item.name for item in result))
"""
    )


def test_frozenset_does_not_rehash_exact_set_like_operands() -> None:
    assert_cpython_compatible(
        r"""
class Key:
    calls = 0

    def __hash__(self):
        type(self).calls += 1
        return 1

    def __eq__(self, other):
        return self is other

def measure(name, operation):
    key = Key()
    operand = frozenset((key,))
    Key.calls = 0
    operation(operand)
    print(name, Key.calls)

receiver = frozenset((0,))
measure("union", lambda operand: receiver.union(operand))
measure("intersection", lambda operand: receiver.intersection(operand))
measure("difference", lambda operand: receiver.difference(operand))
measure("symmetric_difference", lambda operand: receiver.symmetric_difference(operand))
measure("isdisjoint", lambda operand: receiver.isdisjoint(operand))
measure("issubset", lambda operand: receiver.issubset(operand))
measure("issuperset", lambda operand: receiver.issuperset(operand))

key = Key()
operand = {key}
Key.calls = 0
frozenset(operand)
print("constructor_exact_set", Key.calls)
"""
    )


def test_frozenset_zero_operand_methods_return_new_objects() -> None:
    assert_cpython_compatible(
        r"""
f = frozenset((1, 2))
print("constructor_identity", frozenset(f) is f)
print("union_identity", f.union() is f)
print("intersection_identity", f.intersection() is f)
print("difference_identity", f.difference() is f)
"""
    )


def test_frozenset_methods_do_not_dispatch_subclass_binary_operators() -> None:
    assert_cpython_compatible(
        r"""
class F(frozenset):
    def __or__(self, other):
        return "or override"

    def __and__(self, other):
        return "and override"

    def __sub__(self, other):
        return "sub override"

    def __xor__(self, other):
        return "xor override"

f = F((1,))
for name, operation in (
    ("union", lambda: f.union((2,))),
    ("intersection", lambda: f.intersection((1,))),
    ("difference", lambda: f.difference((2,))),
    ("symmetric_difference", lambda: f.symmetric_difference((2,))),
):
    try:
        value = operation()
    except Exception as exc:
        print(name, type(exc).__name__)
    else:
        if isinstance(value, (set, frozenset)):
            print(name, type(value).__name__, tuple(sorted(value)))
        else:
            print(name, type(value).__name__, value)
"""
    )


def test_frozenset_hash_resume_does_not_corrupt_the_operation() -> None:
    source = r"""
from aleff import create_handler, effect

choose = effect("choose")
handler = create_handler(choose)

class Key:
    def __hash__(self):
        return choose()

@handler.on(choose)
def handle(resume):
    return [resume(1), resume(10)]

def run():
    result = frozenset().union((Key(),))
    return len(result), tuple(sorted(type(value).__name__ for value in result))

print(handler(run))
"""
    result = subprocess.run(
        [sys.executable, "-c", source],
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "[(1, ('Key',)), (1, ('Key',))]"
