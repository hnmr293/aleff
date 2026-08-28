"""Strict multi-shot tests for CPython's core object protocols.

The protocol calls in this module deliberately run in a fresh interpreter.
That keeps a bad continuation boundary diagnosable: CPython can abort while a
slot callback is returning, and ``faulthandler`` output is retained by the
parent assertion.
"""

from __future__ import annotations

import operator
from pathlib import Path
import subprocess
import sys
from typing import Any, Callable, Literal, cast

import pytest

from aleff import create_handler, effect


Choose = Callable[[], Any]
Case = Callable[[], None]
CaseKind = Literal["normal", "error", "corner"]
_CASES: dict[str, tuple[CaseKind, Case]] = {}


def _case(kind: CaseKind, name: str) -> Callable[[Case], Case]:
    def register(case: Case) -> Case:
        _CASES[name] = (kind, case)
        return case

    return register


def _outcomes(run: Callable[[Choose], Any], values: tuple[Any, ...] = (1, 10)) -> list[tuple[str, Any]]:
    choose = effect("protocol-choice")
    handler = create_handler(choose)

    @handler.on(choose)
    def resume(k: Any) -> list[tuple[str, Any]]:
        result: list[tuple[str, Any]] = []
        for value in values:
            try:
                result.append(("return", k(value)))
            except Exception as exc:
                result.append(("raise", type(exc).__name__))
        return result

    return cast(list[tuple[str, Any]], handler(lambda: run(choose)))


def _returns(*values: Any) -> list[tuple[str, Any]]:
    return [("return", value) for value in values]


def _assert_outcomes(actual: Any, *expected: Any) -> None:
    assert actual == _returns(*expected)


class _ProtocolFailure(Exception):
    pass


def _raise_or_value(choose: Choose) -> Any:
    value = choose()
    if value == "raise":
        raise _ProtocolFailure("callback failure")
    return value


# Truth and length ---------------------------------------------------------


@_case("normal", "truth_and_len_normalize_and_preserve_exact_types")
def _truth_and_len_normalize_and_preserve_exact_types() -> None:
    def run(choose: Choose) -> tuple[bool, int]:
        class Target:
            def __bool__(self) -> bool:
                return bool(choose())

            def __len__(self) -> int:
                return 99

        target = Target()
        result = (bool(target), len(target))
        assert type(result[0]) is bool
        assert type(result[1]) is int
        return result

    assert _outcomes(run, (True, False)) == [
        ("return", (True, 99)),
        ("return", (False, 99)),
    ]


@_case("error", "truth_and_len_invalid_callback_results_are_shot_isolated")
def _truth_and_len_invalid_callback_results_are_shot_isolated() -> None:
    invalid = object()

    def bool_run(choose: Choose) -> bool:
        class Target:
            def __bool__(self) -> Any:
                return choose()

        return bool(Target())

    def len_run(choose: Choose) -> int:
        class Target:
            def __len__(self) -> Any:
                return choose()

        return len(Target())

    assert _outcomes(bool_run, (True, invalid, False)) == [
        ("return", True),
        ("raise", "TypeError"),
        ("return", False),
    ]
    assert _outcomes(len_run, (1, invalid, 3)) == [
        ("return", 1),
        ("raise", "TypeError"),
        ("return", 3),
    ]


@_case("corner", "truth_len_missing_fallback_and_three_shot_nested")
def _truth_len_missing_fallback_and_three_shot_nested() -> None:
    class Truthy:
        pass

    class Empty:
        def __len__(self) -> int:
            return 0

    assert bool(Truthy()) is True
    assert bool(Empty()) is False

    def run(choose: Choose) -> int:
        class Target:
            def __len__(self) -> int:
                return cast(int, choose())

        def nested() -> int:
            return len(Target())

        value = nested()
        assert type(value) is int
        return value

    assert _outcomes(run, (1, 2, 3)) == _returns(1, 2, 3)


# Unary and conversion slots ------------------------------------------------


def _unary_apply(name: str, target: Any) -> Any:
    operations: dict[str, Callable[[Any], Any]] = {
        "__abs__": abs,
        "__neg__": lambda value: -value,
        "__pos__": lambda value: +value,
        "__invert__": lambda value: ~value,
        "__index__": operator.index,
        "__int__": int,
        "__float__": float,
        "__complex__": complex,
        "__round__": lambda value: round(value, 2),
    }
    return operations[name](target)


def _unary_case(name: str) -> None:
    def run(choose: Choose) -> Any:
        def method(_self: Any, *args: Any) -> Any:
            value = choose()
            if name == "__bool__":
                return bool(value)
            if name == "__len__":
                return int(value)
            if name == "__index__":
                return int(value)
            if name == "__int__":
                return int(value) + 100
            if name == "__float__":
                return float(value) + 0.5
            if name == "__complex__":
                return complex(value, 0.5)
            if name == "__round__":
                return int(value) + 100
            return int(value) + 100

        target_type = type("UnaryTarget", (), {name: method})
        return bool(target_type()) if name == "__bool__" else _unary_apply(name, target_type())

    expected: dict[str, tuple[Any, ...]] = {
        "__abs__": (101, 110),
        "__neg__": (101, 110),
        "__pos__": (101, 110),
        "__invert__": (101, 110),
        "__index__": (1, 10),
        "__int__": (101, 110),
        "__float__": (1.5, 10.5),
        "__complex__": (1 + 0.5j, 10 + 0.5j),
        "__round__": (101, 110),
    }
    actual = _outcomes(run)
    assert actual == _returns(*expected[name])
    if name == "__index__":
        assert all(type(value) is int for _, value in actual)
    elif name == "__int__":
        assert all(type(value) is int for _, value in actual)
    elif name == "__float__":
        assert all(type(value) is float for _, value in actual)
    elif name == "__complex__":
        assert all(type(value) is complex for _, value in actual)


for _unary_name in (
    "__abs__",
    "__neg__",
    "__pos__",
    "__invert__",
    "__index__",
    "__int__",
    "__float__",
    "__complex__",
    "__round__",
):
    _case("normal", f"unary_protocol_{_unary_name}")(cast(Any, lambda name=_unary_name: _unary_case(name)))


@_case("error", "unary_callback_exception_and_invalid_conversion_are_isolated")
def _unary_callback_exception_and_invalid_conversion_are_isolated() -> None:
    invalid = object()

    def run(choose: Choose) -> int:
        class Target:
            def __int__(self) -> Any:
                return _raise_or_value(choose)

        return int(Target())

    assert _outcomes(run, (1, "raise", invalid, 4)) == [
        ("return", 1),
        ("raise", "_ProtocolFailure"),
        ("raise", "TypeError"),
        ("return", 4),
    ]


def _invalid_conversion_case(name: str) -> None:
    invalid = object()

    def run(choose: Choose) -> Any:
        def method(_self: Any) -> Any:
            return choose()

        target = type("InvalidConversionTarget", (), {name: method})()
        if name == "__index__":
            return operator.index(target)  # pyright: ignore[reportArgumentType]
        if name == "__float__":
            return float(target)  # pyright: ignore[reportArgumentType]
        return complex(target)  # pyright: ignore[reportArgumentType, reportCallIssue]

    expected = {"__index__": 7, "__float__": 7.5, "__complex__": 7 + 0.5j}[name]
    valid = expected
    actual = _outcomes(run, (invalid, valid, invalid))
    assert actual == [
        ("raise", "TypeError"),
        ("return", expected),
        ("raise", "TypeError"),
    ]


for _conversion_name in ("__index__", "__float__", "__complex__"):
    _case("error", f"invalid_conversion_{_conversion_name}")(
        cast(Any, lambda name=_conversion_name: _invalid_conversion_case(name))
    )


@_case("corner", "unary_missing_fallback_and_three_shot_nested")
def _unary_missing_fallback_and_three_shot_nested() -> None:
    class Target:
        def __index__(self) -> int:
            return 7

    assert operator.index(Target()) == 7
    with pytest.raises(TypeError):
        abs(Target())  # pyright: ignore[reportArgumentType]

    def run(choose: Choose) -> int:
        class Number:
            def __neg__(self) -> int:
                return -cast(int, choose())

        def outer() -> int:
            return -Number()

        return outer()

    assert _outcomes(run, (1, 2, 3)) == _returns(-1, -2, -3)


if sys.version_info < (3, 14):

    @_case("normal", "int_trunc_only_fallback_before_314")
    def _int_trunc_only_fallback_before_314() -> None:
        def run(choose: Choose) -> int:
            class TruncOnly:
                def __trunc__(self) -> int:
                    return cast(int, choose())

            return int(TruncOnly())

        assert _outcomes(run, (1, 10)) == _returns(1, 10)

else:

    @_case("corner", "int_trunc_only_is_rejected_on_314_and_later")
    def _int_trunc_only_is_rejected_on_314_and_later() -> None:
        class TruncOnly:
            def __trunc__(self) -> int:
                return 1

        with pytest.raises(TypeError):
            int(TruncOnly())


# Binary, reflected, and in-place slots -------------------------------------


_BINARY: tuple[tuple[str, str], ...] = (
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


def _binary_case(name: str, symbol: str, reflected: bool, inplace: bool) -> None:
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

        target = type("BinaryTarget", (), {name: method})()
        result = apply(target)
        assert type(result) is int
        return result

    assert _outcomes(run) == _returns(101, 110)


for _binary_name, _binary_symbol in _BINARY:
    _case("normal", f"binary_{_binary_name}")(
        lambda n=_binary_name, s=_binary_symbol: _binary_case(f"__{n}__", s, False, False)
    )
    _case("normal", f"reflected_{_binary_name}")(
        lambda n=_binary_name, s=_binary_symbol: _binary_case(f"__r{n}__", s, True, False)
    )
    _case("normal", f"inplace_{_binary_name}")(
        lambda n=_binary_name, s=_binary_symbol: _binary_case(f"__i{n}__", s, False, True)
    )


@_case("error", "binary_invalid_result_and_callback_exception_are_isolated")
def _binary_invalid_result_and_callback_exception_are_isolated() -> None:
    invalid = object()

    def run(choose: Choose) -> Any:
        class Target:
            def __add__(self, _other: Any) -> Any:
                return _raise_or_value(choose)

        return Target() + 7

    assert _outcomes(run, (1, "raise", invalid, 10)) == [
        ("return", 1),
        ("raise", "_ProtocolFailure"),
        ("return", invalid),
        ("return", 10),
    ]


@_case("corner", "binary_not_implemented_reflected_fallback_and_missing_inplace")
def _binary_not_implemented_reflected_fallback_and_missing_inplace() -> None:
    def run(choose: Choose) -> int:
        class Left:
            def __add__(self, _other: Any) -> Any:
                return NotImplemented

        class Right:
            def __radd__(self, _other: Any) -> int:
                return cast(int, choose())

        return Left() + Right()

    assert _outcomes(run, (1, 2, 3)) == _returns(1, 2, 3)

    def inplace_run(choose: Choose) -> int:
        class Target:
            def __add__(self, _other: Any) -> int:
                return cast(int, choose())

        target = Target()
        target += 7
        return target

    assert _outcomes(inplace_run) == _returns(1, 10)


# Rich comparison -----------------------------------------------------------


def _comparison_case(name: str, symbol: str) -> None:
    namespace: dict[str, Any] = {}
    exec(f"def compare(target, other):\n    return target {symbol} other", namespace)
    compare = cast(Callable[[Any, Any], Any], namespace["compare"])

    def run(choose: Choose) -> Any:
        def method(_self: Any, _other: Any) -> bool:
            return bool(choose())

        return compare(type("Comparable", (), {name: method})(), object())

    assert _outcomes(run, (False, True)) == _returns(False, True)


for _comparison_name, _comparison_symbol in (
    ("eq", "=="),
    ("ne", "!="),
    ("lt", "<"),
    ("le", "<="),
    ("gt", ">"),
    ("ge", ">="),
):
    _case("normal", f"comparison_{_comparison_name}")(
        lambda n=_comparison_name, s=_comparison_symbol: _comparison_case(f"__{n}__", s)
    )


@_case("error", "comparison_exception_then_success_isolated")
def _comparison_exception_then_success_isolated() -> None:
    def run(choose: Choose) -> bool:
        class Target:
            def __eq__(self, _other: Any) -> bool:
                return cast(bool, _raise_or_value(choose))

        return Target() == object()

    assert _outcomes(run, ("raise", True)) == [("raise", "_ProtocolFailure"), ("return", True)]


@_case("corner", "comparison_missing_fallback_and_non_bool_result_preserved")
def _comparison_missing_fallback_and_non_bool_result_preserved() -> None:
    class Target:
        pass

    assert (Target() == Target()) is False
    assert (Target() != Target()) is True
    with pytest.raises(TypeError):
        Target() < Target()  # pyright: ignore[reportOperatorIssue, reportUnusedExpression]

    marker = object()

    def run(choose: Choose) -> Any:
        class Comparable:
            def __eq__(self, _other: Any) -> Any:
                return choose()

        return Comparable() == object()

    assert _outcomes(run, (marker, marker)) == _returns(marker, marker)


# Attribute and descriptor slots -------------------------------------------


@_case("normal", "attribute_get_set_delete_and_descriptor_callbacks")
def _attribute_get_set_delete_and_descriptor_callbacks() -> None:
    def run(choose: Choose) -> tuple[int, int, int]:
        class Descriptor:
            def __get__(self, _instance: Any, _owner: Any) -> int:
                return cast(int, choose())

            def __set__(self, instance: Any, value: int) -> None:
                instance.saved = value + 2

            def __delete__(self, instance: Any) -> None:
                instance.deleted = 5

        class Target:
            value = Descriptor()

        target = Target()
        got = target.value
        target.value = 100
        del target.value
        result = (got, target.saved, target.deleted)  # pyright: ignore[reportAttributeAccessIssue, reportUnknownMemberType, reportUnknownVariableType]
        assert all(type(value) is int for value in result)  # pyright: ignore[reportUnknownArgumentType, reportUnknownVariableType]
        return result  # pyright: ignore[reportUnknownVariableType]

    assert _outcomes(run) == _returns((1, 102, 5), (10, 102, 5))


@_case("error", "attribute_and_descriptor_exception_shot_isolation")
def _attribute_and_descriptor_exception_shot_isolation() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __getattribute__(self, name: str) -> Any:
                if name == "value":
                    return _raise_or_value(choose)
                return object.__getattribute__(self, name)

        return cast(int, getattr(Target(), "value"))

    assert _outcomes(run, ("raise", 7, "raise", 9)) == [
        ("raise", "_ProtocolFailure"),
        ("return", 7),
        ("raise", "_ProtocolFailure"),
        ("return", 9),
    ]


@_case("corner", "attribute_missing_getattr_and_set_name_three_shot")
def _attribute_missing_getattr_and_set_name_three_shot() -> None:
    def run(choose: Choose) -> str:
        class Descriptor:
            def __set_name__(self, _owner: Any, name: str) -> None:
                assert name == "value"
                self.tag = str(choose())

            def __get__(self, _instance: Any, _owner: Any) -> str:
                return self.tag

        class Target:
            value = Descriptor()

        target = Target()
        return target.value

    assert _outcomes(run, (1, 2, 3)) == _returns("1", "2", "3")
    assert getattr(type("NoFallback", (), {})(), "missing", "fallback") == "fallback"


# Item and iterator slots ---------------------------------------------------


@_case("normal", "item_get_set_delete_callbacks")
def _item_get_set_delete_callbacks() -> None:
    def run(choose: Choose) -> tuple[int, int, int]:
        class Target:
            def __getitem__(self, key: str) -> int:
                assert key == "key"
                return cast(int, choose())

            def __setitem__(self, key: str, value: int) -> None:
                assert key == "key"
                self.saved = value + 2

            def __delitem__(self, key: str) -> None:
                assert key == "key"
                self.deleted = 5

        target = Target()
        got = target["key"]
        target["key"] = 100
        del target["key"]
        return got, target.saved, target.deleted

    assert _outcomes(run) == _returns((1, 102, 5), (10, 102, 5))


@_case("error", "item_callback_exception_then_success_and_invalid_key_result")
def _item_callback_exception_then_success_and_invalid_key_result() -> None:
    invalid = object()

    def run(choose: Choose) -> Any:
        class Target:
            def __getitem__(self, _key: str) -> Any:
                return _raise_or_value(choose)

        return Target()["key"]

    assert _outcomes(run, ("raise", 2, invalid, 4)) == [
        ("raise", "_ProtocolFailure"),
        ("return", 2),
        ("return", invalid),
        ("return", 4),
    ]


@_case("corner", "item_missing_fallback_and_nested_three_shot")
def _item_missing_fallback_and_nested_three_shot() -> None:
    class Target:
        pass

    with pytest.raises(TypeError):
        Target()["key"]  # type: ignore[index]

    def run(choose: Choose) -> int:
        class Item:
            def __getitem__(self, key: int) -> int:
                assert key == 0
                return cast(int, choose())

        def nested() -> int:
            return Item()[0]

        return nested()

    assert _outcomes(run, (1, 2, 3)) == _returns(1, 2, 3)


@_case("normal", "iter_next_reversed_and_async_iter_callbacks")
def _iter_next_reversed_and_async_iter_callbacks() -> None:
    def drive(coroutine: Any) -> Any:
        try:
            coroutine.send(None)
        except StopIteration as completed:
            return completed.value
        raise AssertionError("coroutine unexpectedly suspended")

    def run(choose: Choose) -> tuple[list[int], int, list[int], int]:
        class Iterator:
            def __iter__(self) -> Any:
                return iter((cast(int, choose()),))

            def __next__(self) -> int:
                return 2

        class Reversible:
            def __reversed__(self) -> Any:
                return iter((3,))

        class AsyncIterator:
            def __aiter__(self) -> Any:
                return self

            async def __anext__(self) -> int:
                return 5

        iter_result = list(Iterator())
        next_result = next(Iterator())
        reversed_result = list(reversed(Reversible()))  # pyright: ignore[reportArgumentType, reportCallIssue]

        async def consume() -> int:
            return await anext(AsyncIterator())

        async_result = drive(consume())
        return iter_result, next_result, reversed_result, async_result  # pyright: ignore[reportReturnType]

    assert _outcomes(run) == _returns(([1], 2, [3], 5), ([10], 2, [3], 5))


@_case("error", "iter_next_and_async_callback_exceptions_are_isolated")
def _iter_next_and_async_callback_exceptions_are_isolated() -> None:
    def run(choose: Choose) -> Any:
        class Iterator:
            def __iter__(self) -> Any:
                value = choose()
                if value == "raise":
                    raise _ProtocolFailure
                return iter((value,))

        return list(Iterator())

    assert _outcomes(run, ("raise", 2, "raise", 4)) == [
        ("raise", "_ProtocolFailure"),
        ("return", [2]),
        ("raise", "_ProtocolFailure"),
        ("return", [4]),
    ]

    def invalid_iter(choose: Choose) -> Any:
        class Iterator:
            def __iter__(self) -> Any:
                return iter(choose())

        return list(iter(Iterator()))

    assert _outcomes(invalid_iter, (object(), (), object())) == [
        ("raise", "TypeError"),
        ("return", []),
        ("raise", "TypeError"),
    ]

    def next_run(choose: Choose) -> Any:
        class Iterator:
            def __iter__(self) -> Any:
                return self

            def __next__(self) -> Any:
                return _raise_or_value(choose)

        return next(Iterator())

    assert _outcomes(next_run, ("raise", 2, "raise", 4)) == [
        ("raise", "_ProtocolFailure"),
        ("return", 2),
        ("raise", "_ProtocolFailure"),
        ("return", 4),
    ]

    def async_run(choose: Choose) -> Any:
        class AsyncIterator:
            def __aiter__(self) -> Any:
                return self

            async def __anext__(self) -> Any:
                return _raise_or_value(choose)

        async def consume() -> Any:
            return await anext(AsyncIterator())

        try:
            consume().send(None)
        except StopIteration as completed:
            return completed.value
        raise AssertionError("coroutine unexpectedly suspended")

    assert _outcomes(async_run, ("raise", 2, "raise", 4)) == [
        ("raise", "_ProtocolFailure"),
        ("return", 2),
        ("raise", "_ProtocolFailure"),
        ("return", 4),
    ]


@_case("corner", "reversed_fallback_and_async_iter_invalid_return")
def _reversed_fallback_and_async_iter_invalid_return() -> None:
    class Sequence:
        def __len__(self) -> int:
            return 2

        def __getitem__(self, index: int) -> int:
            if index < 0:
                raise IndexError
            return index + 10

    assert list(reversed(Sequence())) == [11, 10]

    class BadAsync:
        def __aiter__(self) -> Any:
            return object()

    with pytest.raises(TypeError):
        aiter(BadAsync())


# Representation and hash slots --------------------------------------------


def _representation_case(name: str) -> None:
    def run(choose: Choose) -> Any:
        def method(_self: Any, *args: Any) -> Any:
            value = choose()
            if name == "__hash__":
                return value
            if name == "__format__":
                return f"value={value}:{args[0]}"
            return f"value={value}"

        target = type("RepresentationTarget", (), {name: method})()
        if name == "__format__":
            return format(target, "spec")
        return {"__str__": str, "__repr__": repr, "__hash__": hash}[name](target)

    expected = {
        "__str__": ("value=1", "value=10"),
        "__repr__": ("value=1", "value=10"),
        "__format__": ("value=1:spec", "value=10:spec"),
        "__hash__": (1, 10),
    }
    actual = _outcomes(run)
    assert actual == _returns(*expected[name])
    if name == "__hash__":
        assert all(type(value) is int for _, value in actual)


for _representation_name in ("__str__", "__repr__", "__format__", "__hash__"):
    _case("normal", f"representation_{_representation_name}")(lambda n=_representation_name: _representation_case(n))


@_case("error", "representation_invalid_return_and_exception_are_isolated")
def _representation_invalid_return_and_exception_are_isolated() -> None:
    invalid = object()

    def run(choose: Choose) -> str:
        class Target:
            def __str__(self) -> Any:
                return _raise_or_value(choose)

        return str(Target())

    assert _outcomes(run, ("ok", "raise", invalid, "last")) == [
        ("return", "ok"),
        ("raise", "_ProtocolFailure"),
        ("raise", "TypeError"),
        ("return", "last"),
    ]


def _representation_invalid_case(name: str) -> None:
    invalid = object()

    def run(choose: Choose) -> Any:
        def method(_self: Any, *_args: Any) -> Any:
            return choose()

        target = type("InvalidRepresentationTarget", (), {name: method})()
        if name == "__str__":
            return str(target)
        if name == "__repr__":
            return repr(target)
        if name == "__format__":
            return format(target, "spec")
        return hash(target)

    values = (invalid, 1, invalid) if name == "__hash__" else (invalid, "ok", invalid)
    expected = 1 if name == "__hash__" else "ok"
    actual = _outcomes(run, values)
    assert actual == [
        ("raise", "TypeError"),
        ("return", expected),
        ("raise", "TypeError"),
    ]


for _representation_name in ("__repr__", "__format__", "__hash__"):
    _case("error", f"representation_invalid_{_representation_name}")(
        cast(Any, lambda name=_representation_name: _representation_invalid_case(name))
    )


@_case("corner", "representation_missing_fallback_and_hash_minus_one_normalization")
def _representation_missing_fallback_and_hash_minus_one_normalization() -> None:
    # Core type vectorcall interception must preserve ordinary constructor
    # signatures and the type objects exposed by builtins.
    assert int.from_bytes(b"\x01\x02", "big") == 258
    assert int("12", 10) == 12
    assert isinstance("text", str)
    assert str(b"text", "ascii") == "text"
    assert float("1.5") == 1.5
    assert complex("1+2j") == 1 + 2j
    assert bool(0) is False

    class Default:
        pass

    assert type(str(Default())) is str
    assert type(repr(Default())) is str
    assert type(format(Default())) is str

    class Unhashable:
        __hash__ = None  # pyright: ignore[reportAssignmentType]

    with pytest.raises(TypeError):
        hash(Unhashable())

    def run(choose: Choose) -> int:
        class Target:
            def __hash__(self) -> int:
                return cast(int, choose())

        return hash(Target())

    assert _outcomes(run, (-1, 1, 10)) == _returns(-2, 1, 10)


def _run_case(case_name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", case_name],
        text=True,
        capture_output=True,
        timeout=15,
        check=False,
    )


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "normal"])
def test_core_protocol_normal(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "error"])
def test_core_protocol_error_isolated_per_shot(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "corner"])
def test_core_protocol_corner(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_protocols.py --case CASE_NAME")
    _CASES[sys.argv[2]][1]()
