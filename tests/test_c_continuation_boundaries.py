"""Acceptance tests for multi-shot continuations crossing CPython C calls.

Each case runs in its own process because an unsupported continuation shape can
terminate the interpreter instead of raising a Python exception.  The child
assertions describe the required behavior; the parent assertion preserves the
child's faulthandler output when that contract is violated.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator as ABCIterator
import functools
import importlib.abc
import importlib.util
import io
import itertools
import os
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


@_case("error", "itertools_chain_invalid_outer_iterator_isolated_per_shot")
def _itertools_chain_invalid_outer_iterator_isolated_per_shot() -> None:
    def run(choose: Choose) -> list[object]:
        class Outer:
            def __iter__(self) -> Any:
                value = choose()
                return 42 if value == "invalid" else iter(())

        return list(itertools.chain.from_iterable(Outer()))

    _assert_equal(
        _resume_outcomes(run, ("invalid", "valid")),
        [("raise", "TypeError"), ("return", [])],
    )


@_case("error", "itertools_chain_active_error_isolated_per_shot")
def _itertools_chain_active_error_isolated_per_shot() -> None:
    def run(choose: Choose) -> list[int]:
        first = map(lambda _item: _raise_then_return(choose), (None,))
        return list(itertools.chain(first, (100,)))

    _assert_equal(
        _resume_outcomes(run, ("raise", 10)),
        [("raise", "_ExpectedCallbackError"), ("return", [10, 100])],
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


@_case("normal", "next_without_default_effectful_iterator")
def _next_without_default_effectful_iterator() -> None:
    def run(choose: Choose) -> int:
        class Iterator:
            def __iter__(self) -> Iterator:
                return self

            def __next__(self) -> int:
                return cast(int, choose()) + 100

        return next(Iterator())

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


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


@_case("corner", "anext_default_on_restored_stop_without_event_loop")
def _anext_default_on_restored_stop_without_event_loop() -> None:
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
        finally:
            coroutine.close()
        raise AssertionError("coroutine unexpectedly suspended")

    _assert_equal(_resume_outcomes(run, (1, 0)), _returns(42, 99))


@_case("normal", "anext_without_default_without_event_loop")
def _anext_without_default_without_event_loop() -> None:
    def run(choose: Choose) -> int:
        class AsyncIterator:
            def __aiter__(self):
                return self

            async def __anext__(self) -> int:
                return cast(int, choose()) + 100

        async def consume() -> int:
            return await anext(AsyncIterator())

        coroutine = consume()
        try:
            coroutine.send(None)
        except StopIteration as completed:
            return cast(int, completed.value)
        raise AssertionError("coroutine unexpectedly suspended")

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("error", "anext_without_default_stop_isolated_per_shot")
def _anext_without_default_stop_isolated_per_shot() -> None:
    def run(choose: Choose) -> int:
        class AsyncIterator:
            def __aiter__(self):
                return self

            async def __anext__(self) -> int:
                if choose() == 0:
                    raise StopAsyncIteration
                return 42

        async def consume() -> int:
            return await anext(AsyncIterator())

        coroutine = consume()
        try:
            coroutine.send(None)
        except StopIteration as completed:
            return cast(int, completed.value)
        finally:
            coroutine.close()
        raise AssertionError("coroutine unexpectedly suspended")

    _assert_equal(
        _resume_outcomes(run, (1, 0)),
        [("return", 42), ("raise", "StopAsyncIteration")],
    )


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


@_case("normal", "builtin_ascii_effectful_repr")
def _builtin_ascii_effectful_repr() -> None:
    def run(choose: Choose) -> str:
        class Target:
            def __repr__(self) -> str:
                return f"é{choose()}"

        return ascii(Target())

    _assert_equal(_resume_outcomes(run), _returns("\\xe91", "\\xe910"))


@_case("error", "builtin_ascii_success_invalid_success")
def _builtin_ascii_success_invalid_success() -> None:
    invalid = object()

    def run(choose: Choose) -> str:
        class Target:
            def __repr__(self) -> str:
                return cast(str, choose())

        return ascii(Target())

    _assert_equal(
        _resume_outcomes(run, ("é1", invalid, "é10")),
        [("return", "\\xe91"), ("raise", "TypeError"), ("return", "\\xe910")],
    )


@_case("normal", "builtin_getattr_effectful_getattribute")
def _builtin_getattr_effectful_getattribute() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __getattribute__(self, name: str) -> Any:
                if name == "value":
                    return choose()
                return object.__getattribute__(self, name)

        return cast(int, getattr(Target(), "value"))

    _assert_equal(_resume_outcomes(run), _returns(1, 10))


@_case("error", "builtin_getattr_default_and_error_isolated_per_shot")
def _builtin_getattr_default_and_error_isolated_per_shot() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __getattribute__(self, name: str) -> Any:
                if name == "value":
                    outcome = choose()
                    if outcome == 0:
                        raise AttributeError(name)
                    if outcome == 1:
                        raise RuntimeError("boom")
                    return 42
                return object.__getattribute__(self, name)

        return cast(int, getattr(Target(), "value", 99))

    _assert_equal(
        _resume_outcomes(run, (2, 0, 1)),
        [("return", 42), ("return", 99), ("raise", "RuntimeError")],
    )


@_case("normal", "builtin_hasattr_effectful_getattribute")
def _builtin_hasattr_effectful_getattribute() -> None:
    def run(choose: Choose) -> bool:
        class Target:
            def __getattribute__(self, name: str) -> Any:
                if name == "value":
                    if choose():
                        return 42
                    raise AttributeError(name)
                return object.__getattribute__(self, name)

        return hasattr(Target(), "value")

    _assert_equal(_resume_outcomes(run, (0, 1)), _returns(False, True))


@_case("error", "builtin_hasattr_attribute_error_runtime_error_success")
def _builtin_hasattr_attribute_error_runtime_error_success() -> None:
    def run(choose: Choose) -> bool:
        class Target:
            def __getattribute__(self, name: str) -> Any:
                if name == "value":
                    outcome = choose()
                    if outcome == 0:
                        raise AttributeError(name)
                    if outcome == 1:
                        raise RuntimeError("boom")
                    return 42
                return object.__getattribute__(self, name)

        return hasattr(Target(), "value")

    _assert_equal(
        _resume_outcomes(run, (0, 1, 2)),
        [("return", False), ("raise", "RuntimeError"), ("return", True)],
    )


@_case("normal", "builtin_dir_effectful_dir")
def _builtin_dir_effectful_dir() -> None:
    def run(choose: Choose) -> list[str]:
        class Target:
            def __dir__(self) -> list[str]:
                return [f"value_{choose()}"]

        return dir(Target())

    _assert_equal(_resume_outcomes(run), _returns(["value_1"], ["value_10"]))


@_case("normal", "builtin_divmod_effectful_protocol")
def _builtin_divmod_effectful_protocol() -> None:
    def run(choose: Choose) -> tuple[int, int]:
        class Target:
            def __divmod__(self, _other: object) -> tuple[int, int]:
                return cast(int, choose()), 2

        return divmod(Target(), 3)

    _assert_equal(_resume_outcomes(run), _returns((1, 2), (10, 2)))


@_case("normal", "builtin_isinstance_effectful_instancecheck")
def _builtin_isinstance_effectful_instancecheck() -> None:
    def run(choose: Choose) -> bool:
        class Meta(type):
            def __instancecheck__(cls, _instance: object) -> bool:
                return bool(choose())

        class Target(metaclass=Meta):
            pass

        return isinstance(object(), Target)

    _assert_equal(_resume_outcomes(run, (0, 1)), _returns(False, True))


@_case("normal", "builtin_issubclass_effectful_subclasscheck")
def _builtin_issubclass_effectful_subclasscheck() -> None:
    def run(choose: Choose) -> bool:
        class Meta(type):
            def __subclasscheck__(cls, _subclass: type[object]) -> bool:
                return bool(choose())

        class Target(metaclass=Meta):
            pass

        class Candidate:
            pass

        return issubclass(Candidate, Target)

    _assert_equal(_resume_outcomes(run, (0, 1)), _returns(False, True))


@_case("normal", "builtin_memoryview_effectful_buffer")
def _builtin_memoryview_effectful_buffer() -> None:
    def run(choose: Choose) -> bytes:
        class Buffer:
            def __buffer__(self, _flags: int) -> memoryview:
                return memoryview(bytes((cast(int, choose()),)))

            def __release_buffer__(self, _view: memoryview) -> None:
                pass

        return memoryview(Buffer()).tobytes()

    _assert_equal(_resume_outcomes(run), _returns(b"\x01", b"\x0a"))


@_case("normal", "builtin_pow_three_argument_effectful_protocol")
def _builtin_pow_three_argument_effectful_protocol() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __pow__(self, _exponent: object, _modulus: object) -> int:
                return cast(int, choose())

        return pow(Target(), 2, 3)

    _assert_equal(_resume_outcomes(run), _returns(1, 10))


@_case("normal", "builtin_vars_effectful_dict_lookup")
def _builtin_vars_effectful_dict_lookup() -> None:
    def run(choose: Choose) -> dict[str, int]:
        class Target:
            def __getattribute__(self, name: str) -> Any:
                if name == "__dict__":
                    return {"value": cast(int, choose())}
                return object.__getattribute__(self, name)

        return vars(Target())

    _assert_equal(_resume_outcomes(run), _returns({"value": 1}, {"value": 10}))


@_case("normal", "builtin_print_effectful_write")
def _builtin_print_effectful_write() -> None:
    def run(choose: Choose) -> None:
        class Output:
            def write(self, text: str) -> int:
                if text == "value":
                    return cast(int, choose())
                return len(text)

        return print("value", file=Output())

    _assert_equal(_resume_outcomes(run), _returns(None, None))


@_case("error", "builtin_print_write_error_isolated_per_shot")
def _builtin_print_write_error_isolated_per_shot() -> None:
    def run(choose: Choose) -> None:
        class Output:
            def write(self, text: str) -> int:
                if text != "value":
                    return len(text)
                outcome = choose()
                if outcome == "raise":
                    raise RuntimeError("boom")
                return cast(int, outcome)

        return print("value", file=Output())

    _assert_equal(
        _resume_outcomes(run, (1, "raise", 10)),
        [("return", None), ("raise", "RuntimeError"), ("return", None)],
    )


@_case("corner", "builtin_print_resumes_end_after_effectful_write")
def _builtin_print_resumes_end_after_effectful_write() -> None:
    def run(choose: Choose) -> None:
        class Output:
            fail_end = False

            def write(self, text: str) -> int:
                if text == "value":
                    self.fail_end = choose() == 10
                elif self.fail_end:
                    raise RuntimeError("end reached")
                return len(text)

        return print("value", file=Output())

    _assert_equal(
        _resume_outcomes(run),
        [("return", None), ("raise", "RuntimeError")],
    )


@_case("corner", "builtin_print_resumes_write_after_effectful_str")
def _builtin_print_resumes_write_after_effectful_str() -> None:
    def run(choose: Choose) -> None:
        class Value:
            def __str__(self) -> str:
                return f"value={choose()}"

        class Output:
            def write(self, text: str) -> int:
                if text == "value=10":
                    raise RuntimeError("write reached")
                return len(text)

        return print(Value(), file=Output())

    _assert_equal(
        _resume_outcomes(run),
        [("return", None), ("raise", "RuntimeError")],
    )


@_case("corner", "builtin_print_effectful_flush")
def _builtin_print_effectful_flush() -> None:
    def run(choose: Choose) -> None:
        class Output:
            def write(self, text: str) -> int:
                return len(text)

            def flush(self) -> int:
                return cast(int, choose())

        return print("value", file=Output(), flush=True)

    _assert_equal(_resume_outcomes(run), _returns(None, None))


@_case("error", "builtin_print_effectful_flush_precedes_invalid_sep")
def _builtin_print_effectful_flush_precedes_invalid_sep() -> None:
    def run(choose: Choose) -> str:
        class Flush:
            def __bool__(self) -> bool:
                return bool(choose())

        class Output:
            def write(self, text: str) -> int:
                return len(text)

        try:
            print("value", sep=object(), file=Output(), flush=Flush())
        except TypeError:
            return "TypeError"
        return "returned"

    _assert_equal(_resume_outcomes(run), _returns("TypeError", "TypeError"))


@_case("error", "builtin_print_effectful_flush_precedes_invalid_end")
def _builtin_print_effectful_flush_precedes_invalid_end() -> None:
    def run(choose: Choose) -> str:
        class Flush:
            def __bool__(self) -> bool:
                return bool(choose())

        class Output:
            def write(self, text: str) -> int:
                return len(text)

        try:
            print("value", end=object(), file=Output(), flush=Flush())
        except TypeError:
            return "TypeError"
        return "returned"

    _assert_equal(_resume_outcomes(run), _returns("TypeError", "TypeError"))


@_case("normal", "builtin_open_effectful_path_protocol")
def _builtin_open_effectful_path_protocol() -> None:
    paths = (str(Path(__file__).resolve()), str(Path(__file__).resolve().parents[1] / "README.md"))
    expected = tuple(Path(path).read_text(encoding="utf-8")[0] for path in paths)

    def run(choose: Choose) -> str:
        class Target:
            def __fspath__(self) -> str:
                return cast(str, choose())

        with open(Target(), encoding="utf-8") as stream:
            return stream.read(1)

    _assert_equal(_resume_outcomes(run, paths), _returns(*expected))


@_case("error", "builtin_open_invalid_path_isolated_per_shot")
def _builtin_open_invalid_path_isolated_per_shot() -> None:
    valid_paths = (
        str(Path(__file__).resolve()),
        str(Path(__file__).resolve().parents[1] / "README.md"),
    )
    invalid = object()
    expected = tuple(Path(path).read_text(encoding="utf-8")[0] for path in valid_paths)

    def run(choose: Choose) -> str:
        class Target:
            def __fspath__(self) -> str:
                return cast(str, choose())

        with open(Target(), encoding="utf-8") as stream:
            return stream.read(1)

    _assert_equal(
        _resume_outcomes(run, (valid_paths[0], invalid, valid_paths[1])),
        [("return", expected[0]), ("raise", "TypeError"), ("return", expected[1])],
    )


@_case("corner", "builtin_open_effectful_opener")
def _builtin_open_effectful_opener() -> None:
    path = str(Path(__file__).resolve())
    expected = Path(path).read_text(encoding="utf-8")[0]

    def run(choose: Choose) -> str:
        def opener(opener_path: str, flags: int) -> int:
            choose()
            return os.open(opener_path, flags)

        with open(path, encoding="utf-8", opener=opener) as stream:
            return stream.read(1)

    _assert_equal(_resume_outcomes(run), _returns(expected, expected))


@_case("normal", "builtin_input_effectful_readline")
def _builtin_input_effectful_readline() -> None:
    def run(choose: Choose) -> str:
        class Input:
            def readline(self) -> str:
                return f"value={choose()}\n"

        original_stdin = sys.stdin
        original_stdout = sys.stdout
        try:
            sys.stdin = cast(Any, Input())
            sys.stdout = io.StringIO()
            return input("prompt: ")
        finally:
            sys.stdin = original_stdin
            sys.stdout = original_stdout

    _assert_equal(_resume_outcomes(run), _returns("value=1", "value=10"))


@_case("error", "builtin_input_newline_eof_and_crlf_isolated_per_shot")
def _builtin_input_newline_eof_and_crlf_isolated_per_shot() -> None:
    def run(choose: Choose) -> str:
        class Input:
            def readline(self) -> str:
                return cast(str, choose())

        original_stdin = sys.stdin
        original_stdout = sys.stdout
        try:
            sys.stdin = cast(Any, Input())
            sys.stdout = io.StringIO()
            return input()
        finally:
            sys.stdin = original_stdin
            sys.stdout = original_stdout

    _assert_equal(
        _resume_outcomes(run, ("one\n", "", "ten\r\n")),
        [
            ("return", "one"),
            ("raise", "EOFError"),
            ("return", "ten\r"),
        ],
    )


@_case("corner", "builtin_input_effectful_prompt_write_then_readline")
def _builtin_input_effectful_prompt_write_then_readline() -> None:
    def run(choose: Choose) -> str:
        class Input:
            def readline(self) -> str:
                return "answer\n"

        class Output:
            def write(self, text: str) -> int:
                if text == "prompt: ":
                    return cast(int, choose())
                return len(text)

            def flush(self) -> None:
                pass

        original_stdin = sys.stdin
        original_stdout = sys.stdout
        try:
            sys.stdin = cast(Any, Input())
            sys.stdout = cast(Any, Output())
            return input("prompt: ")
        finally:
            sys.stdin = original_stdin
            sys.stdout = original_stdout

    _assert_equal(_resume_outcomes(run), _returns("answer", "answer"))


@_case("normal", "builtin_eval_effectful_expression")
def _builtin_eval_effectful_expression() -> None:
    def run(choose: Choose) -> int:
        return cast(int, eval("choose() + 100", {"choose": choose}))

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "builtin_exec_effectful_code")
def _builtin_exec_effectful_code() -> None:
    def run(choose: Choose) -> tuple[None, int]:
        namespace: dict[str, Any] = {"choose": choose}
        result = exec("value = choose() + 100", namespace)
        return result, cast(int, namespace["value"])

    _assert_equal(_resume_outcomes(run), _returns((None, 101), (None, 110)))


@_case("normal", "builtin_build_class_effectful_set_name")
def _builtin_build_class_effectful_set_name() -> None:
    def run(choose: Choose) -> int:
        class Descriptor:
            saved = 0

            def __set_name__(self, _owner: type[object], _name: str) -> None:
                self.saved = cast(int, choose()) + 100

            def __get__(self, _instance: object, _owner: type[object]) -> int:
                return self.saved

        class Target:
            value = Descriptor()

        return Target.value

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "builtin_build_class_effectful_class_body")
def _builtin_build_class_effectful_class_body() -> None:
    def run(choose: Choose) -> int:
        class Target:
            value = choose()

        return cast(int, Target.value)

    _assert_equal(_resume_outcomes(run), _returns(1, 10))


@_case("normal", "builtin_build_class_effectful_init_subclass")
def _builtin_build_class_effectful_init_subclass() -> None:
    def run(choose: Choose) -> int:
        class Base:
            @classmethod
            def __init_subclass__(cls) -> None:
                cls.value = choose()

        class Target(Base):
            pass

        return cast(int, Target.value)

    _assert_equal(_resume_outcomes(run), _returns(1, 10))


@_case("normal", "builtin_import_effectful_finder")
def _builtin_import_effectful_finder() -> None:
    module_name = "_aleff_effectful_import_case"

    def run(choose: Choose) -> int:
        class Loader(importlib.abc.Loader):
            def __init__(self, value: int) -> None:
                self.value = value

            def create_module(self, _spec: Any) -> None:
                return None

            def exec_module(self, module: Any) -> None:
                module.value = self.value

        class Finder(importlib.abc.MetaPathFinder):
            def find_spec(self, fullname: str, _path: Any, _target: Any = None) -> Any:
                if fullname != module_name:
                    return None
                value = cast(int, choose())
                return importlib.util.spec_from_loader(fullname, Loader(value))

        finder = Finder()
        sys.meta_path.insert(0, finder)
        try:
            module = __import__(module_name)
            return cast(int, module.value)
        finally:
            if finder in sys.meta_path:
                sys.meta_path.remove(finder)
            sys.modules.pop(module_name, None)

    _assert_equal(_resume_outcomes(run), _returns(1, 10))


@_case("error", "builtin_import_finder_error_isolated_per_shot")
def _builtin_import_finder_error_isolated_per_shot() -> None:
    module_name = "_aleff_effectful_import_error_case"

    def run(choose: Choose) -> int:
        class Loader(importlib.abc.Loader):
            def create_module(self, _spec: Any) -> None:
                return None

            def exec_module(self, module: Any) -> None:
                module.value = 42

        class Finder(importlib.abc.MetaPathFinder):
            def find_spec(self, fullname: str, _path: Any, _target: Any = None) -> Any:
                if fullname != module_name:
                    return None
                if choose() == "raise":
                    raise RuntimeError("boom")
                return importlib.util.spec_from_loader(fullname, Loader())

        finder = Finder()
        sys.meta_path.insert(0, finder)
        try:
            module = __import__(module_name)
            return cast(int, module.value)
        finally:
            if finder in sys.meta_path:
                sys.meta_path.remove(finder)
            sys.modules.pop(module_name, None)

    _assert_equal(
        _resume_outcomes(run, ("ok", "raise", "ok")),
        [("return", 42), ("raise", "RuntimeError"), ("return", 42)],
    )


@_case("corner", "builtin_import_effectful_loader_without_global_lock")
def _builtin_import_effectful_loader_without_global_lock() -> None:
    module_name = "_aleff_effectful_import_loader_case"

    def run(choose: Choose) -> int:
        class Loader(importlib.abc.Loader):
            def create_module(self, _spec: Any) -> None:
                return None

            def exec_module(self, module: Any) -> None:
                module.value = choose()

        class Finder(importlib.abc.MetaPathFinder):
            def find_spec(self, fullname: str, _path: Any, _target: Any = None) -> Any:
                if fullname != module_name:
                    return None
                return importlib.util.spec_from_loader(fullname, Loader())

        finder = Finder()
        sys.meta_path.insert(0, finder)
        try:
            module = __import__(module_name)
            return cast(int, module.value)
        finally:
            if finder in sys.meta_path:
                sys.meta_path.remove(finder)
            sys.modules.pop(module_name, None)

    _assert_equal(_resume_outcomes(run), _returns(1, 10))


@_case("normal", "builtin_breakpoint_effectful_hook")
def _builtin_breakpoint_effectful_hook() -> None:
    def run(choose: Choose) -> int:
        original_hook = sys.breakpointhook
        try:
            sys.breakpointhook = lambda: cast(int, choose()) + 100
            return cast(int, breakpoint())
        finally:
            sys.breakpointhook = original_hook

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("normal", "builtin_type_three_argument_effectful_set_name")
def _builtin_type_three_argument_effectful_set_name() -> None:
    def run(choose: Choose) -> int:
        class Descriptor:
            saved = 0

            def __set_name__(self, _owner: type[object], _name: str) -> None:
                self.saved = cast(int, choose()) + 100

            def __get__(self, _instance: object, _owner: type[object]) -> int:
                return self.saved

        target = type("Target", (), {"value": Descriptor()})
        return cast(int, target.value)

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


def _type_construction_suffix_case(choose: Choose, use_build_class: bool) -> tuple[int, int]:
    class First:
        saved = 0

        def __set_name__(self, _owner: type[object], _name: str) -> None:
            self.saved = cast(int, choose())

    class Second:
        saved = 0

        def __init__(self, first: First) -> None:
            self.first = first

        def __set_name__(self, _owner: type[object], _name: str) -> None:
            self.saved = self.first.saved

    first = First()
    second = Second(first)

    class Base:
        @classmethod
        def __init_subclass__(cls) -> None:
            cls.subclass_saved = second.saved

    if use_build_class:

        class Target(Base):
            first_value = first
            second_value = second

    else:
        Target = type(
            "Target",
            (Base,),
            {"first_value": first, "second_value": second},
        )
    return second.saved, cast(int, Target.subclass_saved)


@_case("corner", "builtin_build_class_resumes_set_name_and_init_subclass_suffix")
def _builtin_build_class_resumes_set_name_and_init_subclass_suffix() -> None:
    _assert_equal(
        _resume_outcomes(lambda choose: _type_construction_suffix_case(choose, True)),
        _returns((1, 1), (10, 10)),
    )


@_case("corner", "builtin_type_resumes_set_name_and_init_subclass_suffix")
def _builtin_type_resumes_set_name_and_init_subclass_suffix() -> None:
    _assert_equal(
        _resume_outcomes(lambda choose: _type_construction_suffix_case(choose, False)),
        _returns((1, 1), (10, 10)),
    )


@_case("error", "builtin_type_set_name_error_isolated_per_shot")
def _builtin_type_set_name_error_isolated_per_shot() -> None:
    def run(choose: Choose) -> int:
        class Descriptor:
            saved = 0

            def __set_name__(self, _owner: type[object], _name: str) -> None:
                outcome = choose()
                if outcome == "raise":
                    raise RuntimeError("boom")
                self.saved = cast(int, outcome)

            def __get__(self, _instance: object, _owner: type[object]) -> int:
                return self.saved

        target = type("Target", (), {"value": Descriptor()})
        return cast(int, target.value)

    _assert_equal(
        _resume_outcomes(run, (1, "raise", 10)),
        [("return", 1), ("raise", "RuntimeError"), ("return", 10)],
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


@_case("normal", "list_sort_effectful_key")
def _list_sort_effectful_key() -> None:
    def run(choose: Choose) -> tuple[int, ...]:
        target = [1, 2]
        target.sort(key=lambda value: choose() if value == 1 else 5)
        return tuple(target)

    _assert_equal(_resume_outcomes(run, (0, 10)), _returns((1, 2), (2, 1)))


@_case("corner", "list_sort_completed_shot_is_not_reported_as_mutation")
def _list_sort_completed_shot_is_not_reported_as_mutation() -> None:
    def run(choose: Choose) -> tuple[int, ...]:
        target = [1, 2]
        target.sort(key=lambda value: choose() if value == 1 else 5)
        return tuple(target)

    _assert_equal(
        _resume_outcomes(run, (0, 10, 0)),
        _returns((1, 2), (2, 1), (1, 2)),
    )


def _sort_comparison_effect_case(
    operation: str,
    values: tuple[int, ...],
    target_pair: tuple[int, int],
) -> None:
    expected = tuple(sorted(values))

    def run(choose: Choose) -> tuple[int, ...]:
        class Key:
            def __init__(self, value: int, index: int) -> None:
                self.value = value
                self.index = index

            def __lt__(self, other: Key) -> bool:
                if (self.index, other.index) == target_pair:
                    selected = choose()
                    assert selected in {"first", "second"}
                return self.value < other.value

        indexed = list(enumerate(values))

        def key(item: tuple[int, int]) -> Key:
            return Key(item[1], item[0])

        if operation == "sorted":
            result = sorted(indexed, key=key)
        else:
            result = indexed
            method_result = result.sort(key=key)
            assert method_result is None
        return tuple(value for _, value in result)

    _assert_equal(
        _resume_outcomes(run, ("first", "second")),
        _returns(expected, expected),
    )


_SORT_RUN_PHASE_CASES: tuple[tuple[str, tuple[int, ...], tuple[int, int], tuple[int, int], CaseKind], ...] = (
    ("ascending_scan", (1, 3, 2), (1, 0), (3, 12), "normal"),
    ("descending_scan", (3, 2, 1), (2, 1), (3, 12), "normal"),
    ("binary_insertion", (1, 3, 2), (2, 0), (3, 12), "normal"),
    (
        "merge",
        tuple(range(40)) + tuple(range(-40, 0)),
        (40, 0),
        (3, 12),
        "normal",
    ),
    ("ascending_drop_disambiguation", (1, 3, 2), (0, 1), (3, 13), "corner"),
    ("equal_descending_run", (3, 2, 2, 1), (1, 2), (3, 13), "corner"),
    (
        "post_reverse_ascending_suffix",
        (3, 2, 1, 3, 4, 0),
        (2, 3),
        (3, 13),
        "corner",
    ),
)

for _operation in ("sorted", "list_sort"):
    for _phase, _values, _target_pair, _minimum_version, _kind in _SORT_RUN_PHASE_CASES:
        if sys.version_info >= _minimum_version:
            _case(_kind, f"{_operation}_{_phase}_comparison_multishot")(
                functools.partial(
                    _sort_comparison_effect_case,
                    _operation,
                    _values,
                    _target_pair,
                )
            )


def _sort_disambiguation_error_effect_case(operation: str) -> None:
    class SelectedSortComparisonError(Exception):
        pass

    def run(choose: Choose) -> tuple[int, ...]:
        class Key:
            def __init__(self, value: int, index: int) -> None:
                self.value = value
                self.index = index

            def __lt__(self, other: Key) -> bool:
                if (self.index, other.index) == (0, 1):
                    if choose() == "raise":
                        raise SelectedSortComparisonError
                return self.value < other.value

        values = list(enumerate((1, 3, 2)))

        def key(item: tuple[int, int]) -> Key:
            return Key(item[1], item[0])

        if operation == "sorted":
            result = sorted(values, key=key)
        else:
            result = values
            result.sort(key=key)
        return tuple(value for _, value in result)

    _assert_equal(
        _resume_outcomes(run, ("raise", "continue")),
        [("raise", "SelectedSortComparisonError"), ("return", (1, 2, 3))],
    )


if sys.version_info >= (3, 13):
    for _operation in ("sorted", "list_sort"):
        _case("error", f"{_operation}_disambiguation_error_isolated_per_shot")(
            functools.partial(_sort_disambiguation_error_effect_case, _operation)
        )


def _sequence_search_case(sequence_type: type[list[Any]] | type[tuple[Any, ...]], operation: str) -> None:
    def run(choose: Choose) -> Any:
        class Item:
            def __eq__(self, _other: object) -> Any:
                return choose()

        sequence = sequence_type((Item(),))
        if operation == "count":
            return sequence.count(object())
        if operation == "index":
            return sequence.index(object())
        if operation == "contains":
            return object() in sequence
        if operation == "eq":
            return sequence == sequence_type((object(),))
        return sequence != sequence_type((object(),))

    values = (0, 2) if operation != "index" else (1, 0)
    if operation == "index":
        expected: list[tuple[str, Any]] = [("return", 0), ("raise", "ValueError")]
    elif operation == "count":
        expected = _returns(0, 1)
    elif operation in {"contains", "eq"}:
        expected = _returns(False, True)
    else:
        expected = _returns(True, False)
    _assert_equal(_resume_outcomes(run, values), expected)


for _sequence_type, _sequence_name in ((list, "list"), (tuple, "tuple")):
    for _operation in ("count", "index", "contains", "eq", "ne"):
        _case(
            "error" if _operation == "index" else "normal",
            f"{_sequence_name}_{_operation}_effectful_equality_search",
        )(functools.partial(_sequence_search_case, _sequence_type, _operation))


def _sequence_contains_suffix_case(sequence_type: type[list[Any]] | type[tuple[Any, ...]]) -> None:
    def run(choose: Choose) -> bool:
        class Item:
            def __eq__(self, _other: object) -> Any:
                return choose()

        target = object()
        sequence = sequence_type((Item(), target))
        return target in sequence

    _assert_equal(_resume_outcomes(run, (0, 0)), _returns(True, True))


for _sequence_type, _sequence_name in ((list, "list"), (tuple, "tuple")):
    _case("corner", f"{_sequence_name}_contains_resumes_suffix")(
        functools.partial(_sequence_contains_suffix_case, _sequence_type)
    )


@_case("error", "list_remove_effectful_equality_isolated_per_shot")
def _list_remove_effectful_equality_isolated_per_shot() -> None:
    def run(choose: Choose) -> tuple[Any, tuple[Any, ...]]:
        class Item:
            def __eq__(self, _other: object) -> Any:
                return choose()

        target: list[Any] = [Item(), "suffix"]
        result = target.remove(object())
        return result, tuple(target)

    outcomes = _resume_outcomes(run, (1, 0))
    assert outcomes[0][0] == "return" and outcomes[0][1][0] is None, outcomes
    assert outcomes[0][1][1] == ("suffix",), outcomes
    assert outcomes[1] == ("raise", "ValueError"), outcomes


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


@_case("normal", "itertools_chain_effectful_iterable")
def _itertools_chain_effectful_iterable() -> None:
    def run(choose: Choose) -> list[int]:
        return list(itertools.chain(_EffectfulIterable(choose), (100,)))

    _assert_equal(_resume_outcomes(run), _returns([1, 100], [10, 100]))


@_case("normal", "itertools_chain_from_iterable_effectful_outer")
def _itertools_chain_from_iterable_effectful_outer() -> None:
    def run(choose: Choose) -> list[int]:
        class Outer:
            def __iter__(self) -> ABCIterator[tuple[int, ...]]:
                value = cast(int, choose())
                return iter(((value,), (100,)))

        return list(itertools.chain.from_iterable(Outer()))

    _assert_equal(_resume_outcomes(run), _returns([1, 100], [10, 100]))


@_case("normal", "itertools_chain_from_iterable_effectful_inner")
def _itertools_chain_from_iterable_effectful_inner() -> None:
    def run(choose: Choose) -> list[int]:
        first = map(lambda _item: cast(int, choose()), (None,))
        return list(itertools.chain.from_iterable((first, (100,))))

    _assert_equal(_resume_outcomes(run), _returns([1, 100], [10, 100]))


@_case("corner", "itertools_chain_from_iterable_descriptor_binding")
def _itertools_chain_from_iterable_descriptor_binding() -> None:
    descriptor = itertools.chain.__dict__["from_iterable"]
    assert type(descriptor).__name__ == "classmethod_descriptor"
    assert list(itertools.chain.from_iterable(((1,), (2,)))) == [1, 2]
    assert list(itertools.chain().from_iterable(((3,), (4,)))) == [3, 4]


@_case("corner", "itertools_chain_empty_inputs")
def _itertools_chain_empty_inputs() -> None:
    empty_chain = cast(ABCIterator[object], itertools.chain())
    empty_from_iterable = cast(ABCIterator[object], itertools.chain.from_iterable(()))
    assert list(empty_chain) == []
    assert list(empty_from_iterable) == []


@_case("corner", "itertools_chain_three_shot_suffix")
def _itertools_chain_three_shot_suffix() -> None:
    def run(choose: Choose) -> list[int]:
        first = map(lambda _item: cast(int, choose()), (None,))
        return list(itertools.chain(first, (100,)))

    _assert_equal(
        _resume_outcomes(run, (1, 2, 3)),
        _returns([1, 100], [2, 100], [3, 100]),
    )


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


@_case("normal", "operator_attrgetter_dotted_multiple_pending_suffix")
def _operator_attrgetter_dotted_multiple_pending_suffix() -> None:
    def run(choose: Choose) -> tuple[int, str]:
        class Nested:
            def __getattribute__(self, name: str) -> Any:
                if name == "value":
                    return cast(int, choose()) + 100
                return object.__getattribute__(self, name)

        class Target:
            nested = Nested()
            label = "tail"

        result = operator.attrgetter("nested.value", "label")(Target())
        assert type(result) is tuple
        assert type(result[0]) is int
        assert type(result[1]) is str
        return result

    _assert_equal(_resume_outcomes(run), _returns((101, "tail"), (110, "tail")))


@_case("error", "operator_attrgetter_error_isolated_after_dotted_prefix")
def _operator_attrgetter_error_isolated_after_dotted_prefix() -> None:
    def run(choose: Choose) -> tuple[int, str]:
        class Nested:
            def __getattribute__(self, name: str) -> Any:
                if name == "value":
                    selected = cast(int, choose())
                    if selected == 0:
                        raise ValueError("selected failure")
                    return selected
                return object.__getattribute__(self, name)

        class Target:
            nested = Nested()
            label = "tail"

        return operator.attrgetter("nested.value", "label")(Target())

    _assert_equal(
        _resume_outcomes(run, (0, 10)),
        [("raise", "ValueError"), ("return", (10, "tail"))],
    )


@_case("corner", "operator_attrgetter_constructor_and_single_result")
def _operator_attrgetter_constructor_and_single_result() -> None:
    with pytest.raises(TypeError):
        operator.attrgetter()
    getter = operator.attrgetter("value")
    result = getter(type("Target", (), {"value": 3})())
    assert type(result) is int
    assert result == 3


@_case("normal", "operator_itemgetter_multiple_pending_suffix")
def _operator_itemgetter_multiple_pending_suffix() -> None:
    def run(choose: Choose) -> tuple[int, str]:
        class Target:
            def __getitem__(self, key: str) -> Any:
                if key == "first":
                    return cast(int, choose()) + 100
                if key == "second":
                    return "tail"
                raise AssertionError(key)

        result = operator.itemgetter("first", "second")(Target())
        assert type(result) is tuple
        assert type(result[0]) is int
        assert type(result[1]) is str
        return result

    _assert_equal(_resume_outcomes(run), _returns((101, "tail"), (110, "tail")))


@_case("error", "operator_itemgetter_error_isolated_after_first_item")
def _operator_itemgetter_error_isolated_after_first_item() -> None:
    def run(choose: Choose) -> tuple[int, str]:
        class Target:
            def __getitem__(self, key: str) -> Any:
                if key == "first":
                    selected = cast(int, choose())
                    if selected == 0:
                        raise ValueError("selected failure")
                    return selected
                if key == "second":
                    return "tail"
                raise AssertionError(key)

        return operator.itemgetter("first", "second")(Target())

    _assert_equal(
        _resume_outcomes(run, (0, 10)),
        [("raise", "ValueError"), ("return", (10, "tail"))],
    )


@_case("corner", "operator_itemgetter_constructor_and_single_result")
def _operator_itemgetter_constructor_and_single_result() -> None:
    with pytest.raises(TypeError):
        operator.itemgetter()
    getter = operator.itemgetter(0)
    result = getter((3,))
    assert type(result) is int
    assert result == 3


@_case("normal", "operator_methodcaller_args_kwargs_and_result")
def _operator_methodcaller_args_kwargs_and_result() -> None:
    def run(choose: Choose) -> tuple[str, int, tuple[Any, ...], dict[str, Any]]:
        class Target:
            def method(self, *args: Any, **kwargs: Any) -> tuple[str, int, tuple[Any, ...], dict[str, Any]]:
                assert args == (5, "suffix")
                assert kwargs == {"flag": True, "offset": 2}
                selected = cast(int, choose())
                return ("ok", selected + 100, args, kwargs)

        caller = operator.methodcaller(
            "method",
            5,
            "suffix",
            flag=True,
            offset=2,
        )
        result = caller(Target())
        assert type(result) is tuple
        assert type(result[0]) is str
        assert type(result[1]) is int
        assert type(result[2]) is tuple
        assert type(result[3]) is dict
        return result

    _assert_equal(
        _resume_outcomes(run),
        _returns(
            ("ok", 101, (5, "suffix"), {"flag": True, "offset": 2}),
            ("ok", 110, (5, "suffix"), {"flag": True, "offset": 2}),
        ),
    )


@_case("error", "operator_methodcaller_error_isolated_after_call")
def _operator_methodcaller_error_isolated_after_call() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def method(self, value: int, *, flag: bool) -> int:
                assert value == 5
                assert flag is True
                selected = cast(int, choose())
                if selected == 0:
                    raise ValueError("selected failure")
                return selected

        return operator.methodcaller("method", 5, flag=True)(Target())

    _assert_equal(
        _resume_outcomes(run, (0, 10)),
        [("raise", "ValueError"), ("return", 10)],
    )


@_case("corner", "operator_methodcaller_constructor_validation")
def _operator_methodcaller_constructor_validation() -> None:
    with pytest.raises(TypeError):
        operator.methodcaller()
    with pytest.raises(TypeError):
        operator.methodcaller(3)


@_case("normal", "functools_partial_call_pending_suffix")
def _functools_partial_call_pending_suffix() -> None:
    def run(choose: Choose) -> int:
        def callback(prefix: int, *, offset: int) -> int:
            return prefix + cast(int, choose()) + offset

        operation = functools.partial(callback, 100, offset=10)
        return operation() + 1000

    _assert_equal(_resume_outcomes(run), _returns(1111, 1120))


@_case("error", "functools_partial_call_exception_isolated_per_shot")
def _functools_partial_call_exception_isolated_per_shot() -> None:
    class ExpectedPartialError(Exception):
        pass

    def run(choose: Choose) -> int:
        def callback() -> int:
            value = choose()
            if value == "raise":
                raise ExpectedPartialError
            return cast(int, value)

        return functools.partial(callback)() + 100

    _assert_equal(
        _resume_outcomes(run, ("raise", 10)),
        [("raise", "ExpectedPartialError"), ("return", 110)],
    )


@_case("corner", "functools_nested_partial_args_and_keywords")
def _functools_nested_partial_args_and_keywords() -> None:
    def run(choose: Choose) -> tuple[int, int, int]:
        def callback(first: int, second: int, *, third: int) -> tuple[int, int, int]:
            return first, second + cast(int, choose()), third

        inner = functools.partial(callback, 100)
        outer = functools.partial(inner, third=1000)
        return outer(10)

    _assert_equal(
        _resume_outcomes(run),
        _returns((100, 11, 1000), (100, 20, 1000)),
    )


if sys.version_info >= (3, 14) and hasattr(functools, "Placeholder"):
    _Placeholder = functools.Placeholder

    @_case("normal", "functools_partial_placeholder_positions")
    def _functools_partial_placeholder_positions() -> None:
        def callback(*args: object) -> tuple[object, ...]:
            result = tuple(args)
            assert type(result) is tuple
            return result

        leading = functools.partial(callback, _Placeholder, 2)(1)
        middle = functools.partial(callback, 1, _Placeholder, 3)(2)
        multiple = functools.partial(callback, _Placeholder, 2, _Placeholder, 4)(1, 3)
        assert type(leading) is tuple
        assert type(middle) is tuple
        assert type(multiple) is tuple
        _assert_equal(leading, (1, 2))
        _assert_equal(middle, (1, 2, 3))
        _assert_equal(multiple, (1, 2, 3, 4))

    @_case("normal", "functools_partial_placeholder_nested_retention_and_fill")
    def _functools_partial_placeholder_nested_retention_and_fill() -> None:
        remove = functools.partial(str.replace, _Placeholder, _Placeholder, "")
        remove_dear = functools.partial(remove, _Placeholder, " dear")
        remove_first_dear = functools.partial(remove_dear, _Placeholder, 1)

        retained = remove_dear("Hello, dear dear world!")
        filled = functools.partial(remove, "Hello, dear dear world!", " dear")()
        first = remove_first_dear("Hello, dear dear world!")
        assert type(retained) is str
        assert type(filled) is str
        assert type(first) is str
        _assert_equal(retained, "Hello, world!")
        _assert_equal(filled, "Hello, world!")
        _assert_equal(first, "Hello, dear world!")

    @_case("normal", "functools_partial_placeholder_effectful_callback_pending_suffix")
    def _functools_partial_placeholder_effectful_callback_pending_suffix() -> None:
        def run(choose: Choose) -> tuple[int, int, int, int, int, str]:
            def callback(
                first: int,
                second: int,
                third: int,
                fourth: int,
            ) -> tuple[int, int, int, int, int]:
                selected = cast(int, choose())
                return first, selected, second, third, fourth

            result = functools.partial(callback, _Placeholder, 20, _Placeholder, 30)(1, 3)
            return (*result, "suffix")

        outcomes = _resume_outcomes(run)
        assert all(tag == "return" and type(value) is tuple for tag, value in outcomes)
        _assert_equal(
            outcomes,
            _returns(
                (1, 1, 20, 3, 30, "suffix"),
                (1, 10, 20, 3, 30, "suffix"),
            ),
        )

    @_case("error", "functools_partial_placeholder_callback_error_isolation")
    def _functools_partial_placeholder_callback_error_isolation() -> None:
        class ExpectedPlaceholderError(Exception):
            pass

        def run(choose: Choose) -> tuple[int, object, int, int, int]:
            def callback(
                first: int,
                second: int,
                third: int,
                fourth: int,
            ) -> tuple[int, object, int, int, int]:
                selected = choose()
                if selected == "raise":
                    raise ExpectedPlaceholderError
                return first, selected, second, third, fourth

            return functools.partial(callback, _Placeholder, 20, _Placeholder, 30)(1, 3)

        _assert_equal(
            _resume_outcomes(run, ("raise", 10)),
            [
                ("raise", "ExpectedPlaceholderError"),
                ("return", (1, 10, 20, 3, 30)),
            ],
        )

    @_case("error", "functools_partial_placeholder_insufficient_arguments")
    def _functools_partial_placeholder_insufficient_arguments() -> None:
        def callback(*args: object) -> tuple[object, ...]:
            return tuple(args)

        operation = functools.partial(callback, _Placeholder, 2, _Placeholder, 4)
        with pytest.raises(TypeError, match="missing positional arguments"):
            operation(1)

    @_case("corner", "functools_partial_placeholder_validation_and_type")
    def _functools_partial_placeholder_validation_and_type() -> None:
        def callback(*args: object) -> tuple[object, ...]:
            return tuple(args)

        with pytest.raises(TypeError, match="trailing Placeholders are not allowed"):
            functools.partial(callback, _Placeholder)
        with pytest.raises(TypeError, match="Placeholder cannot be passed as a keyword argument"):
            functools.partial(callback, value=_Placeholder)

        assert _Placeholder is type(_Placeholder)()
        assert type(_Placeholder).__name__ == "_PlaceholderType"
        assert type(functools.partial(callback, 1)).__name__ == "partial"


def _cmp_to_key_case(operation: str) -> None:
    comparisons = {
        "lt": operator.lt,
        "le": operator.le,
        "eq": operator.eq,
        "ne": operator.ne,
        "gt": operator.gt,
        "ge": operator.ge,
    }

    def run(choose: Choose) -> bool:
        def compare(_left: int, _right: int) -> int:
            return cast(int, choose())

        key = functools.cmp_to_key(compare)
        return cast(bool, comparisons[operation](key(1), key(2)))

    expected = {
        "lt": (True, False),
        "le": (True, False),
        "eq": (False, True),
        "ne": (True, False),
        "gt": (False, True),
        "ge": (False, True),
    }[operation]
    values = {
        "lt": (-1, 1),
        "le": (0, 1),
        "eq": (1, 0),
        "ne": (1, 0),
        "gt": (-1, 1),
        "ge": (-1, 0),
    }[operation]
    outcomes = _resume_outcomes(run, values)
    _assert_equal(outcomes, _returns(*expected))
    assert all(tag == "return" and type(value) is bool for tag, value in outcomes), outcomes


for _name in ("lt", "le", "eq", "ne", "gt", "ge"):
    _case("normal", f"functools_cmp_to_key_{_name}")(functools.partial(_cmp_to_key_case, _name))


@_case("error", "functools_cmp_to_key_invalid_result_isolated_per_shot")
def _functools_cmp_to_key_invalid_result_isolated_per_shot() -> None:
    def run(choose: Choose) -> bool:
        def compare(_left: int, _right: int) -> Any:
            return choose()

        key = functools.cmp_to_key(compare)
        return key(1) < key(2)

    outcomes = _resume_outcomes(run, ("invalid", -1))
    _assert_equal(
        outcomes,
        [("raise", "TypeError"), ("return", True)],
    )
    assert type(outcomes[1][1]) is bool, outcomes


@_case("normal", "functools_lru_cache_wrapper_call_pending_suffix")
def _functools_lru_cache_wrapper_call_pending_suffix() -> None:
    def run(choose: Choose) -> int:
        @functools.lru_cache(maxsize=4)
        def cached(_key: int) -> int:
            return cast(int, choose()) + 100

        return cached(1) + 1000

    _assert_equal(_resume_outcomes(run), _returns(1101, 1110))


@_case("error", "functools_lru_cache_exception_isolated_per_shot")
def _functools_lru_cache_exception_isolated_per_shot() -> None:
    class ExpectedCacheError(Exception):
        pass

    def run(choose: Choose) -> int:
        @functools.lru_cache(maxsize=None, typed=True)
        def cached(*, key: int) -> int:
            value = choose()
            if value == "raise":
                raise ExpectedCacheError
            return cast(int, value)

        return cached(key=1) + 100

    _assert_equal(
        _resume_outcomes(run, ("raise", 10)),
        [("raise", "ExpectedCacheError"), ("return", 110)],
    )


@_case("corner", "functools_lru_cache_each_shot_updates_cache")
def _functools_lru_cache_each_shot_updates_cache() -> None:
    def run(choose: Choose) -> tuple[int, int]:
        @functools.lru_cache(maxsize=1)
        def cached(_key: int) -> int:
            return cast(int, choose())

        first = cached(1)
        return first, cached(1)

    outcomes = _resume_outcomes(run)
    _assert_equal(outcomes, _returns((1, 1), (10, 10)))
    assert all(
        tag == "return" and type(value) is tuple and all(type(item) is int for item in value) for tag, value in outcomes
    ), outcomes


@_case("normal", "functools_lru_cache_cache_info_tracks_continuation_hits")
def _functools_lru_cache_cache_info_tracks_continuation_hits() -> None:
    def run(choose: Choose) -> tuple[int, int, int, Any]:
        calls = 0

        @functools.lru_cache(maxsize=2)
        def cached(_key: int) -> int:
            nonlocal calls
            calls += 1
            return cast(int, choose()) + 100

        first = cached(1)
        second = cached(1)
        return first, second, calls, cached.cache_info()

    outcomes = _resume_outcomes(run)
    expected_info = functools._CacheInfo(hits=1, misses=1, maxsize=2, currsize=1)
    _assert_equal(
        outcomes,
        _returns((101, 101, 1, expected_info), (110, 110, 1, expected_info)),
    )


@_case("error", "functools_lru_cache_exception_does_not_pollute_stats")
def _functools_lru_cache_exception_does_not_pollute_stats() -> None:
    class ExpectedCacheError(Exception):
        pass

    def run(choose: Choose) -> tuple[int, int, int, Any]:
        calls = 0

        @functools.lru_cache(maxsize=2)
        def cached(_key: int) -> int:
            nonlocal calls
            calls += 1
            value = choose()
            if value == "raise":
                raise ExpectedCacheError
            return cast(int, value)

        first = cached(1)
        second = cached(1)
        return first, second, calls, cached.cache_info()

    expected_info = functools._CacheInfo(hits=1, misses=1, maxsize=2, currsize=1)
    _assert_equal(
        _resume_outcomes(run, ("raise", 7)),
        [("raise", "ExpectedCacheError"), ("return", (7, 7, 1, expected_info))],
    )


@_case("corner", "functools_lru_cache_preserves_lru_recency_on_continuation")
def _functools_lru_cache_preserves_lru_recency_on_continuation() -> None:
    def run(choose: Choose) -> tuple[tuple[int, ...], tuple[int, ...], Any]:
        calls: list[int] = []

        @functools.lru_cache(maxsize=2)
        def cached(key: int) -> int:
            calls.append(key)
            if key == 1:
                return cast(int, choose())
            return key * 10

        first = cached(1)
        first_again = cached(1)
        cached(2)
        first_after_other = cached(1)
        cached(3)
        second_after_eviction = cached(2)
        return (
            (first, first_again, first_after_other, second_after_eviction),
            tuple(calls),
            cached.cache_info(),
        )

    expected_info = functools._CacheInfo(hits=2, misses=4, maxsize=2, currsize=2)
    _assert_equal(
        _resume_outcomes(run),
        _returns(
            ((1, 1, 1, 20), (1, 2, 3, 2), expected_info),
            ((10, 10, 10, 20), (1, 2, 3, 2, 2, 3, 2), expected_info),
        ),
    )


@_case("corner", "functools_lru_cache_typed_false_separates_int_and_bool")
def _functools_lru_cache_typed_false_separates_int_and_bool() -> None:
    def run(choose: Choose) -> tuple[int, int, str, tuple[tuple[Any, type], ...], Any]:
        calls: list[tuple[Any, type]] = []

        @functools.lru_cache(maxsize=None, typed=False)
        def cached(value: int | bool) -> int | str:
            calls.append((value, type(value)))
            if type(value) is int:
                return cast(int, choose())
            return str(value)

        integer = cast(int, cached(1))
        integer_again = cast(int, cached(1))
        boolean = cast(str, cached(True))
        return integer, integer_again, boolean, tuple(calls), cached.cache_info()

    expected_info = functools._CacheInfo(hits=1, misses=2, maxsize=None, currsize=2)
    _assert_equal(
        _resume_outcomes(run),
        _returns(
            (1, 1, "True", ((1, int), (True, bool)), expected_info),
            (
                10,
                10,
                "True",
                ((1, int), (True, bool), (True, bool)),
                expected_info,
            ),
        ),
    )


@_case("normal", "functools_lru_cache_continuation_preserves_public_wrapper_attributes")
def _functools_lru_cache_continuation_preserves_public_wrapper_attributes() -> None:
    def run(choose: Choose) -> tuple[int, str, str, str, Any, Any]:
        def implementation(_key: int) -> int:
            """Original cached implementation."""
            return cast(int, choose())

        cached = functools.lru_cache(maxsize=2, typed=True)(implementation)
        result = cached(1)
        return (
            result,
            cached.__name__,
            cached.__doc__,
            cached.__wrapped__.__name__,
            cached.cache_parameters(),
            cached.cache_info(),
        )

    expected_info = functools._CacheInfo(hits=0, misses=1, maxsize=2, currsize=1)
    _assert_equal(
        _resume_outcomes(run),
        _returns(
            (
                1,
                "implementation",
                "Original cached implementation.",
                "implementation",
                {"maxsize": 2, "typed": True},
                expected_info,
            ),
            (
                10,
                "implementation",
                "Original cached implementation.",
                "implementation",
                {"maxsize": 2, "typed": True},
                expected_info,
            ),
        ),
    )


_OPERATOR_PROTOCOL_CASES: dict[str, tuple[str, Callable[..., Any], tuple[Any, ...]]] = {
    "abs": ("__abs__", operator.abs, ()),
    "add": ("__add__", operator.add, (2,)),
    "and_": ("__and__", operator.and_, (2,)),
    "call": ("__call__", operator.call, ()),
    "eq": ("__eq__", operator.eq, (2,)),
    "floordiv": ("__floordiv__", operator.floordiv, (2,)),
    "ge": ("__ge__", operator.ge, (2,)),
    "getitem": ("__getitem__", operator.getitem, ("key",)),
    "gt": ("__gt__", operator.gt, (2,)),
    "iadd": ("__iadd__", operator.iadd, (2,)),
    "iand": ("__iand__", operator.iand, (2,)),
    "ifloordiv": ("__ifloordiv__", operator.ifloordiv, (2,)),
    "ilshift": ("__ilshift__", operator.ilshift, (2,)),
    "imatmul": ("__imatmul__", operator.imatmul, (2,)),
    "imod": ("__imod__", operator.imod, (2,)),
    "imul": ("__imul__", operator.imul, (2,)),
    "index": ("__index__", operator.index, ()),
    "inv": ("__invert__", operator.inv, ()),
    "invert": ("__invert__", operator.invert, ()),
    "ior": ("__ior__", operator.ior, (2,)),
    "ipow": ("__ipow__", operator.ipow, (2,)),
    "irshift": ("__irshift__", operator.irshift, (2,)),
    "isub": ("__isub__", operator.isub, (2,)),
    "itruediv": ("__itruediv__", operator.itruediv, (2,)),
    "ixor": ("__ixor__", operator.ixor, (2,)),
    "le": ("__le__", operator.le, (2,)),
    "lshift": ("__lshift__", operator.lshift, (2,)),
    "lt": ("__lt__", operator.lt, (2,)),
    "matmul": ("__matmul__", operator.matmul, (2,)),
    "mod": ("__mod__", operator.mod, (2,)),
    "mul": ("__mul__", operator.mul, (2,)),
    "ne": ("__ne__", operator.ne, (2,)),
    "neg": ("__neg__", operator.neg, ()),
    "or_": ("__or__", operator.or_, (2,)),
    "pos": ("__pos__", operator.pos, ()),
    "pow": ("__pow__", operator.pow, (2,)),
    "rshift": ("__rshift__", operator.rshift, (2,)),
    "sub": ("__sub__", operator.sub, (2,)),
    "truediv": ("__truediv__", operator.truediv, (2,)),
    "xor": ("__xor__", operator.xor, (2,)),
}


def _operator_protocol_case(name: str) -> None:
    method_name, operation, arguments = _OPERATOR_PROTOCOL_CASES[name]

    def run(choose: Choose) -> int:
        def protocol(_self: Any, *_args: Any) -> int:
            return cast(int, choose())

        namespace: dict[str, Any] = {method_name: protocol}
        target = type("EffectfulOperatorTarget", (), namespace)()
        if len(arguments) == 0:
            result = operation(target)
        elif len(arguments) == 1:
            result = operation(target, arguments[0])
        else:
            result = operation(target, arguments[0], arguments[1])
        return cast(int, result) + 100

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("error", "operator_concat_requires_sequence")
def _operator_concat_requires_sequence() -> None:
    class Target:
        def __add__(self, _other: Any) -> int:
            return 1

        def __getitem__(self, _index: Any) -> int:
            return 0

    for operation in (operator.concat, operator.iconcat):
        with pytest.raises(TypeError, match="can't be concatenated"):
            operation(Target(), 2)


for _name in _OPERATOR_PROTOCOL_CASES:
    _case("normal", f"operator_public_{_name}")(functools.partial(_operator_protocol_case, _name))


def _operator_truth_case(name: str) -> None:
    operation = {"truth": operator.truth, "not_": operator.not_}[name]

    def run(choose: Choose) -> bool:
        class Target:
            def __bool__(self) -> bool:
                return cast(bool, choose())

        return cast(bool, operation(Target()))

    expected = (False, True) if name == "truth" else (True, False)
    outcomes = _resume_outcomes(run, (False, True))
    _assert_equal(outcomes, _returns(*expected))
    assert all(tag == "return" and type(value) is bool for tag, value in outcomes), outcomes


for _name in ("truth", "not_"):
    _case("normal", f"operator_public_{_name}")(functools.partial(_operator_truth_case, _name))


@_case("normal", "operator_public_length_hint")
def _operator_public_length_hint() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __length_hint__(self) -> int:
                return cast(int, choose())

        return operator.length_hint(Target()) + 100

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


@_case("error", "operator_index_invalid_result_isolated_per_shot")
def _operator_index_invalid_result_isolated_per_shot() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __index__(self) -> Any:
                return choose()

        return operator.index(Target()) + 100

    _assert_equal(
        _resume_outcomes(run, ("invalid", 10)),
        [("raise", "TypeError"), ("return", 110)],
    )


@_case("error", "operator_index_of_failure_isolated_per_shot")
def _operator_index_of_failure_isolated_per_shot() -> None:
    def run(choose: Choose) -> int:
        class Candidate:
            def __eq__(self, _other: Any) -> bool:
                return cast(bool, choose())

        return operator.indexOf([Candidate()], object())

    outcomes = _resume_outcomes(run, (False, True))
    _assert_equal(
        outcomes,
        [("raise", "ValueError"), ("return", 0)],
    )
    assert type(outcomes[1][1]) is int, outcomes


@_case("normal", "operator_count_of_pending_suffix")
def _operator_count_of_pending_suffix() -> None:
    def run(choose: Choose) -> int:
        class Candidate:
            def __eq__(self, _other: Any) -> bool:
                return cast(bool, choose())

        return operator.countOf([Candidate()], object()) + 100

    outcomes = _resume_outcomes(run, (False, True))
    _assert_equal(outcomes, _returns(100, 101))
    assert all(tag == "return" and type(value) is int for tag, value in outcomes), outcomes


@_case("normal", "operator_contains_normalizes_result")
def _operator_contains_normalizes_result() -> None:
    def run(choose: Choose) -> bool:
        class Container:
            def __contains__(self, _item: Any) -> bool:
                return cast(bool, choose())

        return operator.contains(Container(), object())

    outcomes = _resume_outcomes(run, (False, True))
    _assert_equal(outcomes, _returns(False, True))
    assert all(tag == "return" and type(value) is bool for tag, value in outcomes), outcomes


def _operator_mutation_case(name: str) -> None:
    operation = {"setitem": operator.setitem, "delitem": operator.delitem}[name]

    def run(choose: Choose) -> int:
        class Target:
            selected = 0

            def __setitem__(self, _key: str, _value: int) -> None:
                self.selected = cast(int, choose())

            def __delitem__(self, _key: str) -> None:
                self.selected = cast(int, choose())

        target = Target()
        if name == "setitem":
            result = operation(target, "key", 100)
        else:
            result = operation(target, "key")
        assert result is None
        return target.selected + 100

    _assert_equal(_resume_outcomes(run), _returns(101, 110))


for _name in ("setitem", "delitem"):
    _case("normal", f"operator_public_{_name}")(functools.partial(_operator_mutation_case, _name))


@_case("corner", "operator_public_function_coverage")
def _operator_public_function_coverage() -> None:
    identity_only = {"is_", "is_not", "is_none", "is_not_none"}
    tested = set(_OPERATOR_PROTOCOL_CASES) | {
        "attrgetter",
        "contains",
        "concat",
        "countOf",
        "delitem",
        "indexOf",
        "itemgetter",
        "iconcat",
        "length_hint",
        "methodcaller",
        "not_",
        "setitem",
        "truth",
    }
    expected = set(operator.__all__) - identity_only
    _assert_equal(tested, expected)


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

    @_case("corner", "version_sum_compensated_fast_paths")
    def _version_sum_compensated_fast_paths() -> None:
        _assert_equal(sum([1e16, 1.0, -1e16]), 1.0)
        _assert_equal(sum([1.0, 1e100, 1.0, -1e100]), 2.0)

        if sys.version_info >= (3, 14):
            _assert_equal(sum([1e16 + 2e16j, 1 + 2j, 1 + 1j, -1e16 - 2e16j]), 2 + 3j)

        def effectful_iterator(choose: Choose) -> float:
            return sum(map(lambda _item: choose(), (None,)), 1e16)

        _assert_equal(
            _resume_outcomes(effectful_iterator, (1.0, 10.0)),
            _returns(1e16 + 1.0, 1e16 + 10.0),
        )

        def fallback_callback(choose: Choose) -> Any:
            class Fallback:
                def __radd__(self, left: object) -> float:
                    return choose()

            return sum((1.0, Fallback()))

        _assert_equal(
            _resume_outcomes(fallback_callback),
            _returns(1, 10),
        )

        class FailingFallback:
            def __radd__(self, left: object) -> Any:
                raise ValueError("fallback failure")

        with pytest.raises(ValueError, match="fallback failure"):
            sum((1.0, FailingFallback()))

        _assert_equal(sum([sys.maxsize, 1]), sys.maxsize + 1)
        with pytest.raises(OverflowError):
            sum([1.0, 10**400])
        assert type(sum([True, False, True])) is int


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
