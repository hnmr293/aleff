"""Acceptance tests for multi-shot continuations crossing CPython C calls.

Each case runs in its own process because an unsupported continuation shape can
terminate the interpreter instead of raising a Python exception.  The child
assertions describe the required behavior; the parent assertion preserves the
child's faulthandler output when that contract is violated.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator as ABCIterator
import functools
import itertools
import operator
from pathlib import Path
import subprocess
import sys
from typing import Any, Literal, cast

import pytest

from aleff import create_handler, effect, wind_range


CaseKind = Literal["normal", "error", "corner"]
Case = Callable[[], None]
Choose = Callable[[], Any]

_CASES: dict[str, tuple[CaseKind, Case]] = {}


def _case(kind: CaseKind, name: str) -> Callable[[Case], Case]:
    def register(test_case: Case) -> Case:
        _CASES[name] = (kind, test_case)
        return test_case

    return register


def _resume_outcomes(
    run: Callable[[Callable[[], Any]], Any],
    values: tuple[Any, ...] = (1, 10),
) -> list[tuple[str, Any]]:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in values:
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:  # The exception is part of the test result.
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    return cast(list[tuple[str, Any]], handler(lambda: run(choose)))


def _returns(*values: Any) -> list[tuple[str, Any]]:
    return [("return", value) for value in values]


def _assert_equal(actual: Any, expected: Any) -> None:
    if actual != expected:
        raise AssertionError(f"actual:   {actual!r}\nexpected: {expected!r}")


@_case("normal", "plain_python_call_control")
def _plain_python_call_control() -> None:
    def run(choose: Choose) -> int:
        def callback() -> int:
            return choose() + 100

        return callback() + 900

    _assert_equal(_resume_outcomes(run), _returns(1001, 1010))


@_case("normal", "sum_map_nested_boundaries")
def _sum_map_nested_boundaries() -> None:
    def run(choose: Choose) -> int:
        return sum(map(lambda _item: cast(int, choose()), (None,)), 100)

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "all_normalizes_callback_result")
def _all_normalizes_callback_result() -> None:
    def run(choose: Choose) -> bool:
        return all(map(lambda _item: choose(), (None,)))

    outcomes = _resume_outcomes(run, (0, 2))
    _assert_equal(outcomes, _returns(False, True))
    assert all(type(value) is bool for _, value in outcomes)


@_case("normal", "any_normalizes_callback_result")
def _any_normalizes_callback_result() -> None:
    def run(choose: Choose) -> bool:
        return any(map(lambda _item: choose(), (None,)))

    outcomes = _resume_outcomes(run, (0, 2))
    _assert_equal(outcomes, _returns(False, True))
    assert all(type(value) is bool for _, value in outcomes)


def _keyed_builtin_case(operation: str) -> None:
    def run(choose: Choose) -> int | list[int]:
        def key(value: int) -> int:
            return cast(int, choose()) if value == 1 else 5

        if operation in {"min", "max"}:
            result: int | None = None
            with wind_range(1, 3) as values:
                result = min(values, key=key) if operation == "min" else max(values, key=key)
            assert result is not None
            return result
        return sorted((1, 2), key=key)

    outcomes = _resume_outcomes(run, (0, 10))
    if operation == "sorted":
        assert all(tag == "return" and isinstance(value, list) for tag, value in outcomes), outcomes
        _assert_equal(outcomes[1][1], [2, 1])
    else:
        expected = {"min": (1, 2), "max": (2, 1)}[operation]
        _assert_equal(outcomes, _returns(*expected))


for _name in ("min", "max", "sorted"):
    _case("normal", f"{_name}_effectful_key")(functools.partial(_keyed_builtin_case, _name))


def _identity(value: Any) -> Any:
    return value


def _dict_item(value: Any) -> tuple[str, Any]:
    return "key", value


class _EffectfulIterable:
    def __init__(self, choose: Choose, item_factory: Callable[[Any], Any] = _identity) -> None:
        self.choose = choose
        self.item_factory = item_factory

    def __iter__(self) -> ABCIterator[Any]:
        value = self.choose()
        return iter((self.item_factory(value),))


def _constructor_case(constructor_name: str) -> None:
    constructor = cast(
        Callable[[Any], Any],
        {
            "list": list,
            "tuple": tuple,
            "dict": dict,
            "set": set,
            "frozenset": frozenset,
            "bytes": bytes,
            "bytearray": bytearray,
        }[constructor_name],
    )
    item_factory = _dict_item if constructor_name == "dict" else _identity

    def run(choose: Choose) -> Any:
        return constructor(_EffectfulIterable(choose, item_factory))

    outcomes = _resume_outcomes(run)
    assert all(tag == "return" and type(value).__name__ == constructor_name for tag, value in outcomes), outcomes
    if constructor_name == "dict":
        assert outcomes[0][1]["key"] == 1
        assert outcomes[1][1]["key"] == 10
    else:
        assert 1 in outcomes[0][1]
        assert 10 in outcomes[1][1]


for _name in ("list", "tuple", "dict", "set", "frozenset", "bytes", "bytearray"):
    _case("normal", f"{_name}_constructor_effectful_iterable")(functools.partial(_constructor_case, _name))


@_case("normal", "zip_constructor_effectful_iterable")
def _zip_constructor_effectful_iterable() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(zip(_EffectfulIterable(choose), (100,)))

    _assert_equal(_resume_outcomes(run), _returns([(1, 100)], [(10, 100)]))


@_case("normal", "zip_next_effectful_iterator")
def _zip_next_effectful_iterator() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(zip(map(lambda _item: cast(int, choose()), (None,)), (100,)))

    _assert_equal(_resume_outcomes(run), _returns([(1, 100)], [(10, 100)]))


@_case("normal", "enumerate_constructor_effectful_iterable")
def _enumerate_constructor_effectful_iterable() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(enumerate(_EffectfulIterable(choose)))

    _assert_equal(_resume_outcomes(run), _returns([(0, 1)], [(0, 10)]))


@_case("normal", "enumerate_next_effectful_iterator")
def _enumerate_next_effectful_iterator() -> None:
    def run(choose: Choose) -> list[tuple[int, int]]:
        return list(enumerate(map(lambda _item: cast(int, choose()), (None,))))

    _assert_equal(_resume_outcomes(run), _returns([(0, 1)], [(0, 10)]))


@_case("normal", "enumerate_effectful_start_index")
def _enumerate_effectful_start_index() -> None:
    def run(choose: Choose) -> list[tuple[int, str]]:
        class Start:
            def __index__(self) -> int:
                return cast(int, choose())

        return list(enumerate(("value",), Start()))  # pyright: ignore[reportArgumentType]

    _assert_equal(_resume_outcomes(run), _returns([(1, "value")], [(10, "value")]))


@_case("normal", "reversed_fallback_effectful_length")
def _reversed_fallback_effectful_length() -> None:
    def run(choose: Choose) -> list[int]:
        class Sequence:
            def __len__(self) -> int:
                return cast(int, choose())

            def __getitem__(self, index: int) -> int:
                if index < 0:
                    raise IndexError
                return index

        return list(reversed(Sequence()))

    _assert_equal(_resume_outcomes(run, (1, 3)), _returns([0], [2, 1, 0]))


@_case("normal", "reversed_fallback_effectful_getitem")
def _reversed_fallback_effectful_getitem() -> None:
    def run(choose: Choose) -> list[int]:
        class Sequence:
            def __len__(self) -> int:
                return 2

            def __getitem__(self, index: int) -> int:
                if index < 0 or index > 1:
                    raise IndexError
                return cast(int, choose()) if index == 1 else 0

        return list(reversed(Sequence()))

    _assert_equal(_resume_outcomes(run), _returns([1, 0], [10, 0]))


@_case("normal", "tuple_constructor_preserves_exact_tuple_identity")
def _tuple_constructor_preserves_exact_tuple_identity() -> None:
    source = (1, 2)
    assert tuple(source) is source


@_case("corner", "tuple_constructor_standard_fast_paths")
def _tuple_constructor_standard_fast_paths() -> None:
    empty: tuple[()] = ()
    assert tuple(empty) is empty

    class TupleSubclass(tuple[int, ...]):
        pass

    source = TupleSubclass((1, 2))
    result = tuple(source)
    assert type(result) is tuple
    assert result == source
    assert result is not source


@_case("normal", "immutable_constructors_preserve_exact_identity")
def _immutable_constructors_preserve_exact_identity() -> None:
    byte_string = b"ab"
    frozen = frozenset({1, 2})
    assert bytes(byte_string) is byte_string
    assert frozenset(frozen) is frozen


@_case("normal", "bytes_constructor_prefers_bytes_protocol")
def _bytes_constructor_prefers_bytes_protocol() -> None:
    class BytesAndIterable:
        def __bytes__(self) -> bytes:
            return b"custom"

        def __iter__(self) -> Any:
            return iter((97, 98))

    assert bytes(BytesAndIterable()) == b"custom"


@_case("normal", "bytes_constructor_effectful_bytes_protocol")
def _bytes_constructor_effectful_bytes_protocol() -> None:
    def run(choose: Choose) -> bytes:
        class EffectfulBytes:
            def __bytes__(self) -> bytes:
                return bytes((cast(int, choose()),))

        return bytes(EffectfulBytes())

    _assert_equal(_resume_outcomes(run), _returns(b"\x01", b"\x0a"))


@_case("normal", "bytes_constructor_effectful_index_protocol")
def _bytes_constructor_effectful_index_protocol() -> None:
    def run(choose: Choose) -> int:
        class EffectfulIndex:
            def __index__(self) -> int:
                return cast(int, choose())

        return len(bytes(EffectfulIndex()))

    _assert_equal(_resume_outcomes(run), _returns(1, 10))


@_case("error", "bytes_constructor_rejects_invalid_bytes_protocol")
def _bytes_constructor_rejects_invalid_bytes_protocol() -> None:
    class InvalidBytesAndIterable:
        def __bytes__(self) -> str:
            return "invalid"

        def __iter__(self) -> Any:
            return iter((97,))

    try:
        bytes(InvalidBytesAndIterable())  # type: ignore[arg-type]
    except TypeError as exc:
        assert "__bytes__ returned non-bytes" in str(exc)
    else:
        raise AssertionError("invalid __bytes__ result was accepted")


@_case("error", "bytes_constructor_invalid_protocol_isolated_per_shot")
def _bytes_constructor_invalid_protocol_isolated_per_shot() -> None:
    def run(choose: Choose) -> bytes:
        class EffectfulBytes:
            def __bytes__(self) -> Any:
                value = choose()
                return "invalid" if value == "invalid" else bytes((cast(int, value),))

        return bytes(EffectfulBytes())

    _assert_equal(
        _resume_outcomes(run, ("invalid", 10)),
        [("raise", "TypeError"), ("return", b"\x0a")],
    )


@_case("corner", "bytes_constructor_standard_protocol_precedence")
def _bytes_constructor_standard_protocol_precedence() -> None:
    class BufferAndIterable(bytearray):
        def __iter__(self) -> Any:
            return iter((120,))

    class IndexAndIterable:
        def __index__(self) -> int:
            return 3

        def __iter__(self) -> Any:
            return iter((120,))

    assert bytes(BufferAndIterable(b"ab")) == b"ab"
    assert bytes(IndexAndIterable()) == b"\0\0\0"


@_case("normal", "bytes_constructor_effectful_buffer_protocol")
def _bytes_constructor_effectful_buffer_protocol() -> None:
    def run(choose: Choose) -> bytes:
        class EffectfulBuffer:
            def __buffer__(self, _flags: int) -> memoryview:
                return memoryview(bytes((cast(int, choose()),)))

            def __release_buffer__(self, _view: memoryview) -> None:
                pass

        return bytes(EffectfulBuffer())

    _assert_equal(_resume_outcomes(run), _returns(b"\x01", b"\x0a"))


@_case("error", "bytes_constructor_invalid_buffer_isolated_per_shot")
def _bytes_constructor_invalid_buffer_isolated_per_shot() -> None:
    def run(choose: Choose) -> bytes:
        class EffectfulBuffer:
            def __buffer__(self, _flags: int) -> Any:
                value = choose()
                return "invalid" if value == "invalid" else memoryview(bytes((cast(int, value),)))

            def __release_buffer__(self, _view: memoryview) -> None:
                pass

        return bytes(EffectfulBuffer())

    _assert_equal(
        _resume_outcomes(run, ("invalid", 10)),
        [("raise", "TypeError"), ("return", b"\x0a")],
    )


@_case("error", "zip_strict_mismatch_isolated_per_shot")
def _zip_strict_mismatch_isolated_per_shot() -> None:
    def run(choose: Choose) -> list[tuple[int, object]]:
        return list(zip(map(lambda _item: cast(int, choose()), (None,)), (), strict=True))

    _assert_equal(
        _resume_outcomes(run),
        [("raise", "ValueError"), ("raise", "ValueError")],
    )


@_case("error", "enumerate_invalid_start_isolated_per_shot")
def _enumerate_invalid_start_isolated_per_shot() -> None:
    def run(choose: Choose) -> list[tuple[int, str]]:
        class Start:
            def __index__(self) -> Any:
                value = choose()
                return "invalid" if value == "invalid" else value

        return list(enumerate(("value",), Start()))  # pyright: ignore[reportArgumentType]

    _assert_equal(
        _resume_outcomes(run, ("invalid", 10)),
        [("raise", "TypeError"), ("return", [(10, "value")])],
    )


@_case("error", "reversed_invalid_length_isolated_per_shot")
def _reversed_invalid_length_isolated_per_shot() -> None:
    def run(choose: Choose) -> list[int]:
        class Sequence:
            def __len__(self) -> int:
                return cast(int, choose())

            def __getitem__(self, index: int) -> int:
                if index < 0:
                    raise IndexError
                return index

        return list(reversed(Sequence()))

    _assert_equal(
        _resume_outcomes(run, (-1, 2)),
        [("raise", "ValueError"), ("return", [1, 0])],
    )


@_case("corner", "bytes_constructor_effectful_release_buffer")
def _bytes_constructor_effectful_release_buffer() -> None:
    def run(choose: Choose) -> bytes:
        class EffectfulRelease:
            def __buffer__(self, _flags: int) -> memoryview:
                return memoryview(b"value")

            def __release_buffer__(self, _view: memoryview) -> None:
                choose()

        return bytes(EffectfulRelease())

    _assert_equal(_resume_outcomes(run), _returns(b"value", b"value"))


@_case("corner", "zip_preserves_multiple_pending_iterators")
def _zip_preserves_multiple_pending_iterators() -> None:
    def run(choose: Choose) -> list[tuple[int, int, int]]:
        return list(
            zip(
                map(lambda _item: cast(int, choose()), (None,)),
                (100,),
                (200,),
            )
        )

    _assert_equal(_resume_outcomes(run), _returns([(1, 100, 200)], [(10, 100, 200)]))


@_case("corner", "zip_without_iterables_is_empty")
def _zip_without_iterables_is_empty() -> None:
    assert list(zip()) == []


@_case("corner", "enumerate_effectful_large_start")
def _enumerate_effectful_large_start() -> None:
    def run(choose: Choose) -> list[tuple[int, str]]:
        class Start:
            def __index__(self) -> int:
                return cast(int, choose())

        return list(enumerate(("value",), Start()))  # pyright: ignore[reportArgumentType]

    starts = (sys.maxsize, sys.maxsize + 1)
    _assert_equal(
        _resume_outcomes(run, starts),
        _returns([(starts[0], "value")], [(starts[1], "value")]),
    )


@_case("corner", "reversed_effectful_getitem_three_shot_suffix")
def _reversed_effectful_getitem_three_shot_suffix() -> None:
    def run(choose: Choose) -> list[int]:
        class Sequence:
            def __len__(self) -> int:
                return 2

            def __getitem__(self, index: int) -> int:
                if index < 0 or index > 1:
                    raise IndexError
                return cast(int, choose()) if index == 1 else 0

        return list(reversed(Sequence()))

    _assert_equal(_resume_outcomes(run, (1, 2, 3)), _returns([1, 0], [2, 0], [3, 0]))


@_case("normal", "bytearray_constructor_effectful_index_protocol")
def _bytearray_constructor_effectful_index_protocol() -> None:
    def run(choose: Choose) -> int:
        class EffectfulIndex:
            def __index__(self) -> int:
                return cast(int, choose())

        return len(bytearray(EffectfulIndex()))

    _assert_equal(_resume_outcomes(run), _returns(1, 10))


@_case("normal", "bytearray_constructor_effectful_buffer_protocol")
def _bytearray_constructor_effectful_buffer_protocol() -> None:
    def run(choose: Choose) -> bytearray:
        class EffectfulBuffer:
            def __buffer__(self, _flags: int) -> memoryview:
                return memoryview(bytes((cast(int, choose()),)))

            def __release_buffer__(self, _view: memoryview) -> None:
                pass

        return bytearray(EffectfulBuffer())

    _assert_equal(_resume_outcomes(run), _returns(bytearray(b"\x01"), bytearray(b"\x0a")))


@_case("corner", "bytearray_constructor_standard_protocol_precedence")
def _bytearray_constructor_standard_protocol_precedence() -> None:
    class BufferAndIterable(bytearray):
        def __iter__(self) -> Any:
            return iter((120,))

    class IndexAndIterable:
        def __index__(self) -> int:
            return 3

        def __iter__(self) -> Any:
            return iter((120,))

    assert bytearray(BufferAndIterable(b"ab")) == b"ab"
    assert bytearray(IndexAndIterable()) == b"\0\0\0"


@_case("corner", "bytearray_constructor_native_buffer_inputs")
def _bytearray_constructor_native_buffer_inputs() -> None:
    assert bytearray(b"bytes") == b"bytes"
    assert bytearray(bytearray(b"bytearray")) == b"bytearray"
    assert bytearray(memoryview(b"memoryview")) == b"memoryview"


@_case("corner", "bytearray_constructor_effectful_release_buffer")
def _bytearray_constructor_effectful_release_buffer() -> None:
    def run(choose: Choose) -> bytearray:
        class EffectfulRelease:
            def __buffer__(self, _flags: int) -> memoryview:
                return memoryview(b"value")

            def __release_buffer__(self, _view: memoryview) -> None:
                choose()

        return bytearray(EffectfulRelease())

    _assert_equal(
        _resume_outcomes(run),
        _returns(bytearray(b"value"), bytearray(b"value")),
    )


@_case("normal", "filter_predicate_and_list_consumer")
def _filter_predicate_and_list_consumer() -> None:
    def run(choose: Choose) -> list[int]:
        return list(filter(lambda _item: choose(), (99,)))

    outcomes = _resume_outcomes(run, (0, 1))
    assert all(tag == "return" and isinstance(value, list) for tag, value in outcomes), outcomes
    assert 99 in outcomes[1][1], outcomes


@_case("normal", "iter_callable_sentinel_and_list_consumer")
def _iter_callable_sentinel_and_list_consumer() -> None:
    def run(choose: Choose) -> list[int]:
        class CallableUntilSentinel:
            done = False

            def __call__(self) -> int:
                if self.done:
                    return 99
                self.done = True
                return cast(int, choose())

        return list(iter(CallableUntilSentinel(), 99))

    outcomes = _resume_outcomes(run)
    assert all(tag == "return" and isinstance(value, list) for tag, value in outcomes), outcomes
    assert 1 in outcomes[0][1]
    assert 10 in outcomes[1][1]


@_case("normal", "next_default_after_effectful_stop")
def _next_default_after_effectful_stop() -> None:
    def run(choose: Choose) -> int:
        class Iterator:
            def __iter__(self) -> Iterator:
                return self

            def __next__(self) -> int:
                if choose() == 0:
                    raise StopIteration
                return 42

        return next(Iterator(), 99)

    _assert_equal(_resume_outcomes(run, (1, 0, 1)), _returns(42, 99, 42))


@_case("normal", "next_default_after_effectful_map_callback_stop")
def _next_default_after_effectful_map_callback_stop() -> None:
    def run(choose: Choose) -> int:
        def callback(_item: object) -> int:
            if choose() == 0:
                raise StopIteration
            return 42

        return next(map(callback, (None,)), 99)

    _assert_equal(_resume_outcomes(run, (1, 0, 1)), _returns(42, 99, 42))


@_case("normal", "iter_protocol_returns_iterator")
def _iter_protocol_returns_iterator() -> None:
    def run(choose: Choose) -> list[int]:
        class Iterable:
            def __iter__(self):
                return iter((cast(int, choose()),))

        return list(Iterable())

    outcomes = _resume_outcomes(run)
    assert all(tag == "return" and isinstance(value, list) for tag, value in outcomes), outcomes
    assert 1 in outcomes[0][1], outcomes
    assert 10 in outcomes[1][1], outcomes


@_case("normal", "reversed_protocol_returns_iterator")
def _reversed_protocol_returns_iterator() -> None:
    def run(choose: Choose) -> list[int]:
        class Reversible:
            def __reversed__(self) -> Any:
                return iter((cast(int, choose()),))

        return list(reversed(cast(Any, Reversible())))

    outcomes = _resume_outcomes(run)
    assert all(tag == "return" and isinstance(value, list) for tag, value in outcomes), outcomes
    assert 1 in outcomes[0][1], outcomes
    assert 10 in outcomes[1][1], outcomes


@_case("normal", "aiter_protocol_returns_async_iterator")
def _aiter_protocol_returns_async_iterator() -> None:
    def run(choose: Choose) -> bool:
        class AsyncIterator:
            def __aiter__(self):
                choose()
                return self

            async def __anext__(self) -> int:
                raise StopAsyncIteration

        target = AsyncIterator()
        return aiter(target) is target

    _assert_equal(_resume_outcomes(run), _returns(True, True))


@_case("normal", "anext_default_without_event_loop")
def _anext_default_without_event_loop() -> None:
    def run(choose: Choose) -> int:
        class AsyncIterator:
            def __aiter__(self):
                return self

            async def __anext__(self) -> int:
                if choose() == 0:
                    raise StopAsyncIteration
                return 42

        async def consume() -> int:
            return await anext(AsyncIterator(), 99)

        coroutine = consume()
        try:
            coroutine.send(None)
        except StopIteration as completed:
            return cast(int, completed.value)
        raise AssertionError("coroutine unexpectedly suspended")

    _assert_equal(_resume_outcomes(run, (0, 1)), _returns(99, 42))


@_case("normal", "binary_add_protocol")
def _binary_add_protocol() -> None:
    def run(choose: Choose) -> int:
        class Addend:
            def __add__(self, other: Any) -> int:
                assert other == 7
                return cast(int, choose()) + 100

        return Addend() + 7

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


def _unary_protocol_case(method_name: str, operation: Callable[[Any], Any], values: tuple[Any, ...]) -> None:
    def run(choose: Choose) -> Any:
        def method(_self: Any, *_args: Any) -> Any:
            value = choose()
            if method_name == "__bool__":
                return bool(value)
            if method_name == "__float__":
                return float(value) + 0.5
            if method_name == "__complex__":
                return complex(value, 0.5)
            return value

        target_type = type("UnaryProtocolTarget", (), {method_name: method})
        return operation(target_type())

    expected = {
        "__bool__": tuple(bool(value) for value in values),
        "__float__": tuple(float(value) + 0.5 for value in values),
        "__complex__": tuple(complex(value, 0.5) for value in values),
        "__index__": tuple(bin(value) for value in values),
    }.get(method_name, values)
    _assert_equal(_resume_outcomes(run, values), _returns(*expected))


def _direct_neg(target: Any) -> Any:
    return -target


def _direct_pos(target: Any) -> Any:
    return +target


def _direct_invert(target: Any) -> Any:
    return ~target


_UNARY_PROTOCOLS: tuple[tuple[str, Callable[[Any], Any], tuple[Any, ...]], ...] = (
    ("__bool__", bool, (False, True)),
    ("__len__", len, (1, 10)),
    ("__abs__", abs, (1, 10)),
    ("__neg__", _direct_neg, (1, 10)),
    ("__pos__", _direct_pos, (1, 10)),
    ("__invert__", _direct_invert, (1, 10)),
    ("__index__", bin, (1, 10)),
    ("__int__", int, (1, 10)),
    ("__float__", float, (1, 10)),
    ("__complex__", complex, (1, 10)),
    ("__round__", lambda target: round(target, 2), (1, 10)),
)

for _method_name, _operation, _values in _UNARY_PROTOCOLS:
    _case("normal", f"unary_protocol_{_method_name}")(
        functools.partial(_unary_protocol_case, _method_name, _operation, _values)
    )


@_case("error", "len_success_invalid_success")
def _len_success_invalid_success() -> None:
    invalid = object()

    def run(choose: Choose) -> int:
        class Target:
            def __len__(self) -> Any:
                return choose()

        return len(Target())

    _assert_equal(
        _resume_outcomes(run, (1, invalid, 10)),
        [("return", 1), ("raise", "TypeError"), ("return", 10)],
    )


_BINARY_OPERATIONS: tuple[tuple[str, str], ...] = (
    ("add", "+"),
    ("sub", "-"),
    ("mul", "*"),
    ("matmul", "@"),
    ("truediv", "/"),
    ("floordiv", "//"),
    ("mod", "%"),
    ("pow", "**"),
    ("lshift", "<<"),
    ("rshift", ">>"),
    ("and", "&"),
    ("xor", "^"),
    ("or", "|"),
)


def _binary_protocol_case(
    method_name: str,
    symbol: str,
    reflected: bool,
    inplace: bool,
) -> None:
    namespace: dict[str, Any] = {}
    if inplace:
        source = f"def apply(target):\n    target {symbol}= 7\n    return target"
    elif reflected:
        source = f"def apply(target):\n    return 7 {symbol} target"
    else:
        source = f"def apply(target):\n    return target {symbol} 7"
    exec(source, namespace)
    apply = cast(Callable[[Any], Any], namespace["apply"])

    def run(choose: Choose) -> Any:
        def method(_self: Any, _other: Any) -> int:
            return cast(int, choose()) + 100

        target_type = type("BinaryProtocolTarget", (), {method_name: method})
        return apply(target_type())

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


for _operator_name, _symbol in _BINARY_OPERATIONS:
    _case("normal", f"binary_protocol___{_operator_name}__")(
        functools.partial(_binary_protocol_case, f"__{_operator_name}__", _symbol, False, False)
    )
    _case("normal", f"reflected_protocol___r{_operator_name}__")(
        functools.partial(_binary_protocol_case, f"__r{_operator_name}__", _symbol, True, False)
    )
    _case("normal", f"inplace_protocol___i{_operator_name}__")(
        functools.partial(_binary_protocol_case, f"__i{_operator_name}__", _symbol, False, True)
    )


def _comparison_protocol_case(method_name: str, symbol: str) -> None:
    namespace: dict[str, Any] = {}
    exec(f"def compare(target, other):\n    return target {symbol} other", namespace)
    compare = cast(Callable[[Any, Any], Any], namespace["compare"])

    def run(choose: Choose) -> Any:
        def method(_self: Any, _other: Any) -> Any:
            return choose()

        target_type = type("ComparisonProtocolTarget", (), {method_name: method})
        return compare(target_type(), object())

    _assert_equal(_resume_outcomes(run, (False, True)), _returns(False, True))


for _comparison_name, _comparison_symbol in (
    ("eq", "=="),
    ("ne", "!="),
    ("lt", "<"),
    ("le", "<="),
    ("gt", ">"),
    ("ge", ">="),
):
    _case("normal", f"comparison_protocol___{_comparison_name}__")(
        functools.partial(_comparison_protocol_case, f"__{_comparison_name}__", _comparison_symbol)
    )


@_case("normal", "reflected_add_after_not_implemented")
def _reflected_add_after_not_implemented() -> None:
    def run(choose: Choose) -> int:
        class Left:
            def __add__(self, other: Any) -> Any:
                return NotImplemented

        class Right:
            def __radd__(self, other: Any) -> int:
                assert isinstance(other, Left)
                return cast(int, choose()) + 100

        return Left() + Right()

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "property_descriptor_get")
def _property_descriptor_get() -> None:
    def run(choose: Choose) -> int:
        class Target:
            @property
            def value(self) -> int:
                return cast(int, choose()) + 100

        return Target().value

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "descriptor_set")
def _descriptor_set() -> None:
    def run(choose: Choose) -> int:
        class Descriptor:
            def __set__(self, instance: Any, value: int) -> None:
                instance.saved = cast(int, choose()) + value

        class Target:
            value = Descriptor()

            def __init__(self) -> None:
                self.saved = 0

        target = Target()
        target.value = 100
        return target.saved

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "item_get")
def _item_get() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __getitem__(self, key: str) -> int:
                assert key == "key"
                return cast(int, choose()) + 100

        return Target()["key"]

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "item_set")
def _item_set() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __init__(self) -> None:
                self.saved = 0

            def __setitem__(self, key: str, value: int) -> None:
                assert key == "key"
                self.saved = cast(int, choose()) + value

        target = Target()
        target["key"] = 100
        return target.saved

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "attribute_getattr_fallback")
def _attribute_getattr_fallback() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __getattr__(self, name: str) -> int:
                assert name == "missing"
                return cast(int, choose()) + 100

        return Target().missing

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "attribute_setattr")
def _attribute_setattr() -> None:
    def run(choose: Choose) -> int:
        class Target:
            saved = 0

            def __setattr__(self, name: str, value: int) -> None:
                assert name == "value"
                object.__setattr__(self, "saved", cast(int, choose()) + value)

        target = Target()
        target.value = 100
        return target.saved

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "attribute_delattr")
def _attribute_delattr() -> None:
    def run(choose: Choose) -> int:
        class Target:
            saved = 0

            def __delattr__(self, name: str) -> None:
                assert name == "value"
                object.__setattr__(self, "saved", cast(int, choose()) + 100)

        target = Target()
        del target.value
        return target.saved

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "descriptor_delete")
def _descriptor_delete() -> None:
    def run(choose: Choose) -> int:
        class Descriptor:
            def __delete__(self, instance: Any) -> None:
                instance.saved = cast(int, choose()) + 100

        class Target:
            value = Descriptor()
            saved = 0

        target = Target()
        del target.value
        return target.saved

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "item_delete")
def _item_delete() -> None:
    def run(choose: Choose) -> int:
        class Target:
            saved = 0

            def __delitem__(self, key: str) -> None:
                assert key == "key"
                self.saved = cast(int, choose()) + 100

        target = Target()
        del target["key"]
        return target.saved

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "class_getitem")
def _class_getitem() -> None:
    def run(choose: Choose) -> int:
        class Target:
            @classmethod
            def __class_getitem__(cls, key: str) -> int:
                assert key == "key"
                return cast(int, choose()) + 100

        return cast(int, cast(Any, Target)["key"])

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


def _representation_protocol_case(method_name: str, operation: Callable[[Any], Any]) -> None:
    def run(choose: Choose) -> Any:
        def method(_self: Any, *args: Any) -> Any:
            value = choose()
            if method_name in {"__str__", "__repr__"}:
                return f"value={value}"
            if method_name == "__format__":
                return f"value={value}:{args[0]}"
            return value

        target_type = type("RepresentationTarget", (), {method_name: method})
        return operation(target_type())

    expected = {
        "__str__": ("value=1", "value=10"),
        "__repr__": ("value=1", "value=10"),
        "__format__": ("value=1:spec", "value=10:spec"),
        "__hash__": (1, 10),
    }[method_name]
    _assert_equal(_resume_outcomes(run), _returns(*expected))


def _format_spec(target: Any) -> str:
    return format(target, "spec")


_REPRESENTATION_PROTOCOLS: tuple[tuple[str, Callable[[Any], Any]], ...] = (
    ("__str__", str),
    ("__repr__", repr),
    ("__format__", _format_spec),
    ("__hash__", hash),
)

for _method_name, _operation in _REPRESENTATION_PROTOCOLS:
    _case("normal", f"representation_protocol_{_method_name}")(
        functools.partial(_representation_protocol_case, _method_name, _operation)
    )


@_case("normal", "contains_normalizes_result")
def _contains_normalizes_result() -> None:
    def run(choose: Choose) -> bool:
        class Target:
            def __contains__(self, item: str) -> Any:
                assert item == "item"
                return choose()

        return "item" in Target()

    outcomes = _resume_outcomes(run, (0, 2))
    _assert_equal(outcomes, _returns(False, True))
    assert all(type(value) is bool for _, value in outcomes)


@_case("normal", "list_extend_effectful_iterable")
def _list_extend_effectful_iterable() -> None:
    def run(choose: Choose) -> tuple[None, tuple[int, ...]]:
        target: list[int] = []
        method_result = target.extend(_EffectfulIterable(choose))
        return method_result, tuple(target)

    outcomes = _resume_outcomes(run)
    assert all(tag == "return" and value[0] is None for tag, value in outcomes), outcomes
    assert 1 in outcomes[0][1][1]
    assert 10 in outcomes[1][1][1]


@_case("normal", "list_count_effectful_equality")
def _list_count_effectful_equality() -> None:
    def run(choose: Choose) -> int:
        class Item:
            def __eq__(self, other: object) -> Any:
                assert other == 99
                return choose()

        items: list[Any] = [Item()]
        return items.count(99)

    _assert_equal(_resume_outcomes(run, (0, 2)), _returns(0, 1))


@_case("normal", "dict_get_effectful_equality")
def _dict_get_effectful_equality() -> None:
    def run(choose: Choose) -> str:
        class Key:
            def __hash__(self) -> int:
                return 0

            def __eq__(self, other: object) -> Any:
                return choose()

        stored = Key()
        lookup = Key()
        mapping: dict[Any, str] = {stored: "found"}
        return mapping.get(lookup, "missing")

    _assert_equal(_resume_outcomes(run, (0, 1)), _returns("missing", "found"))


@_case("normal", "itertools_accumulate_pending_suffix")
def _itertools_accumulate_pending_suffix() -> None:
    def run(choose: Choose) -> list[int]:
        def add(left: int, right: int) -> int:
            if right == 2:
                return left + cast(int, choose())
            return left + right

        result: list[int] | None = None
        with wind_range(1, 4) as values:
            result = list(itertools.accumulate(values, add))
        assert result is not None
        return result

    outcomes = _resume_outcomes(run)
    assert all(tag == "return" and isinstance(value, list) for tag, value in outcomes), outcomes
    assert 5 in outcomes[0][1], outcomes
    assert 14 in outcomes[1][1], outcomes


@_case("normal", "functools_reduce_pending_suffix")
def _functools_reduce_pending_suffix() -> None:
    def run(choose: Choose) -> int:
        def add(left: int, right: int) -> int:
            if right == 2:
                return left + cast(int, choose())
            return left + right

        result: int | None = None
        with wind_range(1, 4) as values:
            result = functools.reduce(add, values)
        assert result is not None
        return result

    _assert_equal(_resume_outcomes(run), _returns(5, 14))


def _operator_accessor_case(accessor_name: str) -> None:
    def run(choose: Choose) -> Any:
        class Target:
            def __getattribute__(self, name: str) -> Any:
                if name == "value":
                    return cast(int, choose()) + 100
                return object.__getattribute__(self, name)

            def __getitem__(self, key: str) -> int:
                assert key == "value"
                return cast(int, choose()) + 100

            def method(self, offset: int) -> int:
                return cast(int, choose()) + offset

        accessor = {
            "attrgetter": operator.attrgetter("value"),
            "itemgetter": operator.itemgetter("value"),
            "methodcaller": operator.methodcaller("method", 100),
        }[accessor_name]
        return accessor(Target())

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


for _name in ("attrgetter", "itemgetter", "methodcaller"):
    _case("normal", f"operator_{_name}")(functools.partial(_operator_accessor_case, _name))


if hasattr(itertools, "batched"):

    @_case("normal", "version_itertools_batched")
    def _version_itertools_batched() -> None:
        def run(choose: Choose) -> list[tuple[int, ...]]:
            return list(cast(Any, itertools.batched(_EffectfulIterable(choose), 1)))

        outcomes = _resume_outcomes(run)
        assert all(tag == "return" and isinstance(value, list) for tag, value in outcomes), outcomes
        assert (1,) in outcomes[0][1], outcomes
        assert (10,) in outcomes[1][1], outcomes


if sys.version_info >= (3, 13):

    @_case("error", "version_itertools_batched_strict")
    def _version_itertools_batched_strict() -> None:
        def run(choose: Choose) -> list[tuple[int, ...]]:
            class Items:
                def __iter__(self) -> Any:
                    return iter(cast(tuple[int, ...], choose()))

            return list(itertools.batched(Items(), 2, strict=True))

        _assert_equal(
            _resume_outcomes(run, ((1, 2), (10,))),
            [("return", [(1, 2)]), ("raise", "ValueError")],
        )


if sys.version_info >= (3, 14):

    @_case("error", "version_map_strict")
    def _version_map_strict() -> None:
        def run(choose: Choose) -> list[int]:
            class Items:
                def __iter__(self) -> Any:
                    return iter(cast(tuple[int, ...], choose()))

            return list(map(operator.add, Items(), (100, 200), strict=True))

        _assert_equal(
            _resume_outcomes(run, ((1, 2), (10,))),
            [("return", [101, 202]), ("raise", "ValueError")],
        )

    @_case("normal", "version_sum_complex_fast_path")
    def _version_sum_complex_fast_path() -> None:
        def run(choose: Choose) -> complex:
            return sum(map(lambda _item: cast(complex, choose()), (None,)), 100 + 200j)

        _assert_equal(
            _resume_outcomes(run, (1 + 2j, 10 + 20j)),
            _returns(101 + 202j, 110 + 220j),
        )


@_case("normal", "version_hashable_slice_component")
def _version_hashable_slice_component() -> None:
    class FixedComponent:
        def __init__(self, value: int) -> None:
            self.value = value

        def __hash__(self) -> int:
            return self.value

    def run(choose: Choose) -> int:
        class Component:
            def __hash__(self) -> int:
                return cast(int, choose())

        return hash(slice(Component(), None, None))

    expected = (hash(slice(FixedComponent(1), None, None)), hash(slice(FixedComponent(10), None, None)))
    _assert_equal(_resume_outcomes(run), _returns(*expected))


if sys.version_info < (3, 14):

    @_case("normal", "version_int_trunc_fallback")
    def _version_int_trunc_fallback() -> None:
        def run(choose: Choose) -> int:
            class Truncatable:
                def __trunc__(self) -> int:
                    return cast(int, choose())

            return int(Truncatable())

        _assert_equal(_resume_outcomes(run), _returns(1, 10))


class _ExpectedCallbackError(Exception):
    pass


def _raise_then_return(choose: Choose) -> int:
    value = choose()
    if value == "raise":
        raise _ExpectedCallbackError
    return cast(int, value)


@_case("error", "sum_map_exception_then_success")
def _sum_map_exception_then_success() -> None:
    def run(choose: Choose) -> int:
        return sum(map(lambda _item: _raise_then_return(choose), (None,)), 100)

    _assert_equal(
        _resume_outcomes(run, ("raise", 10)),
        [("raise", "_ExpectedCallbackError"), ("return", 110)],
    )


@_case("error", "property_exception_then_success")
def _property_exception_then_success() -> None:
    def run(choose: Choose) -> int:
        class Target:
            @property
            def value(self) -> int:
                return _raise_then_return(choose) + 100

        return Target().value

    _assert_equal(
        _resume_outcomes(run, ("raise", 10)),
        [("raise", "_ExpectedCallbackError"), ("return", 110)],
    )


@_case("error", "getitem_success_exception_success")
def _getitem_success_exception_success() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __getitem__(self, key: str) -> int:
                return _raise_then_return(choose) + 100

        return Target()["key"]

    _assert_equal(
        _resume_outcomes(run, (1, "raise", 10)),
        [("return", 101), ("raise", "_ExpectedCallbackError"), ("return", 110)],
    )


@_case("error", "len_type_error_then_success")
def _len_type_error_then_success() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __len__(self) -> Any:
                value = choose()
                return "invalid" if value == "invalid" else value

        return len(Target())

    _assert_equal(
        _resume_outcomes(run, ("invalid", 10)),
        [("raise", "TypeError"), ("return", 10)],
    )


@_case("corner", "three_shot_nested_sum_map")
def _three_shot_nested_sum_map() -> None:
    def run(choose: Choose) -> int:
        return sum(map(lambda _item: cast(int, choose()), (None,)), 100)

    _assert_equal(_resume_outcomes(run, (1, 2, 3)), _returns(101, 102, 103))


@_case("corner", "nested_all_sum_map_boundaries")
def _nested_all_sum_map_boundaries() -> None:
    def run(choose: Choose) -> bool:
        total = sum(map(lambda _item: cast(int, choose()), (None,)), 100)
        return all((total > 100,))

    outcomes = _resume_outcomes(run, (0, 10))
    _assert_equal(outcomes, _returns(False, True))
    assert all(type(value) is bool for _, value in outcomes)


@_case("corner", "reduce_suffix_with_restorable_iterator")
def _reduce_suffix_with_restorable_iterator() -> None:
    def run(choose: Choose) -> int:
        def add(left: int, right: int) -> int:
            if right == 1:
                return left + cast(int, choose())
            return left + right

        result: int | None = None
        with wind_range(1, 3) as values:
            result = functools.reduce(add, values, 0)
        assert result is not None
        return result

    _assert_equal(_resume_outcomes(run, (1, 10)), _returns(3, 12))


@_case("corner", "handler_aborts_without_resuming_c_boundary")
def _handler_aborts_without_resuming_c_boundary() -> None:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def abort(_k: Any) -> str:
        return "aborted"

    _assert_equal(handler(lambda: sum(map(lambda _item: cast(int, choose()), (None,)), 100)), "aborted")


def _run_case_in_subprocess(case_name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", case_name],
        text=True,
        capture_output=True,
        timeout=10,
    )


def _case_ids(kind: CaseKind) -> list[str]:
    return [name for name, (case_kind, _) in _CASES.items() if case_kind == kind]


@pytest.mark.parametrize("case_name", _case_ids("normal"))
def test_normal_c_continuation_boundary(case_name: str) -> None:
    result = _run_case_in_subprocess(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("case_name", _case_ids("error"))
def test_c_continuation_boundary_error_isolated_per_shot(case_name: str) -> None:
    result = _run_case_in_subprocess(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("case_name", _case_ids("corner"))
def test_c_continuation_boundary_corner_case(case_name: str) -> None:
    result = _run_case_in_subprocess(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_boundaries.py --case CASE_NAME")
    _CASES[sys.argv[2]][1]()
