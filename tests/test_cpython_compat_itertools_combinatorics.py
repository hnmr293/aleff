"""CPython compatibility regressions for combinatoric itertools."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys

from cpython_compat_support import assert_cpython_compatible


def test_itertools_normal_behavior_and_result_shapes() -> None:
    assert_cpython_compatible(
        """
import itertools

def show(name, iterator):
    values = list(iterator)
    print(name, type(iterator).__name__, values)

show("combinations", itertools.combinations("ABCD", 2))
show("combinations-r0", itertools.combinations((1, 2), 0))
show("combinations-too-large", itertools.combinations((1, 2), 3))
show("combinations-generator", itertools.combinations((x for x in (1, 2, 3)), 2))

show("cwr", itertools.combinations_with_replacement("ABC", 2))
show("cwr-r0", itertools.combinations_with_replacement((1, 2), 0))
show("cwr-too-large", itertools.combinations_with_replacement((), 1))
show("cwr-generator", itertools.combinations_with_replacement((x for x in (1, 2)), 2))

show("permutations-default", itertools.permutations((1, 2, 3)))
show("permutations-r0", itertools.permutations((1, 2), 0))
show("permutations-too-large", itertools.permutations((1, 2), 3))
show("permutations-generator", itertools.permutations((x for x in (1, 2, 3)), 2))

show("product-none", itertools.product())
show("product-empty", itertools.product((), (1, 2)))
show("product-two-pools", itertools.product((1, 2), "ab"))
show("product-generator", itertools.product((x for x in (1, 2)), (3, 4)))
show("product-repeat", itertools.product((0, 1), repeat=2))
show("product-repeat-zero", itertools.product((0, 1), repeat=0))
        """
    )


def test_itertools_argument_errors_and_index_protocol() -> None:
    assert_cpython_compatible(
        """
import itertools

def describe(label, call):
    try:
        result = call()
    except Exception as exc:
        print(label, type(exc).__name__, str(exc))
    else:
        print(label, "ok", list(result))

class BadIndex:
    def __index__(self):
        return "not an integer"

class RaisingIndex:
    def __index__(self):
        raise LookupError("index failed")

for label, call in (
    ("combinations-missing", lambda: itertools.combinations()),
    ("combinations-extra", lambda: itertools.combinations((1,), 1, 2)),
    ("combinations-bad-index", lambda: itertools.combinations((1,), BadIndex())),
    ("combinations-raising-index", lambda: itertools.combinations((1,), RaisingIndex())),
    ("combinations-negative", lambda: itertools.combinations((1,), -1)),
    ("combinations-overflow", lambda: itertools.combinations((1,), 10**100)),
    ("cwr-missing", lambda: itertools.combinations_with_replacement()),
    ("cwr-extra", lambda: itertools.combinations_with_replacement((1,), 1, 2)),
    ("cwr-bad-index", lambda: itertools.combinations_with_replacement((1,), BadIndex())),
    ("cwr-negative", lambda: itertools.combinations_with_replacement((1,), -1)),
    ("cwr-overflow", lambda: itertools.combinations_with_replacement((1,), 10**100)),
    ("permutations-missing", lambda: itertools.permutations()),
    ("permutations-extra", lambda: itertools.permutations((1,), 1, 2)),
    ("permutations-bad-index", lambda: itertools.permutations((1,), BadIndex())),
    ("permutations-negative", lambda: itertools.permutations((1,), -1)),
    ("permutations-overflow", lambda: itertools.permutations((1,), 10**100)),
    ("product-bad-repeat", lambda: itertools.product((1,), repeat=BadIndex())),
    ("product-negative-repeat", lambda: itertools.product((1,), repeat=-1)),
    ("product-overflow-repeat", lambda: itertools.product((1,), repeat=10**100)),
    ("product-unexpected-keyword", lambda: itertools.product((1,), unexpected=True)),
):
    describe(label, call)

describe("combinations-keyword-r", lambda: itertools.combinations((1, 2), r=1))
describe("cwr-keyword-r", lambda: itertools.combinations_with_replacement((1, 2), r=1))
describe("permutations-keyword-r", lambda: itertools.permutations((1, 2), r=1))
describe("permutations-duplicate-iterable", lambda: itertools.permutations((1,), iterable=(2,)))
        """
    )


def test_itertools_constructor_validation_precedes_iterable_evaluation() -> None:
    assert_cpython_compatible(
        """
import itertools

events = []

class Source:
    def __iter__(self):
        events.append("iter")
        return iter(())

def describe(label, call):
    events.clear()
    try:
        call()
    except Exception as exc:
        print(label, type(exc).__name__, str(exc), events)
    else:
        print(label, "ok", events)

describe("combinations-missing-r", lambda: itertools.combinations(Source()))
describe("cwr-missing-r", lambda: itertools.combinations_with_replacement(Source()))
describe("permutations-too-many", lambda: itertools.permutations(Source(), 1, 2))
describe("product-invalid-keyword", lambda: itertools.product(Source(), unexpected=True))
        """
    )


def test_itertools_constructors_do_not_preconvert_iterables() -> None:
    assert_cpython_compatible(
        """
import itertools

class First:
    def __init__(self):
        self.done = False

    def __iter__(self):
        return iter((2,))

    def __next__(self):
        if self.done:
            raise StopIteration
        self.done = True
        return 1

class Source:
    def __iter__(self):
        return First()

def describe(label, call):
    try:
        print(label, list(call()))
    except Exception as exc:
        print(label, type(exc).__name__, str(exc))

describe("combinations", lambda: itertools.combinations(Source(), 1))
describe("cwr", lambda: itertools.combinations_with_replacement(Source(), 1))
describe("permutations", lambda: itertools.permutations(Source(), 1))
describe("product", lambda: itertools.product(Source(), (3,)))
        """
    )


def test_product_repeat_zero_does_not_iterate_inputs() -> None:
    assert_cpython_compatible(
        """
import itertools

class Bomb:
    def __iter__(self):
        raise RuntimeError("input must not be iterated")

try:
    print(list(itertools.product(Bomb(), repeat=0)))
except Exception as exc:
    print(type(exc).__name__, str(exc))
        """
    )


def test_itertools_constructors_preserve_exact_tuple_inputs() -> None:
    assert_cpython_compatible(
        """
import gc
import itertools

def contains_direct_or_one_tuple_level(root, target):
    for reference in gc.get_referents(root):
        if reference is target:
            return True
        if type(reference) is tuple and any(item is target for item in gc.get_referents(reference)):
            return True
    return False

data = (object(),)
objects = (
    ("combinations", itertools.combinations(data, 1)),
    ("cwr", itertools.combinations_with_replacement(data, 1)),
    ("permutations", itertools.permutations(data, 1)),
    ("product", itertools.product(data)),
)
for name, iterator in objects:
    print(name, contains_direct_or_one_tuple_level(iterator, data))
        """
    )


def test_itertools_subclasses_are_constructible_and_preserve_their_type() -> None:
    assert_cpython_compatible(
        """
import itertools

class Combinations(itertools.combinations):
    pass

class CWR(itertools.combinations_with_replacement):
    pass

class Permutations(itertools.permutations):
    pass

class Product(itertools.product):
    pass

def describe(label, call):
    try:
        iterator = call()
    except Exception as exc:
        print(label, type(exc).__name__, str(exc))
    else:
        print(label, type(iterator).__name__, list(iterator))

describe("combinations", lambda: Combinations((1, 2), 1))
describe("cwr", lambda: CWR((1, 2), 1))
describe("permutations", lambda: Permutations((1, 2), 1))
describe("product", lambda: Product((1, 2), repeat=2))
        """
    )


def test_index_continuations_resume_as_index_values() -> None:
    source = """
import itertools
from aleff import create_handler, effect

def combination_case():
    choose = effect("combination-index")
    handler = create_handler(choose)

    class R:
        def __index__(self):
            choose()
            return 2

    @handler.on(choose)
    def resume(k):
        try:
            return ("ok", k(2))
        except Exception as exc:
            return ("raise", type(exc).__name__)

    return handler(lambda: list(itertools.combinations((1, 2, 3), R())))

def product_case():
    choose = effect("product-repeat")
    handler = create_handler(choose)

    class Repeat:
        def __index__(self):
            choose()
            return 2

    @handler.on(choose)
    def resume(k):
        try:
            return ("ok", k(2))
        except Exception as exc:
            return ("raise", type(exc).__name__)

    return handler(lambda: list(itertools.product((1, 2), repeat=Repeat())))

print(combination_case())
print(product_case())
"""
    completed = subprocess.run(
        [sys.executable, "-X", "faulthandler", "-c", source],
        cwd=Path(__file__).resolve().parents[1],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    assert completed.returncode == 0, f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
    assert completed.stderr == ""
    assert completed.stdout == ("('ok', [(1, 2), (1, 3), (2, 3)])\n('ok', [(1, 1), (1, 2), (2, 1), (2, 2)])\n")
