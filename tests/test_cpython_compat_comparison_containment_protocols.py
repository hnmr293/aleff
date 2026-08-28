"""Differential tests for rich comparison and containment protocols."""

from __future__ import annotations

from typing import Any, Callable, cast

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def test_rich_comparison_and_containment_normal_behavior() -> None:
    assert_cpython_compatible(
        r"""
import operator


class Number:
    def __init__(self, value):
        self.value = value

    def __eq__(self, other):
        return isinstance(other, Number) and self.value == other.value

    def __ne__(self, other):
        return not self == other

    def __lt__(self, other):
        return self.value < other.value

    def __le__(self, other):
        return self.value <= other.value

    def __gt__(self, other):
        return self.value > other.value

    def __ge__(self, other):
        return self.value >= other.value


one = Number(1)
two = Number(2)
print("comparisons", [one == two, one != two, one < two, one <= two, two > one, two >= one])
print("operator", [
    operator.eq(one, two),
    operator.ne(one, two),
    operator.lt(one, two),
    operator.le(one, two),
    operator.gt(two, one),
    operator.ge(two, one),
])

left = [1, 2]
right = [1, 3]
print("sequences", [
    left == right,
    left != right,
    left < right,
    left <= right,
    right > left,
    right >= left,
])
print("containment", [
    2 in left,
    9 in left,
    2 not in left,
    9 not in left,
    operator.contains(left, 2),
    operator.contains(left, 9),
])

mapping = {"one": 1, "two": 2}
print("mapping", [
    mapping == {"one": 1, "two": 2},
    mapping != {"one": 1, "two": 3},
    "one" in mapping,
    "missing" not in mapping,
    operator.eq(mapping, {"one": 1, "two": 2}),
    operator.contains(mapping, "two"),
])


class Declining:
    def __eq__(self, other):
        return NotImplemented


class Reflected:
    def __eq__(self, other):
        return "reflected-equality"


print("reflected", Declining() == Reflected(), operator.eq(Declining(), Reflected()))


class Container:
    def __contains__(self, item):
        return item == "yes"


container = Container()
print("custom contains", [
    "yes" in container,
    "no" in container,
    "yes" not in container,
    operator.contains(container, "no"),
])
"""
    )


def test_rich_comparison_and_containment_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import operator


def show(label, operation):
    try:
        print(label, "ok", repr(operation()))
    except BaseException as exc:
        print(label, "error", type(exc).__name__, str(exc))


class Exploding:
    def __eq__(self, other):
        raise ValueError("comparison failed")


show("list-equality", lambda: [Exploding()] == [object()])
show("tuple-equality", lambda: (Exploding(),) == (object(),))
show("list-contains", lambda: object() in [Exploding()])
show("tuple-contains", lambda: object() in (Exploding(),))
show("dict-equality", lambda: {"key": Exploding()} == {"key": object()})
show("operator-equality", lambda: operator.eq(Exploding(), object()))


class BadTruth:
    def __bool__(self):
        raise RuntimeError("truth failed")


class BadComparison:
    def __eq__(self, other):
        return BadTruth()


show("list-bad-truth", lambda: [BadComparison()] == [object()])
show("dict-bad-truth", lambda: {"key": BadComparison()} == {"key": object()})


class BadContainer:
    def __contains__(self, item):
        return BadTruth()


show("in-bad-truth", lambda: object() in BadContainer())
show("operator-contains-bad-truth", lambda: operator.contains(BadContainer(), object()))
show("ordering-type", lambda: object() < object())
show("operator-eq-arity", lambda: operator.eq(1))
show("operator-contains-arity", lambda: operator.contains([], 1, 2))
show("list-contains-arity", lambda: list.__contains__([1]))
show("dict-contains-arity", lambda: dict.__contains__({}))
"""
    )


def test_list_comparison_short_circuits_before_element_callbacks() -> None:
    assert_cpython_compatible(
        r"""
def show(label, operation):
    try:
        print(label, "ok", repr(operation()))
    except BaseException as exc:
        print(label, "error", type(exc).__name__, str(exc))


events = []


class Bomb:
    def __eq__(self, other):
        events.append("eq")
        raise RuntimeError("element comparison must not run")


short = [Bomb()]
long = [Bomb(), object()]
show("list-eq-short", lambda: short == long)
show("list-ne-short", lambda: short != long)
show("list-eq-long", lambda: long == short)
show("list-ne-long", lambda: long != short)
print("events", events)
"""
    )


def test_direct_dictionary_containment_respects_hashes_and_large_integer_keys() -> None:
    assert_cpython_compatible(
        r"""
def show(label, operation):
    try:
        print(label, "ok", repr(operation()))
    except BaseException as exc:
        print(label, "error", type(exc).__name__, str(exc))


calls = []


class Stored:
    def __hash__(self):
        return 1

    def __eq__(self, other):
        calls.append("stored-eq")
        raise RuntimeError("mismatched hashes must not compare")


class Query:
    def __hash__(self):
        return 2


show("mismatched-hash", lambda: dict.__contains__({Stored(): None}, Query()))
print("mismatched-hash-calls", calls)

huge = 10**100


class HugeQuery:
    def __hash__(self):
        return hash(huge)

    def __eq__(self, other):
        return other == huge


show("huge-integer-key-direct", lambda: dict.__contains__({huge: None}, HugeQuery()))
"""
    )


def test_dictionary_equality_reuses_stored_hash_and_propagates_lookup_errors() -> None:
    assert_cpython_compatible(
        r"""
def show(label, operation):
    try:
        print(label, "ok", repr(operation()))
    except BaseException as exc:
        print(label, "error", type(exc).__name__, str(exc))


class RehashingKey:
    def __init__(self):
        self.fail = False

    def __hash__(self):
        if self.fail:
            raise ValueError("key was rehashed")
        return 0


key = RehashingKey()
left = {key: 1}
right = {key: 1}
key.fail = True
show("same-key", lambda: left == right)
show("same-key-ne", lambda: left != right)


class FailingKey:
    def __hash__(self):
        return 0

    def __eq__(self, other):
        raise LookupError("key comparison failed")


show("lookup-error", lambda: {FailingKey(): 1} == {FailingKey(): 1})
show("lookup-error-ne", lambda: {FailingKey(): 1} != {FailingKey(): 1})
"""
    )


def test_operator_comparison_and_containment_metadata_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import inspect
import operator


def describe(name):
    function = getattr(operator, name)
    try:
        signature = str(inspect.signature(function))
    except BaseException as exc:
        signature = type(exc).__name__ + ":unavailable"
    return (
        name,
        function.__name__,
        repr(function.__doc__),
        repr(getattr(function, "__text_signature__", None)),
        signature,
    )


for name in ("eq", "ne", "lt", "le", "gt", "ge", "contains"):
    print(describe(name))
"""
    )


Choose = Callable[[], Any]


def _resume_outcomes(run: Callable[[Choose], Any], values: tuple[Any, ...]) -> list[Any]:
    choose = effect("comparison-choice")
    handler = create_handler(choose)

    @handler.on(choose)
    def resume(k: Any) -> list[Any]:
        return [k(value) for value in values]

    return cast(list[Any], handler(lambda: run(choose)))


def test_dictionary_equality_resumes_then_continues_remaining_entries() -> None:
    def run(choose: Choose) -> bool:
        class Value:
            def __eq__(self, other: Any) -> bool:
                return bool(choose())

        return {"first": Value(), "second": 0} == {"first": object(), "second": 1}

    assert _resume_outcomes(run, (True, False)) == [False, False]


def test_dictionary_containment_resumes_equality_without_restarting_lookup() -> None:
    calls = 0

    def run(choose: Choose) -> bool:
        class Key:
            def __hash__(self) -> int:
                return 0

            def __eq__(self, other: Any) -> bool:
                nonlocal calls
                calls += 1
                return bool(choose())

        return dict.__contains__({Key(): None}, Key())  # pyright: ignore[reportUnknownMemberType]

    assert _resume_outcomes(run, (True, False)) == [True, False]
    assert calls == 1
