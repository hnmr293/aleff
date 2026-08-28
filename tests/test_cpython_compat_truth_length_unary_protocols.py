"""CPython differential and continuation tests for core object protocols."""

from __future__ import annotations

from collections.abc import Callable
import operator
from textwrap import dedent
from typing import Any, cast

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


Choose = Callable[[], Any]


def _source(source: str) -> str:
    return dedent(source).strip() + "\n"


def _resume_outcomes(run: Callable[[Choose], Any], values: tuple[Any, ...]) -> list[tuple[str, Any]]:
    choice = effect("compatibility-choice")
    handler = create_handler(choice)

    @handler.on(choice)
    def resume(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in values:
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    return cast(list[tuple[str, Any]], handler(lambda: run(choice)))


def _returns(*values: Any) -> list[tuple[str, Any]]:
    return [("return", value) for value in values]


def test_truth_len_and_unary_normal_behavior_matches_cpython() -> None:
    assert_cpython_compatible(
        _source(
            """
            import operator

            class Truth:
                def __bool__(self):
                    return True

                def __len__(self):
                    return 1

            class Empty:
                def __len__(self):
                    return 0

            class Number:
                def __abs__(self):
                    return 12

                def __neg__(self):
                    return -12

                def __pos__(self):
                    return 12

                def __invert__(self):
                    return -13

            class Index:
                def __index__(self):
                    return 7

            print(bool(Truth()), bool(Empty()))
            print(not Truth())
            print(len(Truth()), len(Empty()))
            number = Number()
            print(abs(number), -number, +number, ~number)
            print(operator.abs(number), operator.neg(number), operator.pos(number))
            print(operator.invert(number), operator.inv(number))
            index = Index()
            print(operator.index(index), int(index), float(index), complex(index))
            print(bin(index), oct(index), hex(index))
            """
        )
    )


def test_truth_len_and_unary_errors_match_cpython() -> None:
    assert_cpython_compatible(
        _source(
            """
            import operator

            def outcome(call):
                try:
                    value = call()
                except Exception as exc:
                    return (type(exc).__name__, str(exc))
                return (type(value).__name__, value)

            class BadBool:
                def __bool__(self):
                    return 1

            class BadLen:
                def __len__(self):
                    return -1

            class HugeLen:
                def __len__(self):
                    return 1 << 100

            class BadFloat:
                def __float__(self):
                    return 1

            class BadIndex:
                def __index__(self):
                    return object()

            class BadComplex:
                def __complex__(self):
                    return 1

            class BadAbs:
                pass

            print(outcome(lambda: bool(BadBool())))
            print(outcome(lambda: len(BadLen())))
            print(outcome(lambda: len(HugeLen())))
            print(outcome(lambda: float(BadFloat())))
            print(outcome(lambda: operator.index(BadIndex())))
            print(outcome(lambda: int(BadIndex())))
            print(outcome(lambda: bin(BadIndex())))
            print(outcome(lambda: complex(BadComplex())))
            print(outcome(lambda: abs(BadAbs())))
            print(outcome(lambda: operator.neg(BadAbs())))
            """
        )
    )


def test_length_hint_fallbacks_match_cpython() -> None:
    assert_cpython_compatible(
        _source(
            """
            import operator

            class NotImplementedHint:
                def __length_hint__(self):
                    return NotImplemented

            class TypeErrorHint:
                def __length_hint__(self):
                    raise TypeError("try the default")

            class LenTypeErrorHint:
                def __len__(self):
                    raise TypeError("no length")

                def __length_hint__(self):
                    return 23

            class NegativeHint:
                def __length_hint__(self):
                    return -1

            def outcome(call):
                try:
                    return ("return", call())
                except Exception as exc:
                    return (type(exc).__name__, str(exc))

            print(outcome(lambda: operator.length_hint(NotImplementedHint(), 99)))
            print(outcome(lambda: operator.length_hint(TypeErrorHint(), 99)))
            print(outcome(lambda: operator.length_hint(LenTypeErrorHint(), 99)))
            print(outcome(lambda: operator.length_hint(NegativeHint(), 99)))
            """
        )
    )


def test_special_protocol_lookup_does_not_run_metaclass_attribute_hooks() -> None:
    assert_cpython_compatible(
        _source(
            """
            import operator

            events = []

            class Meta(type):
                def __getattribute__(cls, name):
                    events.append(name)
                    return super().__getattribute__(name)

            class Target(metaclass=Meta):
                pass

            def outcome(call):
                try:
                    return ("return", call())
                except Exception as exc:
                    return (type(exc).__name__, str(exc))

            print(outcome(lambda: bool(Target())))
            print(events)
            events.clear()
            print(outcome(lambda: int(Target())))
            print(events)
            events.clear()
            print(outcome(lambda: operator.index(Target())))
            print(events)
            """
        )
    )


def test_truth_context_resumes_through_not() -> None:
    def run(choose: Choose) -> bool:
        class Target:
            def __bool__(self) -> bool:
                return cast(bool, choose())

        return not Target()

    assert _resume_outcomes(run, (True, False)) == _returns(False, True)


def test_truth_context_resumes_through_if() -> None:
    def run(choose: Choose) -> str:
        class Target:
            def __bool__(self) -> bool:
                return cast(bool, choose())

        if Target():
            return "true"
        return "false"

    assert _resume_outcomes(run, (True, False)) == _returns("true", "false")


def test_truth_context_resumes_through_while() -> None:
    def run(choose: Choose) -> str:
        class Target:
            def __init__(self) -> None:
                self.calls = 0

            def __bool__(self) -> bool:
                self.calls += 1
                return cast(bool, choose()) if self.calls == 1 else False

        target = Target()
        while target:
            return "body"
        return "empty"

    assert _resume_outcomes(run, (True, False)) == _returns("body", "empty")


def test_operator_truth_and_not_reject_invalid_resumed_bool() -> None:
    def truth_run(choose: Choose) -> bool:
        class Target:
            def __bool__(self) -> Any:
                return choose()

        return operator.truth(Target())

    def not_run(choose: Choose) -> bool:
        class Target:
            def __bool__(self) -> Any:
                return choose()

        return operator.not_(Target())

    expected = [("raise", "TypeError"), ("return", True)]
    assert _resume_outcomes(truth_run, (1, True)) == expected
    assert _resume_outcomes(not_run, (1, True)) == [
        ("raise", "TypeError"),
        ("return", False),
    ]


def test_len_resumes_and_preserves_negative_result_errors() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __len__(self) -> int:
                return cast(int, choose())

        return len(Target())

    assert _resume_outcomes(run, (0, 7)) == _returns(0, 7)
    assert _resume_outcomes(run, (-1, 8)) == [
        ("raise", "ValueError"),
        ("return", 8),
    ]


def test_length_hint_resumed_not_implemented_uses_default() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __length_hint__(self) -> Any:
                return choose()

        return operator.length_hint(Target(), 99)

    assert _resume_outcomes(run, (NotImplemented, 7)) == _returns(99, 7)


def _index_resume_outcomes(operation: Callable[[Any], Any]) -> list[tuple[str, Any]]:
    class Returned:
        def __index__(self) -> int:
            return 7

    def run(choose: Choose) -> Any:
        class Target:
            def __index__(self) -> Any:
                return choose()

        return operation(Target())

    return _resume_outcomes(run, (Returned(), 7))


def test_index_resume_validates_callback_result_once() -> None:
    operations: tuple[tuple[str, Callable[[Any], Any], Any], ...] = (
        ("operator.index", operator.index, 7),
        ("int", int, 7),
        ("float", float, 7.0),
        ("complex", complex, 7 + 0j),
        ("bin", bin, "0b111"),
        ("oct", oct, "0o7"),
        ("hex", hex, "0x7"),
    )

    for name, operation, valid_result in operations:
        outcomes = _index_resume_outcomes(operation)
        assert outcomes == [
            ("raise", "TypeError"),
            ("return", valid_result),
        ], name


def test_complex_positional_two_argument_resume_keeps_real_and_imaginary_parts() -> None:
    def run(choose: Choose) -> complex:
        class Real:
            def __float__(self) -> float:
                return 1.5

        class Imaginary:
            def __float__(self) -> float:
                return cast(float, choose())

        return complex(Real(), Imaginary())

    assert _resume_outcomes(run, (2.5, 3.5)) == _returns(1.5 + 2.5j, 1.5 + 3.5j)


def test_complex_keyword_two_argument_resume_keeps_real_and_imaginary_parts() -> None:
    def run(choose: Choose) -> complex:
        class Real:
            def __float__(self) -> float:
                return 1.5

        class Imaginary:
            def __float__(self) -> float:
                return cast(float, choose())

        return complex(real=Real(), imag=Imaginary())

    assert _resume_outcomes(run, (2.5, 3.5)) == _returns(1.5 + 2.5j, 1.5 + 3.5j)
