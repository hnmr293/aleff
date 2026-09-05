"""Strict continuation tests for the remaining CPython built-in functions."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import subprocess
import sys
from typing import Any, cast

import pytest

from aleff import create_handler, effect


pytestmark = pytest.mark.publish_wheel


Case = Callable[[], None]
Choose = Callable[[], Any]
_CASES: dict[str, Case] = {}


def _case(name: str) -> Callable[[Case], Case]:
    def register(test_case: Case) -> Case:
        _CASES[name] = test_case
        return test_case

    return register


def _resume_outcomes(run: Callable[[Choose], Any], values: tuple[Any, ...] = (1, 10)) -> list[tuple[str, Any]]:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in values:
            try:
                result = k(value)
                outcomes.append(("return", result))
            except Exception as exc:
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    return cast(list[tuple[str, Any]], handler(lambda: run(choose)))


def _returns(*values: Any) -> list[tuple[str, Any]]:
    return [("return", value) for value in values]


@_case("iter_object_postprocessing")
def _iter_object_postprocessing() -> None:
    def run(choose: Choose) -> list[int]:
        class Iterable:
            def __iter__(self) -> Any:
                return iter((cast(int, choose()), 100))

        return list(iter(Iterable()))

    assert _resume_outcomes(run) == _returns([1, 100], [10, 100])


@_case("iter_invalid_result_isolated")
def _iter_invalid_result_isolated() -> None:
    def run(choose: Choose) -> list[int]:
        class Iterable:
            def __iter__(self) -> Any:
                value = choose()
                return value if value == "invalid" else iter((cast(int, value),))

        return list(iter(Iterable()))

    assert _resume_outcomes(run, ("invalid", 10)) == [
        ("raise", "TypeError"),
        ("return", [10]),
    ]


@_case("iter_callable_sentinel_continues")
def _iter_callable_sentinel_continues() -> None:
    def run(choose: Choose) -> list[int]:
        class CallableValue:
            calls = 0

            def __call__(self) -> int:
                self.calls += 1
                return cast(int, choose()) if self.calls == 1 else 100

        return list(iter(CallableValue(), 100))

    assert _resume_outcomes(run) == _returns([1], [10])


@_case("aiter_validates_and_returns_iterator")
def _aiter_validates_and_returns_iterator() -> None:
    def run(choose: Choose) -> str:
        class AsyncIterator:
            def __aiter__(self) -> AsyncIterator:
                return self

            async def __anext__(self) -> int:
                raise StopAsyncIteration

        class AsyncIterable:
            def __aiter__(self) -> Any:
                choose()
                return AsyncIterator()

        return type(aiter(AsyncIterable())).__name__

    assert _resume_outcomes(run) == _returns("AsyncIterator", "AsyncIterator")


@_case("aiter_invalid_result_isolated")
def _aiter_invalid_result_isolated() -> None:
    def run(choose: Choose) -> str:
        class AsyncIterator:
            def __aiter__(self) -> AsyncIterator:
                return self

            async def __anext__(self) -> int:
                raise StopAsyncIteration

        class AsyncIterable:
            def __aiter__(self) -> Any:
                value = choose()
                return value if value == "invalid" else AsyncIterator()

        return type(aiter(AsyncIterable())).__name__

    assert _resume_outcomes(run, ("invalid", 10)) == [
        ("raise", "TypeError"),
        ("return", "AsyncIterator"),
    ]


@_case("dir_sorts_callback_result")
def _dir_sorts_callback_result() -> None:
    def run(choose: Choose) -> list[str]:
        class Target:
            def __dir__(self) -> list[str]:
                return cast(list[str], choose())

        return dir(Target())

    assert _resume_outcomes(run, (["b", "a"], ["d", "c"])) == _returns(["a", "b"], ["c", "d"])


@_case("dir_invalid_result_isolated")
def _dir_invalid_result_isolated() -> None:
    def run(choose: Choose) -> list[str]:
        class Target:
            def __dir__(self) -> Any:
                return choose()

        return dir(Target())

    assert _resume_outcomes(run, (1, ["b", "a"])) == [
        ("raise", "TypeError"),
        ("return", ["a", "b"]),
    ]


@_case("hash_normalizes_minus_one")
def _hash_normalizes_minus_one() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __hash__(self) -> int:
                return cast(int, choose())

        return hash(Target())

    assert _resume_outcomes(run, (-1, 10)) == _returns(-2, 10)


@_case("hash_invalid_result_isolated")
def _hash_invalid_result_isolated() -> None:
    def run(choose: Choose) -> int:
        class Target:
            def __hash__(self) -> Any:
                return choose()

        return hash(Target())

    assert _resume_outcomes(run, ("invalid", 10)) == [
        ("raise", "TypeError"),
        ("return", 10),
    ]


def _abstract_check_case(name: str) -> None:
    operation = isinstance if name == "isinstance" else issubclass
    hook_name = "__instancecheck__" if name == "isinstance" else "__subclasscheck__"

    def run(choose: Choose) -> bool:
        def hook(_self: Any, _candidate: Any) -> Any:
            return choose()

        checker = type("Checker", (type,), {hook_name: hook})("Checked", (), {})
        candidate: Any = object() if name == "isinstance" else type("Candidate", (), {})
        result = operation(candidate, checker)
        assert type(result) is bool
        return result

    assert _resume_outcomes(run, (0, 2)) == _returns(False, True)


for _name in ("isinstance", "issubclass"):
    _case(f"{_name}_normalizes_hook_result")(lambda name=_name: _abstract_check_case(name))


def _attribute_mutation_case(name: str) -> None:
    def run(choose: Choose) -> tuple[None, int]:
        class Target:
            selected = 0

            def __setattr__(self, name: str, value: Any) -> Any:
                if name == "selected":
                    object.__setattr__(self, name, value)
                    return
                object.__setattr__(self, "selected", cast(int, choose()))
                object.__setattr__(self, name, value)
                return "ignored"

            def __delattr__(self, name: str) -> Any:
                object.__setattr__(self, "selected", cast(int, choose()))
                # Keep the target immutable across shots.  A continuation
                # snapshot copies frame references, not arbitrary heap
                # objects, so deleting the same attribute here would make a
                # later shot observe the first shot's mutation.
                return "ignored"

        target = Target()
        if name == "setattr":
            result = setattr(target, "value", 100)
        else:
            object.__setattr__(target, "value", 100)
            result = delattr(target, "value")
        return result, target.selected

    outcomes = _resume_outcomes(run)
    assert outcomes == _returns((None, 1), (None, 10))


for _name in ("setattr", "delattr"):
    _case(f"{_name}_returns_none")(lambda name=_name: _attribute_mutation_case(name))


def _abstract_tuple_case(name: str) -> None:
    operation = isinstance if name == "isinstance" else issubclass
    hook_name = "__instancecheck__" if name == "isinstance" else "__subclasscheck__"

    def run(choose: Choose) -> bool:
        def hook(_self: Any, _candidate: Any) -> Any:
            return choose()

        checker = type("Checker", (type,), {hook_name: hook})(
            "Checked",
            (),
            {},
        )
        candidate: Any = object() if name == "isinstance" else type("Candidate", (), {})
        first = str
        result = operation(candidate, (first, checker))
        assert type(result) is bool
        return result

    assert _resume_outcomes(run, (0, 2)) == _returns(False, True)


for _name in ("isinstance", "issubclass"):
    _case(f"{_name}_tuple_of_classes")(lambda name=_name: _abstract_tuple_case(name))


def _abstract_invalid_tuple_case(name: str) -> None:
    operation = isinstance if name == "isinstance" else issubclass
    hook_name = "__instancecheck__" if name == "isinstance" else "__subclasscheck__"

    def run(choose: Choose) -> bool:
        def hook(_self: Any, _candidate: Any) -> Any:
            return choose()

        checker = type("Checker", (type,), {hook_name: hook})(
            "Checked",
            (),
            {},
        )
        candidate: Any = object() if name == "isinstance" else type("Candidate", (), {})
        return operation(candidate, (checker, 1))  # pyright: ignore[reportArgumentType]

    assert _resume_outcomes(run, (0, 2)) == [
        ("raise", "TypeError"),
        ("return", True),
    ]


for _name in ("isinstance", "issubclass"):
    _case(f"{_name}_invalid_tuple_isolated")(lambda name=_name: _abstract_invalid_tuple_case(name))


@_case("eval_pending_suffix")
def _eval_pending_suffix() -> None:
    def run(choose: Choose) -> int:
        return cast(int, eval("choose() + 100", {"choose": choose})) + 1000

    assert _resume_outcomes(run) == _returns(1101, 1110)


@_case("exec_pending_assignment")
def _exec_pending_assignment() -> None:
    def run(choose: Choose) -> int:
        namespace = {"choose": choose}
        exec("result = choose() + 100", namespace)
        return cast(int, namespace["result"]) + 1000

    assert _resume_outcomes(run) == _returns(1101, 1110)


@_case("eval_exception_isolated")
def _eval_exception_isolated() -> None:
    class ExpectedEvalError(Exception):
        pass

    def run(choose: Choose) -> int:
        def callback() -> int:
            value = choose()
            if value == "raise":
                raise ExpectedEvalError
            return cast(int, value)

        return cast(int, eval("callback() + 100", {"callback": callback}))

    assert _resume_outcomes(run, ("raise", 10)) == [
        ("raise", "ExpectedEvalError"),
        ("return", 110),
    ]


@_case("exec_exception_isolated")
def _exec_exception_isolated() -> None:
    class ExpectedExecError(Exception):
        pass

    def run(choose: Choose) -> int:
        def callback() -> int:
            value = choose()
            if value == "raise":
                raise ExpectedExecError
            return cast(int, value)

        namespace = {"callback": callback}
        exec("result = callback() + 100", namespace)
        return cast(int, namespace["result"])

    assert _resume_outcomes(run, ("raise", 10)) == [
        ("raise", "ExpectedExecError"),
        ("return", 110),
    ]


@_case("breakpoint_pending_suffix")
def _breakpoint_pending_suffix() -> None:
    original = sys.breakpointhook

    def run(choose: Choose) -> int:
        sys.breakpointhook = lambda: cast(int, choose()) + 100
        return cast(int, breakpoint()) + 1000

    try:
        assert _resume_outcomes(run) == _returns(1101, 1110)
    finally:
        sys.breakpointhook = original


@_case("vars_effectful_dict_lookup")
def _vars_effectful_dict_lookup() -> None:
    def run(choose: Choose) -> dict[str, int]:
        class Target:
            def __getattribute__(self, name: str) -> Any:
                if name == "__dict__":
                    return {"value": cast(int, choose())}
                return object.__getattribute__(self, name)

        return vars(Target())

    assert _resume_outcomes(run) == _returns({"value": 1}, {"value": 10})


@_case("memoryview_effectful_buffer")
def _memoryview_effectful_buffer() -> None:
    if sys.version_info < (3, 12):
        return

    def run(choose: Choose) -> bytes:
        class Buffer:
            def __buffer__(self, _flags: int) -> memoryview:
                return memoryview(bytes((cast(int, choose()),)))

            def __release_buffer__(self, _view: memoryview) -> None:
                pass

        return bytes(memoryview(Buffer()))

    assert _resume_outcomes(run) == _returns(b"\x01", b"\x0a")


@_case("memoryview_invalid_buffer_isolated")
def _memoryview_invalid_buffer_isolated() -> None:
    if sys.version_info < (3, 12):
        return

    def run(choose: Choose) -> bytes:
        class Buffer:
            def __buffer__(self, _flags: int) -> Any:
                value = choose()
                return value if value == "invalid" else memoryview(bytes((cast(int, value),)))

            def __release_buffer__(self, _view: memoryview) -> None:
                pass

        return bytes(memoryview(Buffer()))

    assert _resume_outcomes(run, ("invalid", 10)) == [
        ("raise", "TypeError"),
        ("return", b"\x0a"),
    ]


def _simple_builtin_case(name: str) -> None:
    def run(choose: Choose) -> Any:
        def protocol(_self: Any, *_args: Any) -> Any:
            value = cast(int, choose())
            if name == "complex":
                return complex(value, 2)
            if name == "float":
                return float(value)
            if name in {"format", "repr", "str"}:
                return f"{name}:{value}"
            if name == "divmod":
                return value, 100
            return value

        method_name = {
            "abs": "__abs__",
            "bool": "__bool__",
            "complex": "__complex__",
            "divmod": "__divmod__",
            "float": "__float__",
            "format": "__format__",
            "int": "__int__",
            "pow": "__pow__",
            "repr": "__repr__",
            "str": "__str__",
        }[name]
        target = type("BuiltinTarget", (), {method_name: protocol})()
        if name == "bool":
            return bool(target)
        if name == "divmod":
            return divmod(target, 2)  # pyright: ignore[reportArgumentType, reportCallIssue, reportUnknownVariableType]
        if name == "format":
            return format(target, "spec")
        if name == "pow":
            return pow(target, 2)  # pyright: ignore[reportArgumentType, reportCallIssue, reportUnknownVariableType]
        return {  # pyright: ignore[reportCallIssue, reportUnknownVariableType]
            "abs": abs,
            "complex": complex,
            "float": float,
            "int": int,
            "repr": repr,
            "str": str,
        }[name](target)  # pyright: ignore[reportArgumentType]

    if name == "bool":
        assert _resume_outcomes(run, (False, True)) == _returns(False, True)
    else:
        expected = {
            "abs": (1, 10),
            "complex": (complex(1, 2), complex(10, 2)),
            "divmod": ((1, 100), (10, 100)),
            "float": (1.0, 10.0),
            "format": ("format:1", "format:10"),
            "int": (1, 10),
            "pow": (1, 10),
            "repr": ("repr:1", "repr:10"),
            "str": ("str:1", "str:10"),
        }[name]
        assert _resume_outcomes(run) == _returns(*expected)


for _name in ("abs", "bool", "complex", "divmod", "float", "format", "int", "pow", "repr", "str"):
    _case(f"builtin_{_name}_effectful_protocol")(lambda name=_name: _simple_builtin_case(name))


def _invalid_conversion_case(name: str) -> None:
    def run(choose: Choose) -> Any:
        def protocol(_self: Any, *_args: Any) -> Any:
            value = choose()
            if value == "invalid":
                return 1.5 if name == "int" else 1
            if name == "complex":
                return complex(cast(int, value), 2)
            if name == "float":
                return float(cast(int, value))
            if name in {"format", "repr", "str"}:
                return f"{name}:{value}"
            return cast(int, value)

        method_name = {
            "complex": "__complex__",
            "float": "__float__",
            "format": "__format__",
            "int": "__int__",
            "repr": "__repr__",
            "str": "__str__",
        }[name]
        target = type("InvalidBuiltinTarget", (), {method_name: protocol})()
        if name == "format":
            return format(target, "spec")
        return {  # pyright: ignore[reportCallIssue]
            "complex": complex,
            "float": float,
            "int": int,
            "repr": repr,
            "str": str,
        }[name](target)  # pyright: ignore[reportArgumentType]

    expected = {
        "complex": complex(10, 2),
        "float": 10.0,
        "format": "format:10",
        "int": 10,
        "repr": "repr:10",
        "str": "str:10",
    }[name]
    assert _resume_outcomes(run, ("invalid", 10)) == [
        ("raise", "TypeError"),
        ("return", expected),
    ]


for _name in ("complex", "float", "format", "int", "repr", "str"):
    _case(f"builtin_{_name}_invalid_result_isolated")(lambda name=_name: _invalid_conversion_case(name))


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=10,
    )


@pytest.mark.parametrize("case_name", list(_CASES))
def test_remaining_builtin_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_builtins.py --case CASE")
    _CASES[sys.argv[2]]()
