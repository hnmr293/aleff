"""Acceptance tests for continuation-safe built-in container protocols."""

from __future__ import annotations

from collections.abc import Callable, Iterator, Mapping
import subprocess
import sys
from typing import Any, cast

import pytest

from aleff import create_handler, effect


Choose = Callable[[], Any]
Case = Callable[[], None]
_CASES: dict[str, Case] = {}


def _case(name: str) -> Callable[[Case], Case]:
    def register(case: Case) -> Case:
        _CASES[name] = case
        return case

    return register


def _outcomes(run: Callable[[Choose], Any], values: tuple[Any, ...] = (1, 10)) -> list[Any]:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle(k: Any) -> list[Any]:
        result: list[Any] = []
        for value in values:
            try:
                result.append(k(value))
            except Exception as exc:  # The exception is part of an outcome.
                result.append(type(exc).__name__)
        return result

    return cast(list[Any], handler(lambda: run(choose)))


class _EffectfulIterable:
    def __init__(self, choose: Choose, values: tuple[Any, ...]) -> None:
        self.choose = choose
        self.values = values

    def __iter__(self) -> Iterator[Any]:
        value = self.choose()

        def replace(item: Any) -> Any:
            if item is _CHOSEN:
                return value
            if isinstance(item, tuple):
                return tuple(replace(part) for part in item)  # pyright: ignore[reportUnknownVariableType]
            return item

        return iter(tuple(replace(item) for item in self.values))


_CHOSEN = object()


class _EffectfulMapping(Mapping[str, Any]):
    """A Mapping that is also sequence-shaped and has a misleading iterator."""

    def __init__(self, choose: Choose, value: Any = _CHOSEN) -> None:
        self.choose = choose
        self.value = value

    def __getitem__(self, key: str) -> Any:
        if key != "chosen":
            raise KeyError(key)
        value = self.choose()
        if isinstance(value, BaseException):
            raise value
        return value if self.value is _CHOSEN else self.value

    def __iter__(self) -> Iterator[Any]:
        # A sequence interpretation would consume this as a pair and be wrong.
        return iter((("wrong", 99),))

    def __len__(self) -> int:
        return 1

    def keys(self) -> tuple[str, ...]:  # pyright: ignore[reportIncompatibleMethodOverride]
        return ("chosen",)


class _EffectfulKeysMapping(Mapping[str, int]):
    def __init__(self, choose: Choose) -> None:
        self.choose = choose

    def __getitem__(self, key: str) -> int:
        if key not in {"chosen", "other"}:
            raise KeyError(key)
        return 42

    def __iter__(self) -> Iterator[str]:
        return iter((("wrong", 99),))  # pyright: ignore[reportReturnType]

    def __len__(self) -> int:
        return 1

    def keys(self) -> tuple[str, ...]:  # pyright: ignore[reportIncompatibleMethodOverride]
        return cast(tuple[str, ...], self.choose())


class _EffectfulIndex:
    def __init__(self, choose: Choose) -> None:
        self.choose = choose

    def __index__(self) -> int:
        return cast(int, self.choose())


class _EffectfulHash:
    def __init__(self, choose: Choose) -> None:
        self.choose = choose

    def __hash__(self) -> int:
        return cast(int, self.choose())

    def __eq__(self, other: Any) -> bool:
        return other in {1, 10}


class _EffectfulEquality:
    def __init__(self, choose: Choose) -> None:
        self.choose = choose

    def __eq__(self, _other: Any) -> bool:
        return bool(self.choose())

    def __ne__(self, _other: Any) -> bool:
        return bool(self.choose())


@_case("dict_item_protocols")
def _dict_item_protocols() -> None:
    def get_run(choose: Choose) -> str:
        return dict.__getitem__({1: "one", 10: "ten"}, _EffectfulHash(choose))  # pyright: ignore[reportUnknownMemberType, reportUnknownVariableType]

    def contains_run(choose: Choose) -> bool:
        return dict.__contains__({1: "one", 10: "ten"}, _EffectfulHash(choose))  # pyright: ignore[reportUnknownMemberType]

    def set_run(choose: Choose) -> dict[int, str]:
        result = {1: "old", 10: "old"}
        dict.__setitem__(result, _EffectfulHash(choose), "new")  # pyright: ignore[reportUnknownMemberType]
        return dict(result)

    def del_run(choose: Choose) -> dict[int, str]:
        result = {1: "one", 10: "ten"}
        dict.__delitem__(result, _EffectfulHash(choose))  # pyright: ignore[reportUnknownMemberType]
        return dict(result)

    assert _outcomes(get_run) == ["one", "ten"]
    assert _outcomes(contains_run) == [True, True]
    assert _outcomes(set_run) == [
        {1: "new", 10: "old"},
        {1: "new", 10: "new"},
    ]
    assert _outcomes(del_run) == [{10: "ten"}, {}]
    with pytest.raises(KeyError):
        dict.__getitem__({}, "missing")  # pyright: ignore[reportUnknownMemberType]
    with pytest.raises(KeyError):
        dict.__delitem__({}, "missing")  # pyright: ignore[reportUnknownMemberType]
    with pytest.raises(TypeError):
        dict.__setitem__({}, "key")  # pyright: ignore[reportCallIssue, reportUnknownMemberType]


@_case("dict_equality_protocols")
def _dict_equality_protocols() -> None:
    def equal_run(choose: Choose) -> bool:
        value = _EffectfulEquality(choose)
        return {"key": value} == {"key": object()}

    def not_equal_run(choose: Choose) -> bool:
        value = _EffectfulEquality(choose)
        return {"key": value} != {"key": object()}

    assert _outcomes(equal_run, (True, False)) == [True, False]
    assert _outcomes(not_equal_run, (True, False)) == [False, True]
    with pytest.raises(TypeError):
        dict.__eq__({})  # pyright: ignore[reportCallIssue, reportUnknownMemberType]
    with pytest.raises(TypeError):
        dict.__ne__({})  # pyright: ignore[reportCallIssue, reportUnknownMemberType]


@_case("dict_pop")
def _dict_pop() -> None:
    def pop_hash_run(choose: Choose) -> int:
        result = {1: 100, 10: 200}
        return 1000 + result.pop(_EffectfulHash(choose))  # pyright: ignore[reportArgumentType]

    class EqualityKey:
        def __init__(self, choose: Choose) -> None:
            self.choose = choose

        def __hash__(self) -> int:
            return 1

        def __eq__(self, _other: Any) -> bool:
            return cast(bool, self.choose())

    def pop_equality_run(choose: Choose) -> int:
        result = {1: 100}
        return result.pop(EqualityKey(choose), 300)  # pyright: ignore[reportArgumentType, reportCallIssue, reportUnknownVariableType]

    assert _outcomes(pop_hash_run) == [1100, 1200]
    assert _outcomes(pop_equality_run, (False, True)) == [300, 100]

    with pytest.raises(TypeError):
        {}.pop()  # pyright: ignore[reportCallIssue, reportUnknownMemberType]
    with pytest.raises(TypeError):
        {}.pop("key", 1, 2)  # pyright: ignore[reportCallIssue, reportUnknownMemberType]


@_case("codec_methods")
def _codec_methods() -> None:
    import codecs

    def encode_run(choose: Choose) -> bytes:
        codecs.register_error(
            "aleff_test_encode",
            lambda exc: (str(choose()), exc.end),  # pyright: ignore[reportAttributeAccessIssue, reportUnknownLambdaType, reportUnknownMemberType]
        )
        return "\N{LATIN SMALL LETTER E WITH ACUTE}".encode("ascii", "aleff_test_encode")

    def bytes_decode_run(choose: Choose) -> str:
        codecs.register_error(
            "aleff_test_bytes_decode",
            lambda exc: (str(choose()), exc.end),  # pyright: ignore[reportAttributeAccessIssue, reportUnknownLambdaType, reportUnknownMemberType]
        )
        return b"\xff".decode("ascii", "aleff_test_bytes_decode")

    def bytearray_decode_run(choose: Choose) -> str:
        codecs.register_error(
            "aleff_test_bytearray_decode",
            lambda exc: (str(choose()), exc.end),  # pyright: ignore[reportAttributeAccessIssue, reportUnknownLambdaType, reportUnknownMemberType]
        )
        return bytearray(b"\xff").decode("ascii", "aleff_test_bytearray_decode")

    assert _outcomes(encode_run) == [b"1", b"10"]
    assert _outcomes(bytes_decode_run) == ["1", "10"]
    assert _outcomes(bytearray_decode_run) == ["1", "10"]
    with pytest.raises(UnicodeEncodeError):
        "\N{LATIN SMALL LETTER E WITH ACUTE}".encode("ascii", "strict")
    with pytest.raises(UnicodeDecodeError):
        b"\xff".decode("ascii", "strict")
    with pytest.raises(UnicodeDecodeError):
        bytearray(b"\xff").decode("ascii", "strict")


@_case("memoryview_count_index")
def _memoryview_count_index() -> None:
    view = memoryview(b"ababa")
    if not hasattr(view, "count") or not hasattr(view, "index"):
        return
    assert type(view.count(97)) is int  # pyright: ignore[reportOptionalCall, reportUnknownArgumentType]
    assert view.count(97) == 3  # pyright: ignore[reportOptionalCall]
    assert view.index(98) == 1  # pyright: ignore[reportOptionalCall]
    assert view.count(120) == 0  # pyright: ignore[reportOptionalCall]
    with pytest.raises(ValueError):
        view.index(120)  # pyright: ignore[reportOptionalCall]
    assert view.count("a") == 0  # type: ignore[arg-type]
    with pytest.raises(ValueError):
        view.index("a")  # type: ignore[arg-type]


@_case("dict_fromkeys")
def _dict_fromkeys() -> None:
    def run(choose: Choose) -> dict[int, int]:
        return dict.fromkeys(_EffectfulIterable(choose, (_CHOSEN,)), 7)

    assert _outcomes(run) == [{1: 7}, {10: 7}]


@_case("dict_update")
def _dict_update() -> None:
    def run(choose: Choose) -> dict[str, int]:
        result: dict[str, int] = {}
        result.update(_EffectfulIterable(choose, (("key", _CHOSEN),)))
        return dict(result)

    assert _outcomes(run) == [{"key": 1}, {"key": 10}]


@_case("dict_update_invalid_item_isolated")
def _dict_update_invalid_item_isolated() -> None:
    def run(choose: Choose) -> dict[str, int]:
        result: dict[str, int] = {}
        result.update(_EffectfulIterable(choose, (_CHOSEN,)))
        return dict(result)

    assert _outcomes(run, (("key", 1), 10)) == [{"key": 1}, "TypeError"]


@_case("dict_fromkeys_default_value")
def _dict_fromkeys_default_value() -> None:
    def run(choose: Choose) -> dict[int, None]:
        return dict.fromkeys(_EffectfulIterable(choose, (_CHOSEN,)))

    assert _outcomes(run) == [{1: None}, {10: None}]


@_case("dict_update_keywords_and_signature")
def _dict_update_keywords_and_signature() -> None:
    def run(choose: Choose) -> dict[str, int]:
        result: dict[str, int] = {}
        result.update(_EffectfulIterable(choose, (("key", _CHOSEN),)), extra=3)
        return dict(result)

    assert _outcomes(run) == [{"key": 1, "extra": 3}, {"key": 10, "extra": 3}]
    with pytest.raises(TypeError):
        {}.update(("key", 1), ("other", 2))  # pyright: ignore[reportCallIssue, reportUnknownMemberType]


@_case("dict_update_mapping_sequence_precedence")
def _dict_update_mapping_sequence_precedence() -> None:
    def run(choose: Choose) -> dict[str, Any]:
        result: dict[str, Any] = {}
        result.update(_EffectfulMapping(choose), extra=3)
        return dict(result)

    assert _outcomes(run) == [
        {"chosen": 1, "extra": 3},
        {"chosen": 10, "extra": 3},
    ]


@_case("dict_update_mapping_getitem_error_isolated")
def _dict_update_mapping_getitem_error_isolated() -> None:
    def run(choose: Choose) -> dict[str, Any]:
        result: dict[str, Any] = {}
        result.update(_EffectfulMapping(choose), extra=3)
        return result

    assert _outcomes(run, (RuntimeError("getitem"), 10)) == [
        "RuntimeError",
        {"chosen": 10, "extra": 3},
    ]


@_case("dict_update_mapping_keys_effect_isolated")
def _dict_update_mapping_keys_effect_isolated() -> None:
    def run(choose: Choose) -> dict[str, int]:
        result: dict[str, int] = {}
        result.update(_EffectfulKeysMapping(choose))
        return dict(result)

    assert _outcomes(run, (("chosen",), ("other",))) == [
        {"chosen": 42},
        {"other": 42},
    ]


@_case("dict_update_input_shapes")
def _dict_update_input_shapes() -> None:
    def run(choose: Choose) -> dict[str, int]:
        result: dict[str, int] = {}
        result.update({"mapping": 1})
        result.update([("list", 2)])
        result.update((("tuple", 3),))
        result.update((pair for pair in (("generator", 4),)))
        result.update(_EffectfulIterable(choose, (("chosen", _CHOSEN),)))
        result.update(keyword=5)
        return dict(result)

    assert _outcomes(run) == [
        {"mapping": 1, "list": 2, "tuple": 3, "generator": 4, "chosen": 1, "keyword": 5},
        {"mapping": 1, "list": 2, "tuple": 3, "generator": 4, "chosen": 10, "keyword": 5},
    ]
    with pytest.raises(ValueError):
        {}.update([("too", "many", "items")])  # pyright: ignore[reportArgumentType, reportCallIssue, reportUnknownMemberType]
    with pytest.raises(ValueError):
        {}.update([("too",)])  # pyright: ignore[reportArgumentType, reportCallIssue, reportUnknownMemberType]


@_case("set_update")
def _set_update() -> None:
    def run(choose: Choose) -> set[int]:
        items = list(_EffectfulIterable(choose, (_CHOSEN,)))
        result: set[int] = set()
        result.update(items)
        return set(result)

    assert _outcomes(run) == [{1}, {10}]


@_case("set_update_unhashable_item_isolated")
def _set_update_unhashable_item_isolated() -> None:
    def run(choose: Choose) -> set[Any]:
        result: set[Any] = set()
        result.update(_EffectfulIterable(choose, (_CHOSEN,)))
        return result

    assert _outcomes(run, ([], 10)) == ["TypeError", {10}]


@_case("set_operations")
def _set_operations() -> None:
    operations: dict[str, Callable[[set[int], _EffectfulIterable], Any]] = {
        "union": set.union,  # pyright: ignore[reportUnknownMemberType]
        "intersection": set.intersection,  # pyright: ignore[reportUnknownMemberType]
        "difference": set.difference,  # pyright: ignore[reportUnknownMemberType]
        "symmetric_difference": set.symmetric_difference,  # pyright: ignore[reportUnknownMemberType]
        "isdisjoint": set.isdisjoint,  # pyright: ignore[reportUnknownMemberType]
    }
    expected = {
        "union": [{1, 2, 3}, {1, 2, 3, 10}],
        "intersection": [{1, 2}, {2}],
        "difference": [{3}, {1, 3}],
        "symmetric_difference": [{3}, {1, 3, 10}],
        "isdisjoint": [False, False],
    }
    for name, operation in operations.items():

        def run(choose: Choose, op: Callable[..., Any] = operation) -> Any:
            return op({1, 2, 3}, _EffectfulIterable(choose, (_CHOSEN, 2)))

        assert _outcomes(run) == expected[name]


@_case("set_multiple_operands")
def _set_multiple_operands() -> None:
    def update_run(choose: Choose) -> set[int]:
        items = list(_EffectfulIterable(choose, (_CHOSEN,)))
        result = {0}
        result.update(items, (3,))
        return set(result)

    def intersection_update_run(choose: Choose) -> set[int]:
        items = list(_EffectfulIterable(choose, (_CHOSEN, 2)))
        result = {1, 2, 3}
        result.intersection_update(items, (2, 3))
        return set(result)

    def difference_update_run(choose: Choose) -> set[int]:
        items = list(_EffectfulIterable(choose, (_CHOSEN,)))
        result = {1, 2, 3}
        result.difference_update(items, (2,))
        return set(result)

    def union_run(choose: Choose) -> set[int]:
        return {0}.union(_EffectfulIterable(choose, (_CHOSEN,)), (3,))

    def intersection_run(choose: Choose) -> set[int]:
        return {1, 2, 3}.intersection(_EffectfulIterable(choose, (_CHOSEN, 2)), (2, 3))

    def difference_run(choose: Choose) -> set[int]:
        return {1, 2, 3}.difference(_EffectfulIterable(choose, (_CHOSEN,)), (2,))

    assert _outcomes(update_run) == [{0, 1, 3}, {0, 3, 10}]
    assert _outcomes(intersection_update_run) == [{2}, {2}]
    assert _outcomes(difference_update_run) == [{3}, {1, 3}]
    assert _outcomes(union_run) == [{0, 1, 3}, {0, 3, 10}]
    assert _outcomes(intersection_run) == [{2}, {2}]
    assert _outcomes(difference_run) == [{3}, {1, 3}]


@_case("set_zero_operand_corner_cases")
def _set_zero_operand_corner_cases() -> None:
    def run(choose: Choose) -> tuple[Any, Any, Any, Any, Any, Any, Any]:
        result: set[int] = {1}
        update_result = result.update()
        intersection_update_result = result.intersection_update()
        difference_update_result = result.difference_update()
        return (
            update_result,
            intersection_update_result,
            difference_update_result,
            result.union(),
            result.intersection(),
            result.difference(),
            choose(),
        )

    assert _outcomes(run) == [
        (None, None, None, {1}, {1}, {1}, 1),
        (None, None, None, {1}, {1}, {1}, 10),
    ]


@_case("set_single_operand_signature_errors")
def _set_single_operand_signature_errors() -> None:
    with pytest.raises(TypeError):
        {1}.symmetric_difference()  # pyright: ignore[reportCallIssue]
    with pytest.raises(TypeError):
        {1}.symmetric_difference((2,), (3,))  # pyright: ignore[reportCallIssue]
    with pytest.raises(TypeError):
        {1}.symmetric_difference_update()  # pyright: ignore[reportCallIssue]
    with pytest.raises(TypeError):
        {1}.symmetric_difference_update((2,), (3,))  # pyright: ignore[reportCallIssue]
    with pytest.raises(TypeError):
        {1}.isdisjoint()  # pyright: ignore[reportCallIssue]
    with pytest.raises(TypeError):
        {1}.issubset((1,), (2,))  # pyright: ignore[reportCallIssue]


@_case("frozenset_operations")
def _frozenset_operations() -> None:
    operations: dict[str, Callable[[frozenset[int], _EffectfulIterable], Any]] = {
        "union": frozenset.union,  # pyright: ignore[reportUnknownMemberType]
        "intersection": frozenset.intersection,  # pyright: ignore[reportUnknownMemberType]
        "difference": frozenset.difference,  # pyright: ignore[reportUnknownMemberType]
        "symmetric_difference": frozenset.symmetric_difference,  # pyright: ignore[reportUnknownMemberType]
        "isdisjoint": frozenset.isdisjoint,  # pyright: ignore[reportUnknownMemberType]
        "issubset": frozenset.issubset,  # pyright: ignore[reportUnknownMemberType]
        "issuperset": frozenset.issuperset,  # pyright: ignore[reportUnknownMemberType]
    }
    expected = {
        "union": [frozenset({1, 2, 3}), frozenset({1, 2, 3, 10})],
        "intersection": [frozenset({1, 2}), frozenset({2})],
        "difference": [frozenset({3}), frozenset({1, 3})],
        "symmetric_difference": [frozenset({3}), frozenset({1, 3, 10})],
        "isdisjoint": [False, False],
        "issubset": [False, False],
        "issuperset": [True, False],
    }
    for name, operation in operations.items():

        def run(choose: Choose, op: Callable[..., Any] = operation) -> Any:
            return op(frozenset({1, 2, 3}), _EffectfulIterable(choose, (_CHOSEN, 2)))

        assert _outcomes(run) == expected[name]


@_case("set_subset_and_superset_element_callbacks")
def _set_subset_and_superset_element_callbacks() -> None:
    class EqualityKey:
        def __init__(self, choose: Choose) -> None:
            self.choose = choose

        def __hash__(self) -> int:
            return 1

        def __eq__(self, _other: Any) -> bool:
            return cast(bool, self.choose())

    for container_type in (set, frozenset):

        def subset_hash_run(choose: Choose) -> bool:
            return container_type({1}).issubset((_EffectfulHash(choose),))

        def superset_hash_run(choose: Choose) -> bool:
            return container_type({1, 10}).issuperset((_EffectfulHash(choose),))

        def subset_equality_run(choose: Choose) -> bool:
            return container_type({1}).issubset((EqualityKey(choose),))

        def superset_equality_run(choose: Choose) -> bool:
            return container_type({1}).issuperset((EqualityKey(choose),))

        assert _outcomes(subset_hash_run) == [True, False]
        assert _outcomes(superset_hash_run) == [True, True]
        assert _outcomes(subset_equality_run, (False, True)) == [False, True]
        assert _outcomes(superset_equality_run, (False, True)) == [False, True]


@_case("str_join")
def _str_join() -> None:
    def run(choose: Choose) -> str:
        return ",".join(_EffectfulIterable(choose, (_CHOSEN, "tail")))

    assert _outcomes(run, ("one", "ten")) == ["one,tail", "ten,tail"]


@_case("str_join_invalid_item_isolated")
def _str_join_invalid_item_isolated() -> None:
    def run(choose: Choose) -> str:
        return ",".join(_EffectfulIterable(choose, (_CHOSEN,)))

    assert _outcomes(run, (1, "ok")) == ["TypeError", "ok"]


@_case("bytes_and_bytearray_join")
def _bytes_and_bytearray_join() -> None:
    def bytes_run(choose: Choose) -> bytes:
        return b",".join(_EffectfulIterable(choose, (_CHOSEN, b"b")))

    def bytearray_run(choose: Choose) -> bytearray:
        return bytearray(b",").join(_EffectfulIterable(choose, (_CHOSEN, b"b")))

    assert _outcomes(bytes_run, (b"A", b"B")) == [b"A,b", b"B,b"]
    assert _outcomes(bytearray_run, (b"A", b"B")) == [bytearray(b"A,b"), bytearray(b"B,b")]


@_case("bytes_and_bytearray_join_invalid_item_isolated")
def _bytes_and_bytearray_join_invalid_item_isolated() -> None:
    def bytes_run(choose: Choose) -> bytes:
        return b",".join(_EffectfulIterable(choose, (_CHOSEN,)))

    def bytearray_run(choose: Choose) -> bytearray:
        return bytearray(b",").join(_EffectfulIterable(choose, (_CHOSEN,)))

    assert _outcomes(bytes_run, (1, b"ok")) == ["TypeError", b"ok"]
    assert _outcomes(bytearray_run, (1, b"ok")) == ["TypeError", bytearray(b"ok")]


@_case("bytes_join_empty_single_buffer")
def _bytes_join_empty_single_buffer() -> None:
    def run(choose: Choose) -> tuple[bytes, bytes, bytes]:
        choose()
        return (
            b"|".join(()),
            b"|".join((memoryview(b"single"),)),
            b"|".join((b"",)),
        )

    assert _outcomes(run) == [
        (b"", b"single", b""),
        (b"", b"single", b""),
    ]


def _effectful_buffer_join(
    choose: Choose,
    make_bytearray: bool,
) -> bytes | bytearray:
    class Item:
        def __buffer__(self, _flags: int) -> memoryview:
            value = choose()
            if isinstance(value, BaseException):
                raise value
            return cast(memoryview, value)

    if make_bytearray:
        return bytearray(b"|").join([Item()])
    return b"|".join([Item()])


@_case("bytes_join_effectful_buffer_two_shot")
def _bytes_join_effectful_buffer_two_shot() -> None:
    def run(choose: Choose) -> bytes:
        return cast(bytes, _effectful_buffer_join(choose, False))

    assert _outcomes(run, (memoryview(b"A"), memoryview(b"B"))) == [
        b"A",
        b"B",
    ]


@_case("bytearray_join_effectful_buffer_two_shot")
def _bytearray_join_effectful_buffer_two_shot() -> None:
    def run(choose: Choose) -> bytearray:
        return cast(bytearray, _effectful_buffer_join(choose, True))

    assert _outcomes(run, (memoryview(b"A"), memoryview(b"B"))) == [
        bytearray(b"A"),
        bytearray(b"B"),
    ]


@_case("bytes_join_invalid_effectful_buffer_isolated")
def _bytes_join_invalid_effectful_buffer_isolated() -> None:
    def run(choose: Choose) -> bytes:
        return cast(bytes, _effectful_buffer_join(choose, False))

    assert _outcomes(run, (1, memoryview(b"ok"))) == ["TypeError", b"ok"]


@_case("bytearray_join_invalid_effectful_buffer_isolated")
def _bytearray_join_invalid_effectful_buffer_isolated() -> None:
    def run(choose: Choose) -> bytearray:
        return cast(bytearray, _effectful_buffer_join(choose, True))

    assert _outcomes(run, (1, memoryview(b"ok"))) == [
        "TypeError",
        bytearray(b"ok"),
    ]


@_case("bytes_join_effectful_buffer_exception_isolated")
def _bytes_join_effectful_buffer_exception_isolated() -> None:
    def run(choose: Choose) -> bytes:
        return cast(bytes, _effectful_buffer_join(choose, False))

    assert _outcomes(run, (RuntimeError("buffer"), memoryview(b"ok"))) == [
        "RuntimeError",
        b"ok",
    ]


@_case("bytearray_join_effectful_buffer_exception_isolated")
def _bytearray_join_effectful_buffer_exception_isolated() -> None:
    def run(choose: Choose) -> bytearray:
        return cast(bytearray, _effectful_buffer_join(choose, True))

    assert _outcomes(run, (RuntimeError("buffer"), memoryview(b"ok"))) == [
        "RuntimeError",
        bytearray(b"ok"),
    ]


if sys.version_info < (3, 14):

    @_case("list_index_missing_effectful_repr_isolated")
    def _list_index_missing_effectful_repr_isolated() -> None:
        def run(choose: Choose) -> tuple[str, str]:
            class Missing:
                def __repr__(self) -> str:
                    value = choose()
                    if isinstance(value, BaseException):
                        raise value
                    return cast(str, value)

            values: list[object] = []
            try:
                values.index(Missing())
            except Exception as exc:
                return type(exc).__name__, str(exc)
            raise AssertionError("list.index unexpectedly found the target")

        assert _outcomes(
            run,
            ("<first>", RuntimeError("repr failed"), 1),
        ) == [
            ("ValueError", "<first> is not in list"),
            ("RuntimeError", "repr failed"),
            ("TypeError", "__repr__ returned non-string (type int)"),
        ]


@_case("str_format_methods_preserve_cpython_semantics")
def _str_format_methods_preserve_cpython_semantics() -> None:
    class Value:
        attribute = "attribute"

        def __getitem__(self, key: str) -> str:
            return f"item:{key}"

        def __format__(self, spec: str) -> str:
            return f"formatted:{spec}"

        def __repr__(self) -> str:
            return "represented"

    value = Value()
    assert "{0.attribute} {0[key]}".format(value) == "attribute item:key"
    assert "{0!r} {0:{1}}".format(value, "spec") == ("represented formatted:spec")
    assert "{{{value}}} {other}".format_map({"value": 1, "other": 2}) == "{1} 2"

    with pytest.raises(ValueError):
        "{0} {}".format(value, value)  # noqa: F523, F525
    with pytest.raises(ValueError):
        "{0}".format_map({"0": value})
    with pytest.raises(ValueError):
        "unmatched }".format_map({})


@_case("range_count")
def _range_count() -> None:
    def run(choose: Choose) -> int:
        value = _EffectfulEquality(choose)
        return range(1).count(value)  # pyright: ignore[reportArgumentType]

    assert _outcomes(run, (False, True)) == [0, 1]


@_case("range_index")
def _range_index() -> None:
    def run(choose: Choose) -> int:
        return range(1).index(_EffectfulEquality(choose))  # pyright: ignore[reportArgumentType]

    assert _outcomes(run, (False, True)) == ["ValueError", 0]


@_case("slice_indices")  # pyright: ignore[reportArgumentType]
def _slice_indices() -> tuple[int, int, int]:  # pyright: ignore[reportReturnType]
    def run(choose: Choose) -> tuple[int, int, int]:
        value = _EffectfulIndex(choose)
        return slice(value, 8, 2).indices(10)

    assert _outcomes(run) == [(1, 8, 2), (10, 8, 2)]


@_case("slice_indices_zero_step")
def _slice_indices_zero_step() -> None:
    def run(choose: Choose) -> tuple[int, int, int]:
        return slice(1, 8, _EffectfulIndex(choose)).indices(10)

    assert _outcomes(run, (0, 2)) == ["ValueError", (1, 8, 2)]


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, __file__, "--case", name],
        text=True,
        capture_output=True,
        check=False,
    )


@pytest.mark.parametrize("case_name", tuple(_CASES))
def test_container_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize(
    "case_name",
    ("bytes_and_bytearray_join", "bytes_and_bytearray_join_invalid_item_isolated"),
)
def test_join_continuation_repeated_in_isolated_processes(case_name: str) -> None:
    for _ in range(8):
        result = _run_case(case_name)
        assert result.returncode == 0, result.stdout + result.stderr


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_containers.py --case NAME")
    _CASES[sys.argv[2]]()
