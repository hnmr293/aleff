"""CPython compatibility coverage for set methods and operators (Issue #55)."""

from __future__ import annotations

from typing import Any, Callable, cast

import pytest

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def test_set_methods_and_operators_normal_behavior() -> None:
    assert_cpython_compatible(
        r"""
def stable(value):
    if isinstance(value, (set, frozenset)):
        return (type(value).__name__, tuple(sorted(value)))
    return value

def show(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__, str(exc))
    else:
        print(name, "return", stable(value))

show("set.update", lambda: (lambda s: (s.update([2, 4], (5,)), stable(s)))(set([1, 2, 3])))
show(
    "set.intersection_update",
    lambda: (lambda s: (s.intersection_update([2, 3, 4], (3, 5)), stable(s)))(set([1, 2, 3])),
)
show("set.difference_update", lambda: (lambda s: (s.difference_update([2], (3, 5)), stable(s)))(set([1, 2, 3])))
show(
    "set.symmetric_difference_update",
    lambda: (lambda s: (s.symmetric_difference_update([2, 4]), stable(s)))(set([1, 2, 3])),
)

show("set.union", lambda: set([1, 2]).union([2, 3], (4,)))
show("set.intersection", lambda: set([1, 2, 3]).intersection([2, 3, 4], (2, 5)))
show("set.difference", lambda: set([1, 2, 3]).difference([2], (3, 5)))
show("set.symmetric_difference", lambda: set([1, 2]).symmetric_difference([2, 3]))
show("set.isdisjoint.false", lambda: set([1, 2]).isdisjoint([3, 2]))
show("set.isdisjoint.true", lambda: set([1, 2]).isdisjoint([3, 4]))
show("set.issubset", lambda: set([1, 2]).issubset([0, 1, 2, 3]))
show("set.issuperset", lambda: set([1, 2]).issuperset([1]))

show("frozenset.union", lambda: frozenset([1, 2]).union([2, 3], (4,)))
show("frozenset.intersection", lambda: frozenset([1, 2, 3]).intersection([2, 3, 4], (2, 5)))
show("frozenset.difference", lambda: frozenset([1, 2, 3]).difference([2], (3, 5)))
show("frozenset.symmetric_difference", lambda: frozenset([1, 2]).symmetric_difference([2, 3]))
show("frozenset.isdisjoint", lambda: frozenset([1, 2]).isdisjoint([3, 4]))
show("frozenset.issubset", lambda: frozenset([1, 2]).issubset([0, 1, 2, 3]))
show("frozenset.issuperset", lambda: frozenset([1, 2]).issuperset([1]))

show("set.|", lambda: set([1, 2]) | frozenset([2, 3]))
show("set.&", lambda: set([1, 2]) & frozenset([2, 3]))
show("set.-", lambda: set([1, 2]) - frozenset([2, 3]))
show("set.^", lambda: set([1, 2]) ^ frozenset([2, 3]))
show("frozenset.|", lambda: frozenset([1, 2]) | set([2, 3]))
show("frozenset.&", lambda: frozenset([1, 2]) & set([2, 3]))
show("frozenset.-", lambda: frozenset([1, 2]) - set([2, 3]))
show("frozenset.^", lambda: frozenset([1, 2]) ^ set([2, 3]))

def inplace(symbol):
    namespace = {"value": set([1, 2]), "other": frozenset([2, 3])}
    exec("value " + symbol + "= other", namespace)
    return stable(namespace["value"])

for symbol in ("|", "&", "-", "^"):
    show("set." + symbol + "=", lambda symbol=symbol: inplace(symbol))
"""
    )


def test_set_methods_and_operators_errors() -> None:
    assert_cpython_compatible(
        r"""
def show(name, operation):
    try:
        operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__)
    else:
        print(name, "return")

show("update.unhashable", lambda: set().update([[]]))
show("intersection_update.unhashable", lambda: set([1]).intersection_update([[1]]))
show("difference_update.unhashable", lambda: set([1]).difference_update([[1]]))
show("symmetric_difference_update.unhashable", lambda: set([1]).symmetric_difference_update([[1]]))
show("union.unhashable", lambda: set([1]).union([[1]]))
show("intersection.unhashable", lambda: set([1]).intersection([[1]]))
show("difference.unhashable", lambda: set([1]).difference([[1]]))
show("symmetric_difference.unhashable", lambda: set([1]).symmetric_difference([[1]]))
show("isdisjoint.unhashable", lambda: set([1]).isdisjoint([[1]]))
show("issubset.unhashable", lambda: set([1]).issubset([[1]]))
show("issuperset.unhashable", lambda: set([1]).issuperset([[1]]))

show("union.not_iterable", lambda: set([1]).union(2))
show("intersection.not_iterable", lambda: set([1]).intersection(2))
show("difference.not_iterable", lambda: set([1]).difference(2))
show("symmetric_difference.not_iterable", lambda: set([1]).symmetric_difference(2))
show("isdisjoint.not_iterable", lambda: set([1]).isdisjoint(2))
show("issubset.not_iterable", lambda: set([1]).issubset(2))
show("issuperset.not_iterable", lambda: set([1]).issuperset(2))

show("symmetric_difference.missing", lambda: set([1]).symmetric_difference())
show("symmetric_difference.extra", lambda: set([1]).symmetric_difference([2], [3]))
show("symmetric_difference_update.missing", lambda: set([1]).symmetric_difference_update())
show("symmetric_difference_update.extra", lambda: set([1]).symmetric_difference_update([2], [3]))
show("isdisjoint.missing", lambda: set([1]).isdisjoint())
show("issubset.extra", lambda: set([1]).issubset([1], [2]))

show("operator.list", lambda: set([1]) | [2])
show("operator.unhashable", lambda: set([1]) | set([[]]))
show("descriptor.bad_receiver", lambda: set.union([], [1]))
"""
    )


def test_set_methods_ignore_overridden_set_operators() -> None:
    assert_cpython_compatible(
        r"""
class SetSubclass(set):
    def __or__(self, other):
        raise RuntimeError("__or__ was called")

    def __and__(self, other):
        raise RuntimeError("__and__ was called")

    def __sub__(self, other):
        raise RuntimeError("__sub__ was called")

    def __xor__(self, other):
        raise RuntimeError("__xor__ was called")

    __ior__ = __or__
    __iand__ = __and__
    __isub__ = __sub__
    __ixor__ = __xor__

class FrozenSubclass(frozenset):
    __or__ = SetSubclass.__or__
    __and__ = SetSubclass.__and__
    __sub__ = SetSubclass.__sub__
    __xor__ = SetSubclass.__xor__

def show(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__, str(exc))
    else:
        if isinstance(value, (set, frozenset)):
            value = (type(value).__name__, tuple(sorted(value)))
        print(name, "return", value)

show("set.update", lambda: SetSubclass().update([1]))
show("set.intersection_update", lambda: SetSubclass([1, 2]).intersection_update([2]))
show("set.difference_update", lambda: SetSubclass([1, 2]).difference_update([2]))
show("set.symmetric_difference_update", lambda: SetSubclass([1, 2]).symmetric_difference_update([2]))
show("set.union", lambda: SetSubclass([1]).union([2]))
show("set.intersection", lambda: SetSubclass([1, 2]).intersection([2]))
show("set.difference", lambda: SetSubclass([1, 2]).difference([2]))
show("set.symmetric_difference", lambda: SetSubclass([1]).symmetric_difference([2]))
show("frozenset.union", lambda: FrozenSubclass([1]).union([2]))
show("frozenset.intersection", lambda: FrozenSubclass([1, 2]).intersection([2]))
show("frozenset.difference", lambda: FrozenSubclass([1, 2]).difference([2]))
show("frozenset.symmetric_difference", lambda: FrozenSubclass([1]).symmetric_difference([2]))
"""
    )


def test_set_update_is_incremental() -> None:
    assert_cpython_compatible(
        r"""
observed = []
target = set()

def source():
    yield 1
    observed.append(tuple(sorted(target)))
    yield 2

target.update(source())
print("update.visibility", observed, tuple(sorted(target)))
"""
    )


def test_set_isdisjoint_short_circuits() -> None:
    assert_cpython_compatible(
        r"""
def stops_after_answer():
    yield 1
    raise RuntimeError("iterator consumed after answer")

try:
    result = {1}.isdisjoint(stops_after_answer())
except Exception as exc:
    print("isdisjoint.short_circuit", "raise", type(exc).__name__, str(exc))
else:
    print("isdisjoint.short_circuit", "return", result)
"""
    )


def _resume_outcomes(run: Callable[[Any], Any]) -> list[Any]:
    choose = effect("set-operation-choice")
    handler = create_handler(choose)

    @handler.on(choose)
    def resume(k: Any) -> list[Any]:
        outcomes: list[Any] = []
        for value in (1, 10):
            try:
                outcomes.append(k(value))
            except Exception as exc:
                outcomes.append(type(exc).__name__)
        return outcomes

    return cast(list[Any], handler(lambda: run(choose)))


@pytest.mark.parametrize(
    ("container_type", "operation", "expected_size"),
    [
        (set, "union", 2),
        (set, "intersection", 0),
        (set, "difference", 1),
        (set, "symmetric_difference", 2),
        (frozenset, "union", 2),
        (frozenset, "intersection", 0),
        (frozenset, "difference", 1),
        (frozenset, "symmetric_difference", 2),
    ],
)
def test_nonmutating_set_operation_resume_preserves_operation_state(
    container_type: type[set[Any]] | type[frozenset[Any]], operation: str, expected_size: int
) -> None:
    def run(choose: Any) -> Any:
        class Key:
            def __hash__(self) -> int:
                return cast(int, choose())

        target: Any = container_type([1])
        result = getattr(target, operation)([Key()])
        return type(result).__name__, len(result)

    result_type = "set" if container_type is set else "frozenset"
    expected = [(result_type, expected_size), (result_type, expected_size)]
    assert _resume_outcomes(run) == expected
