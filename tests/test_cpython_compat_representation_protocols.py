"""CPython compatibility tests for core representation and hash protocols."""

from cpython_compat_support import assert_cpython_compatible


def test_str_repr_builtin_format_and_hash_normal_behavior() -> None:
    assert_cpython_compatible(
        r"""
class Value:
    def __str__(self):
        return "string-value"

    def __repr__(self):
        return "repr-value"

    def __format__(self, spec):
        return "formatted:%s:%s" % (spec, type(spec).__name__)

    def __hash__(self):
        return 123456


value = Value()
print(str(value))
print(repr(value))
print(format(value, "spec"))
print(format(value))
print(hash(value), type(hash(value)).__name__)
print(str(b"bytes", "ascii"))
"""
    )


def test_representation_and_hash_invalid_results_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def describe(label, operation):
    try:
        operation()
    except BaseException as exc:
        print(label, type(exc).__name__, str(exc))
    else:
        print(label, "returned")


class BadStr:
    def __str__(self):
        return object()


class BadRepr:
    def __repr__(self):
        return object()


class BadFormat:
    def __format__(self, spec):
        return object()


class BadHash:
    def __hash__(self):
        return object()


describe("str", lambda: str(BadStr()))
describe("repr", lambda: repr(BadRepr()))
describe("format", lambda: format(BadFormat(), "spec"))
describe("hash", lambda: hash(BadHash()))
"""
    )


def test_builtin_argument_errors_and_signature_metadata_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import inspect


def describe(label, operation):
    try:
        operation()
    except BaseException as exc:
        print(label, type(exc).__name__, str(exc))
    else:
        print(label, "returned")


describe("str-too-many", lambda: str("value", "ascii", "strict"))
describe("repr-too-many", lambda: repr("value", "extra"))
describe("format-too-few", lambda: format())
describe("format-too-many", lambda: format("value", "spec", "extra"))
describe("hash-too-many", lambda: hash("value", "extra"))

for name, function in (("str", str), ("repr", repr), ("format", format), ("hash", hash)):
    try:
        signature = str(inspect.signature(function))
    except BaseException as exc:
        signature = "%s:%s" % (type(exc).__name__, str(exc))
    print(name, signature)
"""
    )


def test_unicode_subclasses_and_hash_corner_cases_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
class Text(str):
    pass


class UnicodeResults:
    def __str__(self):
        return Text("str-subclass")

    def __repr__(self):
        return Text("repr-subclass")

    def __format__(self, spec):
        return Text("format-subclass:" + spec)


class MinusOne:
    def __hash__(self):
        return -1


class Huge:
    def __hash__(self):
        return 2**100


class BoolHash:
    def __hash__(self):
        return True


class Unhashable:
    __hash__ = None


value = UnicodeResults()
print(type(str(value)).__name__, str(value))
print(type(repr(value)).__name__, repr(value))
print(type(format(value, "x")).__name__, format(value, "x"))
print(hash(MinusOne()))
print(hash(Huge()), type(hash(Huge())).__name__)
print(hash(BoolHash()), type(hash(BoolHash())).__name__)
try:
    hash(Unhashable())
except BaseException as exc:
    print(type(exc).__name__, str(exc))
"""
    )


def test_nested_c_protocol_calls_resume_with_the_outer_result() -> None:
    assert_cpython_compatible(
        r"""
import sys


def run_shots(make_result, choices):
    if "aleff" in sys.modules:
        from aleff import create_handler, effect

        choose = effect("protocol-choice")
        handler = create_handler(choose)

        @handler.on(choose)
        def resume(k):
            results = []
            for choice in choices:
                try:
                    results.append(("return", k(choice)))
                except BaseException as exc:
                    results.append(("raise", type(exc).__name__, str(exc)))
            return results

        return handler(lambda: make_result(choose))

    results = []
    for choice in choices:
        def choose(choice=choice):
            return choice

        try:
            results.append(("return", make_result(choose)))
        except BaseException as exc:
            results.append(("raise", type(exc).__name__, str(exc)))
    return results


def list_repr(choose):
    class Value:
        def __repr__(self):
            return choose()

    return repr([Value()])


def list_str(choose):
    class Value:
        def __repr__(self):
            return choose()

    return str([Value()])


def tuple_hash(choose):
    class Value:
        def __hash__(self):
            return choose()

    return hash((Value(),))


print("repr", run_shots(list_repr, ("first", "second")))
print("str", run_shots(list_str, ("first", "second")))
print("hash", run_shots(tuple_hash, (7, 11)))
"""
    )
