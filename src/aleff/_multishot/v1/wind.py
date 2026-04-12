from __future__ import annotations

import inspect
from contextvars import ContextVar
from typing import Any, Callable
import greenlet as gl
from .intf import Ref


class wind:
    """Context manager that establishes a dynamic-wind guard.

    Usage::

        with wind(lambda: open("f.txt")) as ref:
            ref.unwrap().read()

        with wind(lambda: log("before"), lambda: log("after")):
            ...

        with wind(after=lambda: cleanup()):
            ...
    """

    def __init__(
        self,
        before: Callable[[], Any] | None = None,
        after: Callable[..., Any] | None = None,
        *,
        auto_exit: bool = True,
    ) -> None:
        self._entry = _WindEntry(before, after, auto_exit)

    def __enter__(self) -> Ref[Any]:
        ref = self._entry.enter()
        stack = _get_wind_stack()
        stack.append(self._entry)
        _set_wind_stack(stack)
        return ref

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: Any,
    ) -> bool:
        stack = _get_wind_stack()
        stack.pop()
        _set_wind_stack(stack)
        return self._entry.exit(exc_type, exc_val, exc_tb)


def capture_wind_stack(caller_gl: gl.greenlet) -> list[_WindEntry]:
    """Read the wind stack from a suspended greenlet's context."""
    ctx = caller_gl.gr_context
    if ctx is None:
        return []
    try:
        return list(ctx[_wind_stack])
    except KeyError:
        return []


def rewind(captured_winds: list[_WindEntry]) -> None:
    """Transition from the current wind stack to *captured_winds*."""
    _do_winds(_get_wind_stack(), captured_winds)


class _Ref[T](Ref[T]):
    __slots__ = ("value",)

    def __init__(self, value: T) -> None:
        self.value = value

    def unwrap(self) -> T:
        return self.value


def _accepts_positional(fn: Callable[..., Any]) -> bool:
    try:
        sig = inspect.signature(fn)
    except (ValueError, TypeError):
        return False
    for p in sig.parameters.values():
        if p.kind in (
            inspect.Parameter.POSITIONAL_ONLY,
            inspect.Parameter.POSITIONAL_OR_KEYWORD,
            inspect.Parameter.VAR_POSITIONAL,
        ):
            return True
    return False


class _WindEntry:
    __slots__ = ("_before", "_after", "_auto_exit", "_cm", "_ref", "_raw_rv")

    def __init__(
        self,
        before: Callable[[], Any] | None,
        after: Callable[..., Any] | None,
        auto_exit: bool,
    ) -> None:
        self._before = before
        self._after = after
        self._auto_exit = auto_exit
        self._cm: Any = None
        self._ref: _Ref[Any] | None = None
        self._raw_rv: Any = None

    def enter(self) -> _Ref[Any]:
        if self._before is None:
            self._cm = None
            self._raw_rv = None
            if self._ref is None:
                self._ref = _Ref(None)
            else:
                self._ref.value = None
            return self._ref

        rv = self._before()
        self._raw_rv = rv

        if self._auto_exit and rv is not None and hasattr(rv, "__exit__"):
            self._cm = rv
            value = self._cm.__enter__()
        else:
            self._cm = None
            value = rv

        if self._ref is None:
            self._ref = _Ref(value)
        else:
            self._ref.value = value
        return self._ref

    def exit(
        self,
        exc_type: type[BaseException] | None = None,
        exc_val: BaseException | None = None,
        exc_tb: Any = None,
    ) -> bool:
        suppress = False
        if self._cm is not None:
            suppress = bool(self._cm.__exit__(exc_type, exc_val, exc_tb))
            self._cm = None
        if self._after is not None:
            if _accepts_positional(self._after):
                self._after(self._raw_rv)
            else:
                self._after()
        return suppress


# -- Wind stack ContextVar --------------------------------------------------

_wind_stack: ContextVar[list[_WindEntry]] = ContextVar("dynamic_wind_stack")


def _get_wind_stack() -> list[_WindEntry]:
    try:
        return list(_wind_stack.get())
    except LookupError:
        return []


def _set_wind_stack(stack: list[_WindEntry]) -> None:
    _wind_stack.set(list(stack))


def _do_winds(
    from_winds: list[_WindEntry],
    to_winds: list[_WindEntry],
) -> None:
    n = min(len(from_winds), len(to_winds))
    common = 0
    for i in range(n):
        if from_winds[i] is to_winds[i]:
            common = i + 1
        else:
            break

    for entry in reversed(from_winds[common:]):
        entry.exit()
    for entry in to_winds[common:]:
        entry.enter()
    _set_wind_stack(list(to_winds))
