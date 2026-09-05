"""Subprocess-isolated continuation coverage for numeric C accelerators."""

from __future__ import annotations

from collections.abc import Callable, Iterator
import cmath
import math
from pathlib import Path
import struct
import subprocess
import sys
from typing import Any, cast
import warnings

import pytest

from aleff import create_handler, effect


Choose = Callable[[], Any]
Run = Callable[[Choose], Any]
Fresh = Callable[[Any], Any]
Outcome = tuple[str, Any]
Case = Callable[[], None]
_CASES: dict[str, Case] = {}
_MATH_REQUIREMENTS: dict[str, str] = {}

_RAISE = object()
_INVALID = object()
_STOP = object()


class ExpectedCallbackError(Exception):
    """An exception raised by a test callback."""


def _case(name: str, *, requires_math: str | None = None) -> Callable[[Case], Case]:
    def register(case: Case) -> Case:
        _CASES[name] = case
        if requires_math is not None:
            _MATH_REQUIREMENTS[name] = requires_math
        return case

    return register


def _outcome(call: Callable[[], Any]) -> Outcome:
    try:
        return "return", call()
    except BaseException as exc:
        return "raise", (type(exc).__name__, str(exc))


def _resume_against_fresh(
    run: Run,
    fresh: Fresh,
    decisions: tuple[Any, ...],
    effect_name: str,
    *,
    reset: Callable[[], None] | None = None,
) -> list[Outcome]:
    """Compare each multi-shot resume with a fresh ordinary execution."""

    choose = effect(effect_name)
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        outcomes: list[Outcome] = []
        for decision in decisions:
            if reset is not None:
                reset()
            actual = _outcome(lambda decision=decision: k(decision))
            expected = _outcome(lambda decision=decision: fresh(decision))
            assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            outcomes.append(actual)
        return outcomes

    result = handler(lambda: run(choose))
    assert suspension_count == 1, "the scenario must suspend exactly once"
    return cast(list[Outcome], result)


class _EffectfulNumber:
    def __init__(self, choose: Choose, protocol: str, transform: Callable[[Any], Any] | None = None) -> None:
        self._choose = choose
        self._protocol = protocol
        self._transform: Callable[[Any], Any] = transform or (lambda value: value)

    def _value(self) -> Any:
        value = self._choose()
        if value is _RAISE:
            raise ExpectedCallbackError("numeric callback failed")
        if value is _INVALID:
            return object() if self._protocol == "float" else "not-an-index"
        return self._transform(value)

    def __float__(self) -> float:
        if self._protocol != "float":
            raise TypeError("not a float protocol case")
        return float(self._value())

    def __index__(self) -> int:
        if self._protocol != "index":
            raise TypeError("not an index protocol case")
        value = self._value()
        return value

    def __ceil__(self) -> int:
        if self._protocol != "special":
            raise TypeError("not a rounding protocol case")
        return int(self._value())

    def __floor__(self) -> int:
        if self._protocol != "special":
            raise TypeError("not a rounding protocol case")
        return int(self._value())

    def __trunc__(self) -> int:
        if self._protocol != "special":
            raise TypeError("not a rounding protocol case")
        return int(self._value())


class _EffectfulIndexNumber:
    def __init__(self, choose: Choose) -> None:
        self._choose = choose

    def __index__(self) -> int:
        value = self._choose()
        if value is _RAISE:
            raise ExpectedCallbackError("numeric callback failed")
        if value is _INVALID:
            return "not-an-index"  # type: ignore[return-value]
        return cast(int, value)


class _EffectfulComplex:
    def __init__(self, choose: Choose) -> None:
        self.choose = choose

    def __complex__(self) -> complex:
        value = self.choose()
        if value is _RAISE:
            raise ExpectedCallbackError("complex callback failed")
        if value is _INVALID:
            return "not-a-complex"  # type: ignore[return-value]
        return complex(value)


class _EffectfulIterator:
    """An iterator whose first item is supplied by the continuation handler."""

    def __init__(self, choose: Choose, values: tuple[Any, ...], protocol: str) -> None:
        self.choose = choose
        self.values = values
        self.protocol = protocol
        self.position = -1

    def __iter__(self) -> "_EffectfulIterator":
        return self

    def __next__(self) -> Any:
        if self.position < 0:
            decision = self.choose()
            if decision is _RAISE:
                raise ExpectedCallbackError("iterator callback failed")
            if decision is _STOP:
                raise StopIteration
            if decision is _INVALID:
                value = object() if self.protocol == "float" else "not-an-index"
            else:
                value = decision
            self.position = 0
            return value
        if self.position >= len(self.values):
            raise StopIteration
        value = self.values[self.position]
        self.position += 1
        return value


_MATH_FLOAT_CALLS: dict[str, Callable[[Any], Any]] = {
    "acos": lambda x: math.acos(x),
    "acosh": lambda x: math.acosh(x),
    "asin": lambda x: math.asin(x),
    "asinh": lambda x: math.asinh(x),
    "atan": lambda x: math.atan(x),
    "atan2": lambda x: math.atan2(x, 1.0),
    "atanh": lambda x: math.atanh(x),
    "cbrt": lambda x: getattr(math, "cbrt")(x),
    "copysign": lambda x: math.copysign(x, -1.0),
    "cos": lambda x: math.cos(x),
    "cosh": lambda x: math.cosh(x),
    "degrees": lambda x: math.degrees(x),
    "erf": lambda x: math.erf(x),
    "erfc": lambda x: math.erfc(x),
    "exp": lambda x: math.exp(x),
    "exp2": lambda x: getattr(math, "exp2")(x),
    "expm1": lambda x: math.expm1(x),
    "fabs": lambda x: math.fabs(x),
    "fma": lambda x: getattr(math, "fma")(x, 2.0, 3.0),
    "fmod": lambda x: math.fmod(x, 2.0),
    "frexp": lambda x: math.frexp(x),
    "gamma": lambda x: math.gamma(x),
    "hypot": lambda x: math.hypot(x, 4.0),
    "isclose": lambda x: math.isclose(x, 1.0),
    "isfinite": lambda x: math.isfinite(x),
    "isinf": lambda x: math.isinf(x),
    "isnan": lambda x: math.isnan(x),
    "ldexp": lambda x: math.ldexp(x, 2),
    "lgamma": lambda x: math.lgamma(x),
    "log": lambda x: math.log(x),
    "log10": lambda x: math.log10(x),
    "log1p": lambda x: math.log1p(x),
    "log2": lambda x: math.log2(x),
    "modf": lambda x: math.modf(x),
    "nextafter": lambda x: math.nextafter(x, 2.0),
    "pow": lambda x: math.pow(x, 3.0),
    "radians": lambda x: math.radians(x),
    "remainder": lambda x: math.remainder(x, 2.0),
    "sin": lambda x: math.sin(x),
    "sinh": lambda x: math.sinh(x),
    "sqrt": lambda x: math.sqrt(x),
    "tan": lambda x: math.tan(x),
    "tanh": lambda x: math.tanh(x),
    "ulp": lambda x: math.ulp(x),
}

_MATH_SPECIAL_CALLS: dict[str, Callable[[Any], Any]] = {
    "ceil": lambda x: math.ceil(x),
    "floor": lambda x: math.floor(x),
    "trunc": lambda x: math.trunc(x),
}

_MATH_INDEX_CALLS: dict[str, Callable[[Any], Any]] = {
    "comb": lambda x: math.comb(x, 2),
    "factorial": lambda x: math.factorial(x),
    "gcd": lambda x: math.gcd(x, 8),
    "isqrt": lambda x: math.isqrt(x),
    "lcm": lambda x: math.lcm(x, 8),
    "perm": lambda x: math.perm(x, 2),
}

_MATH_ITERATOR_NAMES = ("dist", "fsum", "prod", "sumprod")

_CMATH_CALLS: dict[str, Callable[[Any], Any]] = {
    "acos": lambda x: cmath.acos(x),
    "acosh": lambda x: cmath.acosh(x),
    "asin": lambda x: cmath.asin(x),
    "asinh": lambda x: cmath.asinh(x),
    "atan": lambda x: cmath.atan(x),
    "atanh": lambda x: cmath.atanh(x),
    "cos": lambda x: cmath.cos(x),
    "cosh": lambda x: cmath.cosh(x),
    "exp": lambda x: cmath.exp(x),
    "isclose": lambda x: cmath.isclose(x, 0.5 + 0.25j),
    "isfinite": lambda x: cmath.isfinite(x),
    "isinf": lambda x: cmath.isinf(x),
    "isnan": lambda x: cmath.isnan(x),
    "log": lambda x: cmath.log(x),
    "log10": lambda x: cmath.log10(x),
    "phase": lambda x: cmath.phase(x),
    "polar": lambda x: cmath.polar(x),
    "rect": lambda x: cmath.rect(x, 0.5),
    "sin": lambda x: cmath.sin(x),
    "sinh": lambda x: cmath.sinh(x),
    "sqrt": lambda x: cmath.sqrt(x),
    "tan": lambda x: cmath.tan(x),
    "tanh": lambda x: cmath.tanh(x),
}


def _numeric_case(name: str, protocol: str, operation: Callable[[Any], Any]) -> None:
    if name == "acosh":
        values = (1.25, 1.5)
    else:
        values = (0.25, 0.5) if protocol == "float" else (5, 6)
    decisions = (_RAISE, _INVALID, *values, values[0])

    def run(choose: Choose) -> Any:
        return operation(_EffectfulNumber(choose, protocol))

    def fresh(decision: Any) -> Any:
        return operation(_EffectfulNumber(lambda: decision, protocol))

    outcomes = _resume_against_fresh(run, fresh, decisions, f"math-{name}")
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[4]
    assert outcomes[2][0] == "return"


for _name, _operation in _MATH_FLOAT_CALLS.items():
    _case(f"math_{_name}_numeric_conversion_multishot", requires_math=_name)(
        lambda name=_name, operation=_operation: _numeric_case(name, "float", operation)
    )

for _name, _operation in _MATH_INDEX_CALLS.items():
    _case(f"math_{_name}_numeric_conversion_multishot", requires_math=_name)(
        lambda name=_name, operation=_operation: _numeric_case(name, "index", operation)
    )

for _name, _operation in _MATH_SPECIAL_CALLS.items():
    _case(f"math_{_name}_numeric_conversion_multishot", requires_math=_name)(
        lambda name=_name, operation=_operation: _numeric_case(name, "special", operation)
    )


def _math_special_passthrough_case(name: str) -> None:
    choose = effect(f"math-{name}-passthrough")
    handler = create_handler(choose)

    class Value:
        def special(self) -> Any:
            decision = choose()
            if decision is _RAISE:
                raise ExpectedCallbackError(f"math.{name} callback failed")
            return decision

        __ceil__ = special
        __floor__ = special
        __trunc__ = special

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        return [_outcome(lambda decision=decision: k(decision)) for decision in (_RAISE, "sentinel", 3)]

    outcomes = cast(list[Outcome], handler(lambda: getattr(math, name)(Value())))
    assert outcomes[0][0] == "raise"
    assert outcomes[1] == ("return", "sentinel")
    assert outcomes[2] == ("return", 3)


for _name in _MATH_SPECIAL_CALLS:
    _case(f"math_{_name}_special_result_passthrough_multishot", requires_math=_name)(
        lambda name=_name: _math_special_passthrough_case(name)
    )


def _math_special_numeric_subclass_case(name: str, base: type[int] | type[float]) -> None:
    special_name = f"__{name}__"

    def call(choose: Choose) -> Any:
        def special(_self: Any) -> Any:
            decision = choose()
            if decision is _RAISE:
                raise ExpectedCallbackError(f"math.{name} subclass callback failed")
            return decision

        value_type = type("Value", (base,), {special_name: special})
        value = value_type(1.25 if base is float else 1)
        return getattr(math, name)(value)

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, "sentinel", 3, "sentinel"),
        f"math-{name}-{base.__name__}-subclass",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1] == outcomes[3] == ("return", "sentinel")
    assert outcomes[2] == ("return", 3)


for _name in _MATH_SPECIAL_CALLS:
    for _base in (int, float):
        _case(
            f"math_{_name}_{_base.__name__}_subclass_special_multishot",
            requires_math=_name,
        )(
            lambda name=_name, base=_base: _math_special_numeric_subclass_case(
                name,
                base,
            )
        )


def _cmath_case(name: str, operation: Callable[[Any], Any]) -> None:
    values = (0.25, 0.5) if name == "rect" else (0.25 + 0.5j, 0.5 + 0.25j)
    decisions = (_RAISE, _INVALID, *values, values[0])

    def run(choose: Choose) -> Any:
        if name == "rect":
            return operation(_EffectfulNumber(choose, "float"))
        return operation(_EffectfulComplex(choose))

    def fresh(decision: Any) -> Any:
        if name == "rect":
            return operation(_EffectfulNumber(lambda: decision, "float"))
        return operation(_EffectfulComplex(lambda: decision))

    outcomes = _resume_against_fresh(run, fresh, decisions, f"cmath-{name}")
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[4]


for _name, _operation in _CMATH_CALLS.items():
    _case(f"cmath_{_name}_numeric_conversion_multishot")(
        lambda name=_name, operation=_operation: _cmath_case(name, operation)
    )


def _cmath_fallback_case(protocol: str) -> None:
    values = (0.25, 0.5) if protocol == "float" else (1, 2)
    decisions = (_RAISE, _INVALID, *values, values[0])

    def run(choose: Choose) -> Any:
        value = _EffectfulNumber(choose, protocol) if protocol == "float" else _EffectfulIndexNumber(choose)
        return cmath.sin(value)

    def fresh(decision: Any) -> Any:
        value = (
            _EffectfulNumber(lambda: decision, protocol)
            if protocol == "float"
            else _EffectfulIndexNumber(lambda: decision)
        )
        return cmath.sin(value)

    outcomes = _resume_against_fresh(run, fresh, decisions, f"cmath-{protocol}-fallback")
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[4]
    assert outcomes[2][0] == "return"


for _protocol in ("float", "index"):
    _case(f"cmath_{_protocol}_fallback_multishot")(lambda protocol=_protocol: _cmath_fallback_case(protocol))


def _iterator_math_case(name: str) -> None:
    current: list[_EffectfulIterator] = []

    def make(choose: Choose) -> _EffectfulIterator:
        if name == "dist":
            values = (4.0,)
            protocol = "float"
        elif name == "sumprod":
            values = (2.0,)
            protocol = "float"
        else:
            values = (2.0,)
            protocol = "float"
        iterator = _EffectfulIterator(choose, values, protocol)
        current[:] = [iterator]
        return iterator

    def call(iterator: Iterator[Any]) -> Any:
        if name == "dist":
            return math.dist(iterator, (0.0, 0.0))
        if name == "fsum":
            return math.fsum(iterator)
        if name == "prod":
            return math.prod(iterator)
        if name == "sumprod":
            return math.sumprod(iterator, (3.0, 4.0))
        raise AssertionError(name)

    def run(choose: Choose) -> Any:
        return call(make(choose))

    def fresh(decision: Any) -> Any:
        if name == "dist":
            values = (4.0,)
            protocol = "float"
        elif name == "sumprod":
            values = (2.0,)
            protocol = "float"
        else:
            values = (2.0,)
            protocol = "float"
        return call(_EffectfulIterator(lambda: decision, values, protocol))

    def reset() -> None:
        if current:
            current[0].position = -1

    outcomes = _resume_against_fresh(
        run,
        fresh,
        (_RAISE, _INVALID, 1, 3, 1),
        f"math-{name}-iterator",
        reset=reset,
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[4]


for _name in _MATH_ITERATOR_NAMES:
    _case(f"math_{_name}_iterator_state_multishot", requires_math=_name)(lambda name=_name: _iterator_math_case(name))


def _numeric_iterator_callback_case(name: str) -> None:
    def operation(choose: Choose) -> Any:
        if name == "fsum":
            return math.fsum([_EffectfulNumber(choose, "float"), 2.0])
        if name == "dist":
            return math.dist([_EffectfulNumber(choose, "float"), 4.0], [0.0, 0.0])

        class Product:
            def __mul__(self, other: object) -> Any:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError(f"{name} multiplication failed")
                if decision is _INVALID:
                    return object()
                return decision

            __rmul__ = __mul__

        if name == "prod":
            return math.prod([Product(), 2])
        return math.sumprod(cast(Any, [Product()]), [2])

    def run(choose: Choose) -> Any:
        return operation(choose)

    def fresh(decision: Any) -> Any:
        return operation(lambda: decision)

    decisions = (_RAISE, _INVALID, 3, 5, 3)
    outcomes = _resume_against_fresh(run, fresh, decisions, f"math-{name}-item-callback")
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[4]
    assert outcomes[2][0] == "return"


for _name in ("fsum", "dist", "prod", "sumprod"):
    _case(f"math_{_name}_item_callback_multishot", requires_math=_name)(
        lambda name=_name: _numeric_iterator_callback_case(name)
    )


def _math_dict_iterator_snapshot_case() -> None:
    def call(choose: Choose) -> Any:
        class Value:
            def __float__(self) -> float:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("dict key float callback failed")
                if decision is _INVALID:
                    return cast(Any, object())
                return float(decision)

            def __index__(self) -> int:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("dict key index callback failed")
                if decision is _INVALID:
                    return cast(Any, "not-an-index")
                return cast(int, decision)

        source = {Value(): None, 2: None, 3: None}
        return math.fsum(iter(source))

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, 1, 4, 1),
        "math-dict-iterator-snapshot",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[4]
    assert outcomes[2][0] == "return"


@_case("math_fsum_dict_iterator_snapshot_multishot", requires_math="fsum")
def _math_fsum_dict_iterator_snapshot_multishot() -> None:
    _math_dict_iterator_snapshot_case()


def _math_dict_iterator_mutation_case() -> None:
    choose = effect("math-dict-iterator-mutation")
    handler = create_handler(choose)
    source: dict[Any, None] = {}

    class Value:
        def __float__(self) -> float:
            return float(choose())

        def __index__(self) -> int:
            return cast(int, choose())

    source.update({Value(): None, 2: None})

    @handler.on(choose)
    def resume(k: Any) -> Any:
        source[3] = None
        return k(1)

    def consume() -> Any:
        return math.fsum(iter(source))

    with pytest.raises(RuntimeError, match="dictionary changed size during iteration"):
        handler(consume)


@_case("math_fsum_dict_iterator_mutation_multishot", requires_math="fsum")
def _math_fsum_dict_iterator_mutation_multishot() -> None:
    _math_dict_iterator_mutation_case()


def _math_dict_iterator_late_mutation_case() -> None:
    choose = effect("math-dict-iterator-late-mutation")
    handler = create_handler(choose)
    source: dict[Any, None] = {}
    should_mutate = False

    class EffectfulValue:
        def __float__(self) -> float:
            return float(choose())

        def __index__(self) -> int:
            return cast(int, choose())

    class MutatingValue:
        def __float__(self) -> float:
            if should_mutate:
                source[3] = None
            return 2.0

        def __index__(self) -> int:
            if should_mutate:
                source[3] = None
            return 2

    source.update({EffectfulValue(): None, MutatingValue(): None})

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal should_mutate
        should_mutate = False
        first = _outcome(lambda: k(1))
        should_mutate = True
        second = _outcome(lambda: k(1))
        return [first, second]

    def consume() -> Any:
        return math.fsum(iter(source))

    outcomes = handler(consume)
    assert outcomes[0][0] == "return"
    assert outcomes[1] == (
        "raise",
        ("RuntimeError", "dictionary changed size during iteration"),
    )


@_case("math_fsum_dict_iterator_late_mutation_multishot", requires_math="fsum")
def _math_fsum_dict_iterator_late_mutation_multishot() -> None:
    _math_dict_iterator_late_mutation_case()


def _iterator_snapshot_outcomes(
    target: Iterator[Any],
    expected_first: Outcome,
    expected_second: Outcome,
    *,
    mutate: Callable[[], None] | None = None,
    normalize: Callable[[list[Any]], Any] = tuple,
) -> None:
    choose = effect("native-iterator-snapshot")
    handler = create_handler(choose)
    mutate_enabled = False
    trigger_position = 0

    class Trigger:
        def __iter__(self) -> "Trigger":
            return self

        def __next__(self) -> None:
            nonlocal trigger_position
            if trigger_position == 0:
                choose()
                if mutate_enabled and mutate is not None:
                    mutate()
            trigger_position += 1
            return None

    trigger = Trigger()

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal mutate_enabled, trigger_position

        def resume_once() -> Outcome:
            raw = _outcome(lambda: k(None))
            if raw[0] == "raise":
                return raw
            pairs = cast(tuple[tuple[None, Any], ...], raw[1])
            return "return", normalize([value for _, value in pairs])

        mutate_enabled = False
        trigger_position = 0
        first = resume_once()
        mutate_enabled = True
        trigger_position = 0
        second = resume_once()
        return [first, second]

    outcomes = handler(lambda: tuple(zip(trigger, target)))
    assert outcomes == [expected_first, expected_second], (
        f"actual={outcomes!r}, expected={[expected_first, expected_second]!r}"
    )


def _dict_iterator(source: dict[str, int], kind: str) -> Iterator[Any]:
    reverse = kind.startswith("reverse_")
    view_name = kind.removeprefix("reverse_")
    view: Any
    if view_name == "key":
        view = source
    elif view_name == "value":
        view = source.values()
    else:
        view = source.items()
    return cast(Iterator[Any], reversed(view) if reverse else iter(view))


def _dict_iterator_expected(kind: str, values: tuple[int, int, int]) -> tuple[Any, ...]:
    keys = ("a", "b", "c")
    entries: tuple[Any, ...]
    view_name = kind.removeprefix("reverse_")
    if view_name == "key":
        entries = keys
    elif view_name == "value":
        entries = values
    else:
        entries = tuple(zip(keys, values))
    ordered = tuple(reversed(entries)) if kind.startswith("reverse_") else entries
    return ordered[1:]


def _dict_native_iterator_snapshot_case(kind: str, scenario: str) -> None:
    source = {"a": 10, "b": 20, "c": 30}
    target = _dict_iterator(source, kind)
    original_tail = _dict_iterator_expected(kind, (10, 20, 30))

    if scenario == "exhausted":
        tuple(target)
        _iterator_snapshot_outcomes(
            target,
            ("return", ()),
            ("return", ()),
        )
        return

    if scenario == "sticky":
        source["d"] = 40
        with pytest.raises(RuntimeError, match="dictionary changed size during iteration"):
            next(target)
        del source["d"]
        expected = (
            "raise",
            ("RuntimeError", "dictionary changed size during iteration"),
        )
        _iterator_snapshot_outcomes(target, expected, expected)
        return

    next(target)
    first = ("return", original_tail)
    if scenario == "normal":
        _iterator_snapshot_outcomes(target, first, first)
        return
    if scenario == "size-change":
        expected = (
            "raise",
            ("RuntimeError", "dictionary changed size during iteration"),
        )
        _iterator_snapshot_outcomes(
            target,
            first,
            expected,
            mutate=lambda: source.__setitem__("d", 40),
        )
        return
    if scenario == "value-change":
        updated_tail = _dict_iterator_expected(kind, (100, 200, 300))

        def update_values() -> None:
            source.update(a=100, b=200, c=300)

        expected = first if kind.endswith("key") else ("return", updated_tail)
        _iterator_snapshot_outcomes(target, first, expected, mutate=update_values)
        return

    assert scenario == "same-size-key-change"
    expected = (
        "raise",
        ("RuntimeError", "dictionary keys changed during iteration"),
    )

    def replace_consumed_key() -> None:
        del source["a" if not kind.startswith("reverse_") else "c"]
        source["d"] = 40

    if kind.startswith("reverse_"):
        pytest.skip("reverse dict iterators do not report same-size key changes")
    _iterator_snapshot_outcomes(target, first, expected, mutate=replace_consumed_key)


_DICT_ITERATOR_KINDS = (
    "key",
    "value",
    "item",
    "reverse_key",
    "reverse_value",
    "reverse_item",
)

for _kind in _DICT_ITERATOR_KINDS:
    for _scenario in ("normal", "size-change", "value-change", "sticky", "exhausted"):
        _case(f"dict_{_kind}_iterator_{_scenario.replace('-', '_')}_multishot")(
            lambda kind=_kind, scenario=_scenario: _dict_native_iterator_snapshot_case(kind, scenario)
        )

for _kind in ("key", "value", "item"):
    _case(f"dict_{_kind}_iterator_same_size_key_change_multishot")(
        lambda kind=_kind: _dict_native_iterator_snapshot_case(kind, "same-size-key-change")
    )


def _set_native_iterator_snapshot_case(frozen: bool, scenario: str) -> None:
    source: set[int] | frozenset[int] = frozenset((0, 1, 2)) if frozen else {0, 1, 2}
    target = iter(source)
    consumed = next(target)
    expected_tail = tuple(sorted(set(source) - {consumed}))

    def normalize(values: list[Any]) -> tuple[Any, ...]:
        return tuple(sorted(values))

    if scenario == "normal":
        expected = ("return", expected_tail)
        _iterator_snapshot_outcomes(
            target,
            expected,
            expected,
            normalize=normalize,
        )
        return
    if scenario == "exhausted":
        tuple(target)
        _iterator_snapshot_outcomes(
            target,
            ("return", ()),
            ("return", ()),
            normalize=normalize,
        )
        return

    assert not frozen
    mutable = cast(set[int], source)
    expected_error = (
        "raise",
        ("RuntimeError", "Set changed size during iteration"),
    )
    if scenario == "sticky":
        mutable.add(3)
        with pytest.raises(RuntimeError, match="Set changed size during iteration"):
            next(target)
        mutable.remove(3)
        _iterator_snapshot_outcomes(
            target,
            expected_error,
            expected_error,
            normalize=normalize,
        )
        return

    assert scenario == "size-change"
    _iterator_snapshot_outcomes(
        target,
        ("return", expected_tail),
        expected_error,
        mutate=lambda: mutable.add(3),
        normalize=normalize,
    )


for _frozen in (False, True):
    for _scenario in ("normal", "exhausted"):
        _case(f"{'frozenset' if _frozen else 'set'}_iterator_{_scenario}_multishot")(
            lambda frozen=_frozen, scenario=_scenario: _set_native_iterator_snapshot_case(frozen, scenario)
        )

for _scenario in ("size-change", "sticky"):
    _case(f"set_iterator_{_scenario.replace('-', '_')}_multishot")(
        lambda scenario=_scenario: _set_native_iterator_snapshot_case(False, scenario)
    )


def _struct_pack_case(api: str) -> None:
    current: list[bytearray] = []

    def run(choose: Choose) -> Any:
        value = _EffectfulNumber(choose, "index")
        target = bytearray(8)
        current[:] = [target]
        if api == "pack":
            return struct.pack("2i", value, 2)
        if api == "pack_into":
            result = struct.pack_into("2i", target, 0, value, 2)
        elif api == "Struct.pack":
            result = struct.Struct("2i").pack(value, 2)
        else:
            result = struct.Struct("2i").pack_into(target, 0, value, 2)
        return result, bytes(target)

    def fresh(decision: Any) -> Any:
        value = _EffectfulNumber(lambda: decision, "index")
        target = bytearray(8)
        if api == "pack":
            return struct.pack("2i", value, 2)
        if api == "pack_into":
            result = struct.pack_into("2i", target, 0, value, 2)
        elif api == "Struct.pack":
            result = struct.Struct("2i").pack(value, 2)
        else:
            result = struct.Struct("2i").pack_into(target, 0, value, 2)
        return result, bytes(target)

    def reset() -> None:
        if current:
            current[0][:] = b"\0" * 8

    outcomes = _resume_against_fresh(
        run,
        fresh,
        (_RAISE, _INVALID, 11, 13, 11),
        f"struct-{api}",
        reset=reset,
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[4]


for _api in ("pack", "pack_into", "Struct.pack", "Struct.pack_into"):
    _case(f"{_api.replace('.', '_')}_numeric_conversion_multishot")(lambda api=_api: _struct_pack_case(api))


def _numeric_keyword_case(module_name: str, keyword: str) -> None:
    operation = math.isclose if module_name == "math" else cmath.isclose

    def call(choose: Choose) -> Any:
        tolerance = _EffectfulNumber(choose, "float")
        kwargs: dict[str, Any] = {keyword: tolerance}
        return operation(1.0, 1.0, **kwargs)

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, 1e-9, -1.0, 1e-9),
        f"{module_name}-isclose-{keyword}",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[3][0] == "raise"
    assert outcomes[2] == outcomes[4]


for _module_name in ("math", "cmath"):
    for _keyword in ("rel_tol", "abs_tol"):
        _case(
            f"{_module_name}_isclose_{_keyword}_keyword_multishot",
            requires_math="isclose" if _module_name == "math" else None,
        )(
            lambda module_name=_module_name, keyword=_keyword: _numeric_keyword_case(
                module_name,
                keyword,
            )
        )


@_case("math_nextafter_steps_keyword_index_multishot", requires_math="nextafter")
def _math_nextafter_steps_keyword_index_multishot() -> None:
    def call(choose: Choose) -> Any:
        return math.nextafter(
            1.0,
            2.0,
            steps=_EffectfulNumber(choose, "index"),
        )

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, 1, 2, 1),
        "math-nextafter-steps",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


def _assert_numeric_callback_not_called(
    name: str,
    call: Callable[[Choose], Any],
) -> None:
    choose = effect(name)
    handler = create_handler(choose)
    callback_count = 0

    @handler.on(choose)
    def resume(_continuation: Any) -> int:
        nonlocal callback_count
        callback_count += 1
        return 1

    outcome = _outcome(lambda: handler(lambda: call(choose)))
    assert outcome[0] == "raise"
    assert outcome[1][0] == "TypeError"
    assert callback_count == 0


@_case(
    "math_non_callback_integer_and_signature_validation",
    requires_math="nextafter",
)
def _math_non_callback_integer_and_signature_validation() -> None:
    _assert_numeric_callback_not_called(
        "math-ldexp-exponent-no-index",
        lambda choose: math.ldexp(
            1.0,
            cast(Any, _EffectfulIndexNumber(choose)),
        ),
    )
    _assert_numeric_callback_not_called(
        "math-nextafter-third-positional",
        lambda choose: cast(Callable[..., Any], math.nextafter)(
            1.0,
            2.0,
            cast(Any, _EffectfulIndexNumber(choose)),
        ),
    )
    _assert_numeric_callback_not_called(
        "math-sin-extra-positional",
        lambda choose: cast(Callable[..., Any], math.sin)(
            cast(Any, _EffectfulNumber(choose, "float")),
            1.0,
        ),
    )


@_case("math_float_index_fallback_multishot", requires_math="sin")
def _math_float_index_fallback_multishot() -> None:
    def call(choose: Choose) -> Any:
        return math.sin(cast(Any, _EffectfulIndexNumber(choose)))

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, 1, 2, 1),
        "math-float-index-fallback",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


def _math_index_subclass_warning_case(name: str) -> None:
    class IndexResult(int):
        pass

    def call(choose: Choose) -> Any:
        class Value:
            def __index__(self) -> int:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("index callback failed")
                return cast(int, decision)

        value = cast(Any, Value())
        if name == "sin":
            return math.sin(value)
        if name == "fsum":
            return math.fsum((value,))
        return math.dist((value,), (0.0,))

    def capture(operation: Callable[[], Any]) -> Any:
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            result = operation()
        return result, tuple((item.category.__name__, str(item.message)) for item in caught)

    decisions = (_RAISE, 1, IndexResult(2), 1)
    choose = effect(f"math-{name}-index-subclass-warning")
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        outcomes: list[Outcome] = []
        for decision in decisions:
            actual = _outcome(lambda decision=decision: capture(lambda: k(decision)))
            expected = _outcome(lambda decision=decision: capture(lambda: call(lambda: decision)))
            assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            outcomes.append(actual)
        return outcomes

    outcomes = cast(list[Outcome], handler(lambda: call(choose)))
    assert suspension_count == 1
    assert outcomes[0][0] == "raise"
    assert outcomes[1] == outcomes[3]
    assert outcomes[2][0] == "return"
    assert outcomes[2][1][1][0][0] == "DeprecationWarning"


for _name in ("sin", "fsum", "dist"):
    _case(
        f"math_{_name}_index_subclass_warning_multishot",
        requires_math=_name,
    )(lambda name=_name: _math_index_subclass_warning_case(name))


def _math_long_float_type_name_case(name: str) -> None:
    def call(choose: Choose) -> Any:
        def to_float(_self: Any) -> float:
            decision = choose()
            if decision is _RAISE:
                raise ExpectedCallbackError("float callback failed")
            return cast(float, decision)

        value_type = type("FloatValue" + "X" * 100, (), {"__float__": to_float})
        value: Any = value_type()
        if name == "sin":
            return math.sin(value)
        return math.fsum((value,))

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, cast(Any, object()), 1.25, 1.25),
        f"math-{name}-long-float-type-name",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[3]


for _name in ("sin", "fsum"):
    _case(
        f"math_{_name}_long_float_type_name_multishot",
        requires_math=_name,
    )(lambda name=_name: _math_long_float_type_name_case(name))


def _math_rounding_fallback_case(name: str, protocol: str) -> None:
    class FloatValue:
        def __init__(self, choose: Choose) -> None:
            self.choose = choose

        def __float__(self) -> float:
            decision = self.choose()
            if decision is _RAISE:
                raise ExpectedCallbackError("rounding float callback failed")
            if decision is _INVALID:
                return cast(Any, object())
            return cast(float, decision)

    class IndexValue:
        def __init__(self, choose: Choose) -> None:
            self.choose = choose

        def __index__(self) -> int:
            decision = self.choose()
            if decision is _RAISE:
                raise ExpectedCallbackError("rounding index callback failed")
            if decision is _INVALID:
                return cast(Any, "not-an-index")
            return cast(int, decision)

    def call(callback: Choose) -> Any:
        value = FloatValue(callback) if protocol == "float" else IndexValue(callback)
        return getattr(math, name)(value)

    valid = (1.25, 2.5) if protocol == "float" else (1, 2)
    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, *valid, valid[0]),
        f"math-{name}-{protocol}-fallback",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _name in ("ceil", "floor"):
    for _protocol in ("float", "index"):
        _case(
            f"math_{_name}_{_protocol}_fallback_multishot",
            requires_math=_name,
        )(
            lambda name=_name, protocol=_protocol: _math_rounding_fallback_case(
                name,
                protocol,
            )
        )


def _numeric_later_argument_case(name: str) -> None:
    protocol = "float" if name == "hypot" else "index"

    def call(choose: Choose) -> Any:
        value = _EffectfulNumber(choose, protocol)
        if name == "hypot":
            return math.hypot(3.0, value)
        if name == "gcd":
            return math.gcd(12, value)
        return math.lcm(12, value)

    valid = (4.0, 5.0) if protocol == "float" else (8, 6)
    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, *valid, valid[0]),
        f"math-{name}-later-argument",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _name in ("hypot", "gcd", "lcm"):
    _case(f"math_{_name}_later_argument_multishot", requires_math=_name)(
        lambda name=_name: _numeric_later_argument_case(name)
    )


def _math_fixed_later_argument_case(name: str, position: int) -> None:
    protocols = {
        "atan2": "float",
        "comb": "index",
        "copysign": "float",
        "fma": "float",
        "fmod": "float",
        "isclose": "float",
        "log": "float",
        "nextafter": "float",
        "perm": "index",
        "pow": "float",
        "remainder": "float",
    }
    arguments: dict[str, list[Any]] = {
        "atan2": [1.0, 2.0],
        "comb": [6, 2],
        "copysign": [1.0, -2.0],
        "fma": [2.0, 3.0, 4.0],
        "fmod": [5.0, 2.0],
        "isclose": [1.0, 1.0],
        "log": [8.0, 2.0],
        "nextafter": [1.0, 2.0],
        "perm": [6, 2],
        "pow": [2.0, 3.0],
        "remainder": [5.0, 2.0],
    }
    protocol = protocols[name]

    def call(choose: Choose) -> Any:
        values = arguments[name].copy()
        values[position] = _EffectfulNumber(choose, protocol)
        return getattr(math, name)(*values)

    valid = (2, 3) if protocol == "index" else (2.0, 3.0)
    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, *valid, valid[0]),
        f"math-{name}-argument-{position + 1}",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


_MATH_FIXED_LATER_ARGUMENTS = (
    ("atan2", 1),
    ("comb", 1),
    ("copysign", 1),
    ("fma", 1),
    ("fma", 2),
    ("fmod", 1),
    ("isclose", 1),
    ("log", 1),
    ("nextafter", 1),
    ("perm", 1),
    ("pow", 1),
    ("remainder", 1),
)

for _name, _position in _MATH_FIXED_LATER_ARGUMENTS:
    _case(
        f"math_{_name}_argument_{_position + 1}_callback_multishot",
        requires_math=_name,
    )(
        lambda name=_name, position=_position: _math_fixed_later_argument_case(
            name,
            position,
        )
    )


def _cmath_later_argument_case(name: str) -> None:
    def call(choose: Choose) -> Any:
        if name == "rect":
            value: Any = _EffectfulNumber(choose, "float")
            return cmath.rect(1.0, value)
        value = _EffectfulComplex(choose)
        if name == "isclose":
            return cmath.isclose(1.0 + 0.5j, value)
        return cmath.log(8.0 + 0.0j, value)

    valid: tuple[Any, Any]
    if name == "rect":
        valid = (0.5, 1.0)
    elif name == "log":
        valid = (2.0 + 0.0j, 3.0 + 0.0j)
    else:
        valid = (1.0 + 0.5j, 1.0 + 0.25j)
    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, *valid, valid[0]),
        f"cmath-{name}-argument-2",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _name in ("isclose", "log", "rect"):
    _case(f"cmath_{_name}_argument_2_callback_multishot")(lambda name=_name: _cmath_later_argument_case(name))


@_case("math_prod_start_callback_multishot", requires_math="prod")
def _math_prod_start_callback_multishot() -> None:
    def call(choose: Choose) -> Any:
        class Start:
            def __mul__(self, other: object) -> Any:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("math.prod start callback failed")
                if decision is _INVALID:
                    return NotImplemented
                return decision

        return math.prod((2,), start=Start())

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, 3, 5, 3),
        "math-prod-start",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == outcomes[4] == ("return", 3)


def _math_iter_acquisition_case(name: str, position: int) -> None:
    class Iterable:
        def __init__(self, choose: Choose) -> None:
            self.choose = choose

        def __iter__(self) -> Iterator[float]:
            decision = self.choose()
            if decision is _RAISE:
                raise ExpectedCallbackError(f"math.{name} __iter__ failed")
            if decision is _INVALID:
                return cast(Any, object())
            if name == "dist":
                return iter((float(decision), 4.0))
            return iter((float(decision),))

    def call(choose: Choose) -> Any:
        source = Iterable(choose)
        if name == "fsum":
            return math.fsum(source)
        if name == "prod":
            return math.prod(source)
        if name == "dist":
            arguments: list[Any] = [source, (0.0, 0.0)]
            if position == 1:
                arguments.reverse()
            return math.dist(*arguments)
        arguments = [source, (2.0,)]
        if position == 1:
            arguments.reverse()
        return math.sumprod(*arguments)

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, 1.0, 3.0, 1.0),
        f"math-{name}-argument-{position + 1}-iter",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _name, _positions in (
    ("fsum", (0,)),
    ("prod", (0,)),
    ("dist", (0, 1)),
    ("sumprod", (0, 1)),
):
    for _position in _positions:
        _case(
            f"math_{_name}_argument_{_position + 1}_iter_callback_multishot",
            requires_math=_name,
        )(
            lambda name=_name, position=_position: _math_iter_acquisition_case(
                name,
                position,
            )
        )


def _numeric_second_iterator_case(name: str) -> None:
    iterators: list[_EffectfulIterator] = []

    def call(choose: Choose) -> Any:
        iterator = _EffectfulIterator(choose, (4.0,), "float")
        iterators[:] = [iterator]
        if name == "dist":
            return math.dist((0.0, 0.0), iterator)
        return math.sumprod((3.0, 4.0), iterator)

    def fresh(decision: Any) -> Any:
        iterator = _EffectfulIterator(lambda: decision, (4.0,), "float")
        if name == "dist":
            return math.dist((0.0, 0.0), iterator)
        return math.sumprod((3.0, 4.0), iterator)

    def reset() -> None:
        if iterators:
            iterators[0].position = -1

    outcomes = _resume_against_fresh(
        call,
        fresh,
        (_RAISE, _INVALID, 1.0, 2.0, 1.0),
        f"math-{name}-second-iterator",
        reset=reset,
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _name in ("dist", "sumprod"):
    _case(
        f"math_{_name}_second_iterator_multishot",
        requires_math=_name,
    )(lambda name=_name: _numeric_second_iterator_case(name))


def _numeric_iterator_index_fallback_case(name: str) -> None:
    def call(choose: Choose) -> Any:
        value = cast(Any, _EffectfulIndexNumber(choose))
        if name == "fsum":
            return math.fsum([value])
        return math.dist([value], [0.0])

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, 1, 2, 1),
        f"math-{name}-item-index-fallback",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _name in ("fsum", "dist"):
    _case(
        f"math_{_name}_item_index_fallback_multishot",
        requires_math=_name,
    )(lambda name=_name: _numeric_iterator_index_fallback_case(name))


def _numeric_iterator_stop_case(name: str) -> None:
    current: list[_EffectfulIterator] = []

    def call(choose: Choose) -> Any:
        iterator = _EffectfulIterator(choose, (), "float")
        current[:] = [iterator]
        if name == "dist":
            return math.dist(iterator, ())
        if name == "fsum":
            return math.fsum(iterator)
        if name == "prod":
            return math.prod(iterator)
        return math.sumprod(iterator, ())

    def fresh(decision: Any) -> Any:
        saved = current[:]
        try:
            return call(lambda: decision)
        finally:
            current[:] = saved

    def reset() -> None:
        if current:
            current[0].position = -1

    outcomes = _resume_against_fresh(
        call,
        fresh,
        (_STOP, _RAISE, 1.0, _STOP),
        f"math-{name}-next-stop",
        reset=reset,
    )
    assert outcomes[0][0] == "return"
    assert outcomes[1][0] == "raise"
    assert outcomes[0] == outcomes[3]


for _name in _MATH_ITERATOR_NAMES:
    _case(
        f"math_{_name}_next_stop_multishot",
        requires_math=_name,
    )(lambda name=_name: _numeric_iterator_stop_case(name))


@_case("math_sumprod_iterator_acquisition_order", requires_math="sumprod")
def _math_sumprod_iterator_acquisition_order() -> None:
    events: list[str] = []
    choose = effect("math-sumprod-iterator-order")
    handler = create_handler(choose)

    class Left:
        def __init__(self) -> None:
            self.done = False

        def __iter__(self) -> "Left":
            events.append("left.iter")
            return self

        def __next__(self) -> int:
            events.append("left.next")
            if self.done:
                raise StopIteration
            self.done = True
            return cast(int, choose())

    class Right:
        def __iter__(self) -> Iterator[int]:
            events.append("right.iter")
            return iter((2,))

    @handler.on(choose)
    def resume(k: Any) -> Any:
        assert events == ["left.iter", "right.iter", "left.next"]
        return k(3)

    assert handler(lambda: math.sumprod(Left(), Right())) == 6


def _buffer_decision(choose: Choose, callback_name: str) -> memoryview:
    decision = choose()
    if decision is _RAISE:
        raise ExpectedCallbackError(f"{callback_name} callback failed")
    if decision is _INVALID:
        return cast(Any, object())
    return memoryview(cast(bytes | bytearray, decision))


def _struct_unpack_buffer_case(api: str) -> None:
    def call(choose: Choose) -> Any:
        class Buffer:
            def __buffer__(self, _flags: int) -> memoryview:
                return _buffer_decision(choose, f"{api} __buffer__")

            def __release_buffer__(self, _view: memoryview) -> None:
                pass

        source = cast(Any, Buffer())
        descriptor = struct.Struct("i")
        if api == "unpack":
            return struct.unpack("i", source)
        if api == "unpack_from":
            return struct.unpack_from("i", source, 0)
        if api == "iter_unpack":
            return list(struct.iter_unpack("i", source))
        if api == "Struct.unpack":
            return descriptor.unpack(source)
        if api == "Struct.unpack_from":
            return descriptor.unpack_from(source, 0)
        return list(descriptor.iter_unpack(source))

    first = struct.pack("i", 11)
    second = struct.pack("i", 13)
    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, first, second, first),
        f"struct-{api}-buffer",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _api in (
    "unpack",
    "unpack_from",
    "iter_unpack",
    "Struct.unpack",
    "Struct.unpack_from",
    "Struct.iter_unpack",
):
    _case(f"struct_{_api.replace('.', '_')}_buffer_callback_multishot")(
        lambda api=_api: _struct_unpack_buffer_case(api)
    )


def _struct_pack_callback_case(api: str, kind: str) -> None:
    current: list[bytearray] = []

    def call(choose: Choose) -> Any:
        class Value:
            def __float__(self) -> float:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("struct float callback failed")
                if decision is _INVALID:
                    return cast(Any, object())
                return cast(float, decision)

            def __bool__(self) -> bool:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("struct bool callback failed")
                if decision is _INVALID:
                    return cast(Any, object())
                return cast(bool, decision)

            def __complex__(self) -> complex:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("struct complex callback failed")
                if decision is _INVALID:
                    return cast(Any, object())
                return cast(complex, decision)

        formats = {"float": "d", "bool": "?", "complex_F": "F", "complex_D": "D"}
        format_string = formats[kind]
        value = cast(Any, Value())
        descriptor = struct.Struct(format_string)
        if api == "pack":
            return struct.pack(format_string, value)
        if api == "Struct.pack":
            return descriptor.pack(value)
        target = bytearray(descriptor.size)
        current[:] = [target]
        if api == "pack_into":
            struct.pack_into(format_string, target, 0, value)
        else:
            descriptor.pack_into(target, 0, value)
        return bytes(target)

    def reset() -> None:
        if current:
            current[0][:] = b"\0" * len(current[0])

    def fresh(decision: Any) -> Any:
        saved = current[:]
        try:
            return call(lambda: decision)
        finally:
            current[:] = saved

    if kind == "float":
        decisions = (_RAISE, _INVALID, 1.25, -2.5, 1.25)
    elif kind == "bool":
        decisions = (_RAISE, _INVALID, False, True, False)
    else:
        decisions = (_RAISE, _INVALID, 1.25 + 2.5j, -2.5 + 1.25j, 1.25 + 2.5j)
    outcomes = _resume_against_fresh(
        call,
        fresh,
        decisions,
        f"struct-{api}-{kind}",
        reset=None if api in ("pack", "Struct.pack") else reset,
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _api in ("pack", "pack_into", "Struct.pack", "Struct.pack_into"):
    _kinds = ["float", "bool"]
    if sys.version_info >= (3, 14):
        _kinds.extend(("complex_F", "complex_D"))
    for _kind in _kinds:
        _case(f"struct_{_api.replace('.', '_')}_{_kind}_callback_multishot")(
            lambda api=_api, kind=_kind: _struct_pack_callback_case(api, kind)
        )


def _struct_pack_fallback_case(
    api: str,
    format_string: str,
    protocol: str,
) -> None:
    current: list[bytearray] = []

    class LengthValue:
        def __init__(self, choose: Choose) -> None:
            self.choose = choose

        def __len__(self) -> int:
            decision = self.choose()
            if decision is _RAISE:
                raise ExpectedCallbackError("struct length callback failed")
            if decision is _INVALID:
                return cast(Any, "not-a-length")
            return cast(int, decision)

    def call(choose: Choose) -> Any:
        if protocol == "len":
            value: Any = LengthValue(choose)
        elif protocol == "float":
            value = _EffectfulNumber(choose, "float")
        else:
            value = _EffectfulIndexNumber(choose)
        descriptor = struct.Struct(format_string)
        if api == "pack":
            return struct.pack(format_string, value)
        if api == "Struct.pack":
            return descriptor.pack(value)
        target = bytearray(descriptor.size)
        current[:] = [target]
        if api == "pack_into":
            struct.pack_into(format_string, target, 0, value)
        else:
            descriptor.pack_into(target, 0, value)
        return bytes(target)

    def reset() -> None:
        if current:
            current[0][:] = b"\0" * len(current[0])

    def fresh(decision: Any) -> Any:
        saved = current[:]
        try:
            return call(lambda: decision)
        finally:
            current[:] = saved

    if protocol == "len":
        decisions = (_RAISE, _INVALID, -1, 0, 2, 0)
    elif protocol == "float":
        decisions = (_RAISE, _INVALID, 1.25, -2.5, 1.25)
    else:
        decisions = (_RAISE, _INVALID, 1, -2, 1)
    outcomes = _resume_against_fresh(
        call,
        fresh,
        decisions,
        f"struct-{api}-{format_string}-{protocol}-fallback",
        reset=None if api in ("pack", "Struct.pack") else reset,
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    if protocol == "len":
        assert outcomes[2][0] == "raise"
        assert outcomes[3][0] == "return"
        assert outcomes[3] == outcomes[5]
    else:
        assert outcomes[2][0] == "return"
        assert outcomes[2] == outcomes[4]


for _api in ("pack", "pack_into", "Struct.pack", "Struct.pack_into"):
    _case(f"struct_{_api.replace('.', '_')}_bool_len_fallback_multishot")(
        lambda api=_api: _struct_pack_fallback_case(api, "?", "len")
    )
    for _format in ("f", "d"):
        _case(f"struct_{_api.replace('.', '_')}_{_format}_index_fallback_multishot")(
            lambda api=_api, format_string=_format: _struct_pack_fallback_case(
                api,
                format_string,
                "index",
            )
        )
    if sys.version_info >= (3, 14):
        for _format in ("F", "D"):
            for _protocol in ("float", "index"):
                _case(f"struct_{_api.replace('.', '_')}_{_format}_{_protocol}_fallback_multishot")(
                    lambda api=_api, format_string=_format, protocol=_protocol: _struct_pack_fallback_case(
                        api, format_string, protocol
                    )
                )


def _struct_pack_into_destination_case(api: str) -> None:
    current: list[bytearray] = []

    def call(choose: Choose) -> Any:
        backing = bytearray(8)
        current[:] = [backing]

        class Destination:
            def __buffer__(self, _flags: int) -> memoryview:
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("pack_into destination callback failed")
                if decision is _INVALID:
                    return cast(Any, object())
                return memoryview(backing)

            def __release_buffer__(self, _view: memoryview) -> None:
                pass

        destination = cast(Any, Destination())
        if api == "pack_into":
            struct.pack_into("i", destination, 0, 7)
        else:
            struct.Struct("i").pack_into(destination, 0, 7)
        return bytes(backing)

    def reset() -> None:
        if current:
            current[0][:] = b"\0" * len(current[0])

    def fresh(decision: Any) -> Any:
        saved = current[:]
        try:
            return call(lambda: decision)
        finally:
            current[:] = saved

    outcomes = _resume_against_fresh(
        call,
        fresh,
        (_RAISE, _INVALID, 0, 1, 0),
        f"struct-{api}-destination-buffer",
        reset=reset,
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


def _struct_pack_into_offset_case(api: str) -> None:
    current: list[bytearray] = []

    def call(choose: Choose) -> Any:
        target = bytearray(8)
        current[:] = [target]
        offset = cast(Any, _EffectfulNumber(choose, "index"))
        if api == "pack_into":
            struct.pack_into("i", target, offset, 7)
        else:
            struct.Struct("i").pack_into(target, offset, 7)
        return bytes(target)

    def reset() -> None:
        if current:
            current[0][:] = b"\0" * len(current[0])

    def fresh(decision: Any) -> Any:
        saved = current[:]
        try:
            return call(lambda: decision)
        finally:
            current[:] = saved

    outcomes = _resume_against_fresh(
        call,
        fresh,
        (_RAISE, _INVALID, 0, 4, 0),
        f"struct-{api}-offset",
        reset=reset,
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


def _struct_unpack_from_offset_case(api: str) -> None:
    payload = b"x" + struct.pack("i", 11) + b"y"

    def call(choose: Choose) -> Any:
        offset = cast(Any, _EffectfulNumber(choose, "index"))
        if api == "unpack_from":
            return struct.unpack_from("i", payload, offset)
        return struct.Struct("i").unpack_from(payload, offset)

    outcomes = _resume_against_fresh(
        call,
        lambda decision: call(lambda: decision),
        (_RAISE, _INVALID, 1, -5, 1),
        f"struct-{api}-offset",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2][0] == "return"
    assert outcomes[2] == outcomes[4]


for _api in ("pack_into", "Struct.pack_into"):
    _case(f"struct_{_api.replace('.', '_')}_destination_buffer_multishot")(
        lambda api=_api: _struct_pack_into_destination_case(api)
    )
    _case(f"struct_{_api.replace('.', '_')}_offset_index_multishot")(
        lambda api=_api: _struct_pack_into_offset_case(api)
    )

for _api in ("unpack_from", "Struct.unpack_from"):
    _case(f"struct_{_api.replace('.', '_')}_offset_index_multishot")(
        lambda api=_api: _struct_unpack_from_offset_case(api)
    )


def _assert_struct_callback_not_called(
    name: str,
    call: Callable[[Choose], Any],
    expected_errors: tuple[str, ...] = ("error",),
) -> None:
    choose = effect(name)
    handler = create_handler(choose)
    callback_count = 0

    @handler.on(choose)
    def resume(_continuation: Any) -> Any:
        nonlocal callback_count
        callback_count += 1
        return 1

    outcome = _outcome(lambda: handler(lambda: call(choose)))
    assert outcome[0] == "raise"
    assert outcome[1][0] in expected_errors
    assert callback_count == 0


@_case("struct_bytearray_and_whitespace_preserve_following_callback")
def _struct_bytearray_and_whitespace_preserve_following_callback() -> None:
    for format_string in ("1s i", "1p\v\fi"):

        def call(choose: Choose, format_string: str = format_string) -> Any:
            return struct.pack(
                format_string,
                bytearray(b"x"),
                cast(Any, _EffectfulIndexNumber(choose)),
            )

        outcomes = _resume_against_fresh(
            call,
            lambda decision, call=call: call(lambda: decision),
            (_RAISE, _INVALID, 1, 2, 1),
            f"struct-bytearray-whitespace-{format_string!r}",
        )
        assert outcomes[0][0] == "raise"
        assert outcomes[1][0] == "raise"
        assert outcomes[2][0] == "return"
        assert outcomes[2] == outcomes[4]


@_case("struct_field_validation_precedes_following_callback")
def _struct_field_validation_precedes_following_callback() -> None:
    _assert_struct_callback_not_called(
        "struct-field-validation-order",
        lambda choose: struct.pack(
            "bi",
            128,
            cast(Any, _EffectfulIndexNumber(choose)),
        ),
    )


@_case("struct_pack_into_bounds_precede_value_callback")
def _struct_pack_into_bounds_precede_value_callback() -> None:
    for offset, expected_errors in (
        (0, ("error",)),
        (sys.maxsize + 1, ("IndexError",)),
    ):
        _assert_struct_callback_not_called(
            f"struct-pack-into-bounds-{offset}",
            lambda choose, offset=offset: struct.pack_into(
                "i",
                bytearray(1),
                offset,
                cast(Any, _EffectfulIndexNumber(choose)),
            ),
            expected_errors,
        )


@_case("struct_iter_unpack_zero_size_precedes_buffer_callback")
def _struct_iter_unpack_zero_size_precedes_buffer_callback() -> None:
    class Buffer:
        def __init__(self, choose: Choose) -> None:
            self.choose = choose

        def __buffer__(self, _flags: int) -> memoryview:
            self.choose()
            return memoryview(b"")

        def __release_buffer__(self, _view: memoryview) -> None:
            pass

    _assert_struct_callback_not_called(
        "struct-iter-unpack-zero-size",
        lambda choose: list(struct.iter_unpack("0s", cast(Any, Buffer(choose)))),
    )


@_case("struct_buffer_release_contract_multishot")
def _struct_buffer_release_contract_multishot() -> None:
    current: list[Any] = []

    def call(choose: Choose) -> Any:
        class Buffer:
            def __init__(self) -> None:
                self.release_count = 0

            def __buffer__(self, _flags: int) -> memoryview:
                return _buffer_decision(choose, "struct release __buffer__")

            def __release_buffer__(self, _view: memoryview) -> None:
                self.release_count += 1

        owner = Buffer()
        current[:] = [owner]
        result = struct.unpack("i", cast(Any, owner))
        return result, owner.release_count

    def fresh(decision: Any) -> Any:
        saved = current[:]
        try:
            return call(lambda: decision)
        finally:
            current[:] = saved

    def reset() -> None:
        if current:
            current[0].release_count = 0

    packed = struct.pack("i", 11)
    outcomes = _resume_against_fresh(
        call,
        fresh,
        (_RAISE, _INVALID, packed, packed),
        "struct-buffer-release-contract",
        reset=reset,
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == ("return", ((11,), 1))
    assert outcomes[2] == outcomes[3]


def _struct_effectful_release_buffer_case(api: str) -> None:
    current: list[Any] = []

    def call(choose: Choose) -> Any:
        class Buffer:
            def __init__(self, backing: bytearray) -> None:
                self.backing = backing
                self.release_count = 0

            def __buffer__(self, _flags: int) -> memoryview:
                return memoryview(self.backing)

            def __release_buffer__(self, _view: memoryview) -> None:
                self.release_count += 1
                decision = choose()
                if decision is _RAISE:
                    raise ExpectedCallbackError("struct release callback failed")

        backing = bytearray(4)
        backing[:4] = struct.pack("i", 11)
        owner = Buffer(backing)
        current[:] = [owner]
        source = cast(Any, owner)
        descriptor = struct.Struct("i")
        if api == "unpack":
            result: Any = struct.unpack("i", source)
        elif api == "unpack_from":
            result = struct.unpack_from("i", source, 0)
        elif api == "iter_unpack":
            result = list(struct.iter_unpack("i", source))
        elif api == "Struct.unpack":
            result = descriptor.unpack(source)
        elif api == "Struct.unpack_from":
            result = descriptor.unpack_from(source, 0)
        elif api == "Struct.iter_unpack":
            result = list(descriptor.iter_unpack(source))
        elif api == "pack_into":
            result = struct.pack_into("i", source, 0, 13)
        else:
            result = descriptor.pack_into(source, 0, 13)
        return result, bytes(backing), owner.release_count

    choose = effect(f"struct-{api}-release-buffer")
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        suspended_owner = current[0]
        assert suspended_owner.release_count == 1
        outcomes: list[Outcome] = []
        for decision in (_RAISE, None, None):
            suspended_owner.release_count = 1
            current[:] = [suspended_owner]
            actual = _outcome(lambda decision=decision: k(decision))
            expected = _outcome(lambda decision=decision: call(lambda: decision))
            current[:] = [suspended_owner]
            assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            if actual[0] == "return":
                assert actual[1][2] == 1
            outcomes.append(actual)
        return outcomes

    outcomes = cast(list[Outcome], handler(lambda: call(choose)))
    assert suspension_count == 1
    assert outcomes[1] == outcomes[2]
    assert outcomes[1][0] == "return"
    assert outcomes[1][1][2] == 1


for _api in (
    "unpack",
    "unpack_from",
    "Struct.unpack",
    "Struct.unpack_from",
    "pack_into",
    "Struct.pack_into",
):
    _case(f"struct_{_api.replace('.', '_')}_release_buffer_multishot")(
        lambda api=_api: _struct_effectful_release_buffer_case(api)
    )


@_case("struct_subclass_format_override_is_not_called")
def _struct_subclass_format_override_is_not_called() -> None:
    callback_count = 0
    choose = effect("struct-subclass-format-override")
    handler = create_handler(choose)

    class Descriptor(struct.Struct):
        def __getattribute__(self, name: str) -> Any:
            if name == "format":
                choose()
            return super().__getattribute__(name)

    @handler.on(choose)
    def resume(_continuation: Any) -> None:
        nonlocal callback_count
        callback_count += 1

    assert handler(lambda: Descriptor("i").pack(7)) == struct.pack("i", 7)
    assert callback_count == 0


@_case("struct_unpack_apis_normal_error_corner")
def _struct_unpack_apis_normal_error_corner() -> None:
    assert struct.pack("0i") == b""
    assert struct.pack("4s", b"a") == b"a\0\0\0"
    assert struct.unpack("2i", struct.pack("2i", 11, 13)) == (11, 13)

    protocol_calls = 0

    class IndexValue:
        def __index__(self) -> int:
            nonlocal protocol_calls
            protocol_calls += 1
            return 1

    with pytest.raises(struct.error):
        struct.pack("i<i", IndexValue(), IndexValue())
    assert protocol_calls == 0

    payload = struct.pack("ii", 11, 13)
    assert struct.unpack("ii", payload) == (11, 13)
    assert struct.unpack_from("ii", b"x" + payload, 1) == (11, 13)
    assert list(struct.iter_unpack("ii", payload * 2)) == [(11, 13), (11, 13)]
    descriptor = struct.Struct("ii")
    assert descriptor.unpack(payload) == (11, 13)
    assert descriptor.unpack_from(b"x" + payload, 1) == (11, 13)
    assert list(descriptor.iter_unpack(payload * 2)) == [(11, 13), (11, 13)]
    for operation in (
        lambda: struct.unpack("ii", b"short"),
        lambda: struct.unpack_from("ii", b"short", 0),
        lambda: list(struct.iter_unpack("ii", b"short")),
        lambda: descriptor.unpack(b"short"),
        lambda: descriptor.unpack_from(b"short", 0),
        lambda: list(descriptor.iter_unpack(b"short")),
    ):
        with pytest.raises((struct.error, ValueError)):
            operation()


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=15,
        check=False,
    )


def _require_available_math(case_name: str) -> None:
    math_name = _MATH_REQUIREMENTS.get(case_name)
    if math_name is not None and not hasattr(math, math_name):
        pytest.skip(f"math.{math_name} is unavailable on this CPython version")


@pytest.mark.parametrize("case_name", tuple(_CASES))
def test_numeric_struct_continuation(case_name: str) -> None:
    _require_available_math(case_name)
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


def test_math_case_requirements_are_explicit() -> None:
    math_cases = {name for name in _CASES if name.startswith("math_")}
    assert set(_MATH_REQUIREMENTS) == math_cases
    assert {
        _MATH_REQUIREMENTS[name]
        for name in math_cases
        if "_special_result_passthrough_" in name or "_item_callback_" in name
    } == {"ceil", "floor", "trunc", "fsum", "dist", "prod", "sumprod"}


def test_missing_math_requirement_is_skipped(monkeypatch: pytest.MonkeyPatch) -> None:
    case_name = "math_acos_numeric_conversion_multishot"
    monkeypatch.delattr(math, "acos")
    with pytest.raises(pytest.skip.Exception, match=r"math\.acos is unavailable"):
        _require_available_math(case_name)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_numeric_struct.py --case CASE")
    _CASES[sys.argv[2]]()
