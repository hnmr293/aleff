"""Differential regression tests for Issue #55's bytes text adapters."""

from __future__ import annotations

import codecs
from typing import Any, Callable

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def _effect_outcomes(run: Callable[[Callable[[], Any]], Any], values: tuple[Any, ...]) -> list[Any]:
    choice = effect("cpython_compat_22_choice")
    handler = create_handler(choice)

    @handler.on(choice)
    def handle(k: Any) -> list[Any]:
        outcomes: list[Any] = []
        for value in values:
            try:
                outcomes.append(k(value))
            except Exception as exc:  # The exception is part of the outcome.
                outcomes.append(type(exc).__name__)
        return outcomes

    return handler(lambda: run(choice))


def test_bytes_join_normal_results_and_types_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def record(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__)
    else:
        print(name, "return", type(value).__name__, repr(value))

separator = bytearray(b"|")
record("mixed", lambda: b"|".join([b"a", separator, memoryview(b"c")]))
record("empty", lambda: b"|".join(()))
record("single", lambda: b"|".join((memoryview(b"x"),)))
record("generator", lambda: b"|".join(item for item in (b"a", b"b")))
record("receiver", lambda: (separator.join([b"a", b"b"]), separator))
"""
    )


def test_bytes_join_exact_list_input_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
items = [b"a", bytearray(b"b"), memoryview(b"c")]
result = b"|".join(items)
print(type(items).__name__, type(result).__name__, repr(result))
"""
    )


def test_bytes_join_exact_tuple_input_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
items = (b"a", bytearray(b"b"), memoryview(b"c"))
result = b"|".join(items)
print(type(items).__name__, type(result).__name__, repr(result))
"""
    )


def test_bytes_join_rejects_invalid_and_noncontiguous_items() -> None:
    assert_cpython_compatible(
        r"""
def record(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__)
    else:
        print(name, "return", type(value).__name__, repr(value))

record("list-item", lambda: b",".join([[65]]))
record("tuple-item", lambda: b",".join([(65,)]))
record("integer-item", lambda: b",".join([65]))
record("str-item", lambda: b",".join(["a"]))
record("noncontiguous", lambda: b"".join([memoryview(b"abcd")[::2]]))
record("not-iterable", lambda: b",".join(None))
"""
    )


def test_bytes_join_detects_source_sequence_mutation() -> None:
    assert_cpython_compatible(
        r"""
def record(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__)
    else:
        print(name, "return", type(value).__name__, repr(value))

def source_sequence_change():
    items = []

    class Item:
        def __buffer__(self, flags):
            items.append(b"y")
            return memoryview(b"x")

    items.append(Item())
    return b"".join(items)

record("source-size", source_sequence_change)
"""
    )


def test_bytearray_join_normal_results_and_types_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def record(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__)
    else:
        print(name, "return", type(value).__name__, repr(value))

separator = bytearray(b"|")
record("mixed", lambda: separator.join([b"a", bytearray(b"b"), memoryview(b"c")]))
record("empty", lambda: separator.join(()))
record("single", lambda: separator.join((memoryview(b"x"),)))
record("generator", lambda: separator.join(item for item in (b"a", b"b")))
record("receiver", lambda: (separator.join([b"a", b"b"]), separator))
"""
    )


def test_bytearray_join_exact_list_and_tuple_inputs_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
separator = bytearray(b"|")
for name, items in (
    ("list", [b"a", bytearray(b"b"), memoryview(b"c")]),
    ("tuple", (b"a", bytearray(b"b"), memoryview(b"c"))),
):
    result = separator.join(items)
    print(name, type(items).__name__, type(result).__name__, repr(result), separator)
"""
    )


def test_bytearray_join_rejects_invalid_items() -> None:
    assert_cpython_compatible(
        r"""
def record(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__)
    else:
        print(name, "return", type(value).__name__, repr(value))

record("list-item", lambda: bytearray(b",").join([[65]]))
record("tuple-item", lambda: bytearray(b",").join([(65,)]))
record("integer-item", lambda: bytearray(b",").join([65]))
record("str-item", lambda: bytearray(b",").join(["a"]))
record("noncontiguous", lambda: bytearray(b"").join([memoryview(b"abcd")[::2]]))

"""
    )


def test_bytearray_join_detects_reentrant_mutations() -> None:
    assert_cpython_compatible(
        r"""
def record(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__)
    else:
        print(name, "return", type(value).__name__, repr(value))

def separator_resize():
    separator = bytearray(b",")

    class Items:
        def __iter__(self):
            separator.clear()
            return iter((b"a", b"b"))

    return separator.join(Items())

def source_sequence_change():
    items = []

    class Item:
        def __buffer__(self, flags):
            items.append(b"y")
            return memoryview(b"x")

    items.append(Item())
    return bytearray(b"").join(items)

def retained_item_buffer():
    first = bytearray(b"a")

    class Item:
        def __buffer__(self, flags):
            first.extend(b"x")
            return memoryview(b"b")

    return bytearray(b"").join([first, Item()])

record("separator-resize", separator_resize)
record("source-size", source_sequence_change)
record("retained-buffer", retained_item_buffer)
"""
    )


def test_bytes_and_bytearray_decode_normal_errors_and_corner_cases() -> None:
    assert_cpython_compatible(
        r"""
import codecs

def record(name, operation):
    try:
        value = operation()
    except Exception as exc:
        print(name, "raise", type(exc).__name__)
    else:
        print(name, "return", type(value).__name__, repr(value))

codecs.register_error("compat_nul", lambda exc: ("A\x00B", exc.end))
codecs.register_error("compat_skip", lambda exc: ("X", exc.end + 1))
codecs.register_error("compat_bad", lambda exc: (b"X", exc.end))

for name, value in (("bytes", b"a\xffb"), ("bytearray", bytearray(b"a\xffb"))):
    record(name + "-utf8", lambda value=value: value.__class__(b"caf\xc3\xa9").decode("utf-8"))
    record(name + "-replace", lambda value=value: value.decode("ascii", "replace"))
    record(name + "-ignore", lambda value=value: value.decode("ascii", "ignore"))
    record(name + "-nul", lambda value=value: value.decode("ascii", "compat_nul"))
    record(name + "-skip", lambda value=value: value.decode("ascii", "compat_skip"))
    record(name + "-strict", lambda value=value: value.decode("ascii", "strict"))
    record(name + "-bad-handler", lambda value=value: value.decode("ascii", "compat_bad"))
    record(name + "-bad-encoding", lambda value=value: value.decode("not-an-encoding"))
    record(name + "-bad-errors", lambda value=value: value.decode("ascii", "not-an-error-handler"))

record("bytes-empty", lambda: b"".decode())
record("bytearray-empty", lambda: bytearray().decode())
"""
    )


def test_effectful_bytes_decode_resumes_the_original_source() -> None:
    def bytes_run(choose: Callable[[], Any]) -> str:
        codecs.register_error("compat_22_bytes_effect", lambda exc: (str(choose()), exc.end))
        return b"a\xffb".decode("ascii", "compat_22_bytes_effect")

    assert _effect_outcomes(bytes_run, ("X", "Y")) == ["aXb", "aYb"]


def test_effectful_bytearray_decode_resumes_the_original_source() -> None:
    def bytearray_run(choose: Callable[[], Any]) -> str:
        codecs.register_error("compat_22_bytearray_effect", lambda exc: (str(choose()), exc.end))
        return bytearray(b"a\xffb").decode("ascii", "compat_22_bytearray_effect")

    assert _effect_outcomes(bytearray_run, ("X", "Y")) == ["aXb", "aYb"]


def test_effectful_bytes_decode_honors_error_position() -> None:
    def positioned_bytes(choose: Callable[[], Any]) -> str:
        codecs.register_error("compat_22_position_bytes", lambda exc: ("X", choose()))
        return b"a\xffb".decode("ascii", "compat_22_position_bytes")

    assert _effect_outcomes(positioned_bytes, (3, 4)) == ["aX", "IndexError"]


def test_effectful_bytearray_decode_honors_error_position() -> None:
    def positioned_bytearray(choose: Callable[[], Any]) -> str:
        codecs.register_error("compat_22_position_bytearray", lambda exc: ("X", choose()))
        return bytearray(b"a\xffb").decode("ascii", "compat_22_position_bytearray")

    assert _effect_outcomes(positioned_bytearray, (3, 4)) == ["aX", "IndexError"]


def test_effectful_bytes_decode_preserves_embedded_nul_in_replacement() -> None:
    def nul_bytes(choose: Callable[[], Any]) -> str:
        codecs.register_error("compat_22_nul_bytes", lambda exc: (choose(), exc.end))
        return b"a\xffb".decode("ascii", "compat_22_nul_bytes")

    assert _effect_outcomes(nul_bytes, ("X", "A\x00B")) == ["aXb", "aA\x00Bb"]


def test_effectful_bytearray_decode_uses_utf8_byte_length_for_replacement() -> None:
    def unicode_bytearray(choose: Callable[[], Any]) -> str:
        codecs.register_error("compat_22_unicode_bytearray", lambda exc: (choose(), exc.end))
        return bytearray(b"a\xffb").decode("ascii", "compat_22_unicode_bytearray")

    assert _effect_outcomes(unicode_bytearray, ("X", "é")) == ["aXb", "aéb"]
