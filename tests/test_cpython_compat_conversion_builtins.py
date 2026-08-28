"""CPython compatibility tests for conversion built-ins in Issue #55."""

from __future__ import annotations

from typing import Any

from cpython_compat_support import assert_cpython_compatible


def test_scalar_conversions_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def show(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "ERR", type(exc).__name__, str(exc))
    else:
        print(name, "OK", type(value).__name__, repr(value))


class IndexValue:
    def __index__(self):
        return -13


class IntValue:
    def __int__(self):
        return 17


class FloatValue:
    def __float__(self):
        return 2.5


class ComplexValue:
    def __complex__(self):
        return 3 + 4j


class TextValue:
    def __str__(self):
        return "text-value"


class AsciiValue:
    def __repr__(self):
        return "café"


for name, operation in (
    ("abs-int", lambda: abs(-23)),
    ("abs-float", lambda: abs(-2.5)),
    ("abs-complex", lambda: abs(3 - 4j)),
    ("abs-index", lambda: abs(IndexValue())),
    ("ascii-unicode", lambda: ascii("café\n")),
    ("ascii-container", lambda: ascii(["café", "plain"])),
    ("ascii-custom", lambda: ascii(AsciiValue())),
    ("bin-zero", lambda: bin(0)),
    ("bin-negative", lambda: bin(-13)),
    ("bin-large", lambda: bin(2**100 + 1)),
    ("bin-index", lambda: bin(IndexValue())),
    ("bool-empty", lambda: bool([])),
    ("bool-nonempty", lambda: bool([0])),
    ("bool-zero", lambda: bool(0)),
    ("bool-one", lambda: bool(1)),
    ("bool-nan", lambda: bool(float("nan"))),
    ("complex-string", lambda: complex(" -2.5+4j ")),
    ("complex-two-args", lambda: complex(2, -3.5)),
    ("complex-index", lambda: complex(IndexValue())),
    ("complex-custom", lambda: complex(ComplexValue())),
    ("float-string", lambda: float(" -1.25 ")),
    ("float-infinity", lambda: float("-inf")),
    ("float-index", lambda: float(IndexValue())),
    ("float-custom", lambda: float(FloatValue())),
    ("hex-zero", lambda: hex(0)),
    ("hex-negative", lambda: hex(-13)),
    ("hex-large", lambda: hex(2**100 + 1)),
    ("hex-index", lambda: hex(IndexValue())),
    ("int-number", lambda: int(-2.75)),
    ("int-string", lambda: int(" +101 ", 2)),
    ("int-bytes", lambda: int(b"ff", 16)),
    ("int-index", lambda: int(IndexValue())),
    ("int-custom", lambda: int(IntValue())),
    ("oct-zero", lambda: oct(0)),
    ("oct-negative", lambda: oct(-13)),
    ("oct-large", lambda: oct(2**100 + 1)),
    ("oct-index", lambda: oct(IndexValue())),
    ("str-number", lambda: str(-23)),
    ("str-bytes", lambda: str(b"caf\xc3\xa9", "utf-8")),
    ("str-custom", lambda: str(TextValue())),
):
    show(name, operation)
"""
    )


def test_scalar_conversion_errors_and_corner_cases_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def show(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, type(exc).__name__, str(exc))
    else:
        print(name, "OK", type(value).__name__, repr(value))


class BadIndex:
    def __index__(self):
        return 1.5


class BadAbs:
    def __abs__(self):
        return "not a number"


class BadBool:
    def __bool__(self):
        return 1


class BadComplex:
    def __complex__(self):
        return 1


class BadFloat:
    def __float__(self):
        return "not a float"


class BadInt:
    def __int__(self):
        return 1.5


class BadStr:
    def __str__(self):
        return b"not text"


for name, operation in (
    ("abs-type", lambda: abs("x")),
    ("abs-bad-result", lambda: abs(BadAbs())),
    ("ascii-bad-result", lambda: ascii(type("BadRepr", (), {"__repr__": lambda self: 1})())),
    ("bin-type", lambda: bin("x")),
    ("bin-bad-index", lambda: bin(BadIndex())),
    ("bool-bad-result", lambda: bool(BadBool())),
    ("complex-type", lambda: complex("x")),
    ("complex-bad-result", lambda: complex(BadComplex())),
    ("complex-too-many", lambda: complex(1, 2, 3)),
    ("float-type", lambda: float("not-a-float")),
    ("float-bad-result", lambda: float(BadFloat())),
    ("hex-type", lambda: hex("x")),
    ("hex-bad-index", lambda: hex(BadIndex())),
    ("int-invalid-base", lambda: int("10", 1)),
    ("int-base-non-string", lambda: int(10, 2)),
    ("int-bad-result", lambda: int(BadInt())),
    ("oct-type", lambda: oct("x")),
    ("oct-bad-index", lambda: oct(BadIndex())),
    ("str-bad-result", lambda: str(BadStr())),
    ("str-invalid-encoding", lambda: str(b"x", "no-such-encoding")),
    ("str-too-many", lambda: str(b"x", "ascii", "strict")),
):
    show(name, operation)

show("int-empty", lambda: int(""))
show("int-sign-only", lambda: int("+"))
show("int-whitespace", lambda: int("   "))
show("float-overflow-text", lambda: float("1e9999"))
show("complex-no-imaginary", lambda: complex("1+2"))
"""
    )


def test_sequence_and_mapping_constructors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def show(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "ERR", type(exc).__name__, str(exc))
    else:
        if isinstance(value, (set, frozenset)):
            value = (type(value).__name__, sorted(value))
        print(name, "OK", type(value).__name__, repr(value))


class Iterable:
    def __iter__(self):
        yield "first"
        yield "second"


class ByteIterable:
    def __iter__(self):
        yield 0
        yield 127
        yield 255


class PairIterable:
    def __iter__(self):
        yield ("one", 1)
        yield ["two", 2]


class MappingLike:
    def keys(self):
        return ["left", "right"]

    def __getitem__(self, key):
        return {"left": 10, "right": 20}[key]


class ExplodingIterable:
    def __iter__(self):
        yield "before-error"
        raise RuntimeError("iteration stopped")


class Unhashable:
    __hash__ = None


for name, operation in (
    ("bytearray-empty", lambda: bytearray()),
    ("bytearray-int", lambda: bytearray(3)),
    ("bytearray-bytes", lambda: bytearray(b"abc")),
    ("bytearray-encoded", lambda: bytearray("café", "utf-8")),
    ("bytearray-iterable", lambda: bytearray(ByteIterable())),
    ("bytearray-generator", lambda: bytearray(x for x in (1, 2, 3))),
    ("bytes-empty", lambda: bytes()),
    ("bytes-int", lambda: bytes(3)),
    ("bytes-encoded", lambda: bytes("café", "utf-8")),
    ("bytes-iterable", lambda: bytes(ByteIterable())),
    ("bytes-generator", lambda: bytes(x for x in (3, 2, 1))),
    ("dict-empty", lambda: dict()),
    ("dict-pairs", lambda: dict(PairIterable())),
    ("dict-mapping", lambda: dict(MappingLike())),
    ("dict-keyword", lambda: dict(PairIterable(), three=3)),
    ("frozenset-empty", lambda: frozenset()),
    ("frozenset-iterable", lambda: frozenset(Iterable())),
    ("frozenset-generator", lambda: frozenset(x for x in (2, 1, 2))),
    ("list-empty", lambda: list()),
    ("list-string", lambda: list("abc")),
    ("list-iterable", lambda: list(Iterable())),
    ("list-generator", lambda: list(x for x in (3, 1, 2))),
    ("set-empty", lambda: set()),
    ("set-iterable", lambda: set(Iterable())),
    ("set-generator", lambda: set(x for x in (2, 1, 2))),
    ("tuple-empty", lambda: tuple()),
    ("tuple-string", lambda: tuple("abc")),
    ("tuple-iterable", lambda: tuple(Iterable())),
    ("tuple-generator", lambda: tuple(x for x in (3, 1, 2))),
):
    show(name, operation)

backing = bytearray(b"abc")
view = memoryview(backing)
print("memoryview-bytes", type(view).__name__, view.format, view.itemsize, view.ndim, view.shape, view.strides, view.readonly, view.tobytes())
view[1] = ord("Z")
print("memoryview-mutation", backing, view.tobytes())
view.release()
show("memoryview-released", lambda: view.tobytes())
show("memoryview-bytes-input", lambda: memoryview(b"xyz").tobytes())

for name, operation in (
    ("bytearray-invalid-item", lambda: bytearray([256])),
    ("bytearray-not-iterable", lambda: bytearray(1.5)),
    ("bytes-invalid-item", lambda: bytes([-1])),
    ("bytes-iteration-error", lambda: bytes(ExplodingIterable())),
    ("dict-invalid-pair", lambda: dict([("key",)])),
    ("dict-not-iterable", lambda: dict(1)),
    ("dict-unhashable-key", lambda: dict([([], 1)])),
    ("frozenset-not-iterable", lambda: frozenset(1)),
    ("frozenset-unhashable", lambda: frozenset([Unhashable()])),
    ("list-not-iterable", lambda: list(1)),
    ("list-iteration-error", lambda: list(ExplodingIterable())),
    ("memoryview-not-buffer", lambda: memoryview(1)),
    ("set-not-iterable", lambda: set(1)),
    ("set-unhashable", lambda: set([Unhashable()])),
    ("tuple-not-iterable", lambda: tuple(1)),
    ("tuple-iteration-error", lambda: tuple(ExplodingIterable())),
):
    show(name, operation)
"""
    )


def test_constructor_argument_validation_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
def show(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, type(exc).__name__, str(exc))
    else:
        print(name, "OK", type(value).__name__, repr(value))


class ListSubclass(list):
    pass


class TupleSubclass(tuple):
    pass


class DictSubclass(dict):
    pass


class SetSubclass(set):
    pass


class FrozenSetSubclass(frozenset):
    pass


class BytesSubclass(bytes):
    pass


class BytearraySubclass(bytearray):
    pass


for name, operation in (
    ("list-subclass", lambda: ListSubclass((1, 2))),
    ("tuple-subclass", lambda: TupleSubclass((1, 2))),
    ("dict-subclass", lambda: DictSubclass({"a": 1})),
    ("set-subclass", lambda: SetSubclass((1, 2))),
    ("frozenset-subclass", lambda: FrozenSetSubclass((1, 2))),
    ("bytes-subclass", lambda: BytesSubclass(b"ab")),
    ("bytearray-subclass", lambda: BytearraySubclass(b"ab")),
    ("list-too-many", lambda: list([], [])),
    ("tuple-too-many", lambda: tuple([], [])),
    ("dict-too-many", lambda: dict({}, {})),
    ("set-too-many", lambda: set([], [])),
    ("frozenset-too-many", lambda: frozenset([], [])),
    ("bytes-too-many", lambda: bytes(1, "ascii")),
    ("bytearray-too-many", lambda: bytearray(1, "ascii")),
    ("memoryview-too-many", lambda: memoryview(b"x", b"y")),
):
    show(name, operation)
"""
    )


def test_memoryview_resume_revalidates_buffer_result() -> None:
    from aleff import create_handler, effect

    choose = effect("choose")
    handler = create_handler(choose)

    class Buffer:
        def __buffer__(self, _flags: int) -> Any:
            return choose()

        def __release_buffer__(self, _view: memoryview) -> None:
            pass

    @handler.on(choose)
    def resume(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in ("invalid", memoryview(b"ok")):
            try:
                result = k(value)
            except Exception as exc:
                outcomes.append((type(exc).__name__, str(exc)))
            else:
                outcomes.append(("return", result.tobytes()))
        return outcomes

    outcomes = handler(lambda: memoryview(Buffer()))
    assert outcomes[0][0] == "TypeError"
    assert outcomes[1] == ("return", b"ok")


def test_complex_resume_validates_the_second_argument_protocol() -> None:
    from aleff import create_handler, effect

    choose = effect("choose")
    handler = create_handler(choose)

    class RealPart:
        def __complex__(self) -> complex:
            return 3 + 4j

    class ImaginaryPart:
        def __float__(self) -> float:
            return choose()

    @handler.on(choose)
    def resume(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in (2.5, 6.25):
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:
                outcomes.append((type(exc).__name__, str(exc)))
        return outcomes

    assert handler(lambda: complex(RealPart(), ImaginaryPart())) == [
        ("return", 3 + 6.5j),
        ("return", 3 + 10.25j),
    ]
