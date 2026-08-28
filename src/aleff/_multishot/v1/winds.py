from abc import ABC, abstractmethod
from contextlib import AbstractContextManager
from dataclasses import dataclass
import inspect
from contextvars import ContextVar
from typing import Any, Callable, cast, Literal, overload, TypeGuard
from types import TracebackType
import greenlet as gl
from .intf import Ref, Wind


def _noop() -> None:
    """Default `before`/`after` for a wind that only marks a dynamic extent."""
    return None


@overload
def wind[T, B: bool | None](
    before: Callable[[], AbstractContextManager[T, B]],
    after: Callable[..., Any] | None = None,
    *,
    auto_exit: Literal[True] = True,
) -> Wind[T, B]: ...


@overload
def wind[T, B: bool | None](
    before: Callable[[], AbstractContextManager[T, B]],
    after: Callable[..., Any] | None = None,
    *,
    auto_exit: Literal[False],
) -> Wind[T, Literal[False]]: ...


@overload
def wind[T](
    before: Callable[[], T],
    after: Callable[..., Any] | None = None,
) -> Wind[T, Literal[False]]: ...


@overload
def wind(
    before: None = None,
    after: Callable[..., Any] | None = None,
) -> Wind[None, Literal[False]]: ...


def wind[T, B: bool | None](  # pyright: ignore[reportInconsistentOverload]
    before: Callable[[], AbstractContextManager[T, B]] | Callable[[], T] | None = None,
    after: Callable[..., Any] | None = None,
    *,
    auto_exit: bool = True,
) -> Wind[T, B]:
    if before is None:
        # T is None
        before = cast(Callable[[], T], _noop)
    if after is None:
        after = _noop
    return _Wind[T](before, after, auto_exit=auto_exit)  # pyright: ignore[reportReturnType]


class _Ref[T](Ref[T]):
    __slots__ = ("value",)

    def __init__(self, value: T) -> None:
        self.value = value

    def unwrap(self) -> T:
        return self.value


class WindBase[T, S](ABC):
    """Base class for dynamic-wind context managers.

    Subclasses implement the ``_wind_*`` protocol methods.  The base class
    manages the wind stack and drives the snapshot/restore lifecycle for
    multi-shot continuations.

    T: type of the value yielded by ``__enter__`` (the ``as`` target)
    S: type of the snapshot state for multi-shot re-entry

    Protocol methods:

    ``_wind_enter()``
        Called on initial entry and on multi-shot re-entry.  Returns the
        value that ``__enter__`` yields (the ``as`` target).

    ``_wind_exit(exc_type, exc_val, exc_tb)``
        Called on exit.  Returns ``True`` to suppress the exception.

    ``_wind_snapshot()``
        Called at continuation capture time.  Returns an opaque value that
        will be passed to ``_wind_restore`` on re-entry.
        Default: returns ``None``.

    ``_wind_restore(state)``
        Called before ``_wind_enter`` on multi-shot re-entry.  Receives
        the value returned by ``_wind_snapshot`` at capture time.
        Default: no-op.
    """

    def __init__(self) -> None:
        # Number of times this entry has been entered without a matching exit.
        # Nesting the same object is allowed, so this is a depth, not a flag.
        self._wind_depth = 0

    def __enter__(self) -> T:
        result = self._enter_extent()
        _push_wind_entry(self)
        return result

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: Any,
    ) -> bool:
        # Checked before the pop so that an unbalanced __exit__ is reported as
        # such instead of corrupting the stack (or failing inside pop).
        self._check_active()
        _pop_wind_entry()
        return self._exit_extent(exc_type, exc_value, traceback)

    def _enter_extent(self) -> T:
        """Enter the dynamic extent, tracking the enter/exit balance."""
        result = self._wind_enter()
        self._wind_depth += 1
        return result

    def _exit_extent(
        self,
        exc_type: type[BaseException] | None = None,
        exc_val: BaseException | None = None,
        exc_tb: TracebackType | None = None,
    ) -> bool:
        """Exit the dynamic extent, tracking the enter/exit balance."""
        self._check_active()
        self._wind_depth -= 1
        return self._wind_exit(exc_type, exc_val, exc_tb)

    def _check_active(self) -> None:
        if self._wind_depth <= 0:
            raise RuntimeError("wind exit without active entry")

    @abstractmethod
    def _wind_enter(self) -> T: ...

    def _wind_exit(
        self,
        exc_type: type[BaseException] | None = None,
        exc_val: BaseException | None = None,
        exc_tb: TracebackType | None = None,
    ) -> bool:
        return False

    @abstractmethod
    def _wind_snapshot(self) -> S: ...

    def _wind_restore(self, state: S) -> None:
        pass

    @staticmethod
    def _capture(caller_gl: gl.greenlet) -> "_CapturedWinds":
        """Read the wind stack from a suspended greenlet's context and snapshot."""
        ctx = caller_gl.gr_context
        if ctx is None:
            return []
        try:
            entries = list(ctx[_wind_stack])
        except KeyError:
            return []
        return [_CapturedWindEntry(entry, entry._wind_snapshot()) for entry in entries]

    @staticmethod
    def _rewind(to_winds: "_CapturedWinds") -> None:
        """Re-enter the captured dynamic extents in the current context.

        Called by :func:`rewind` at the start of a multi-shot re-entry, from
        inside a freshly created greenlet whose context was copied from the
        *invoker* of ``k()`` — the handler.  The wind stack visible here
        therefore describes the handler's branch of the dynamic tree, not the
        continuation's.  ``k()`` returns to the handler, so that branch stays
        active: nothing is ever unwound here.

        The two branches share a prefix — the extents established outside the
        handler boundary.  Those are already active and must not be entered
        twice.  Entries below the shared prefix belong to the captured branch
        alone and are entered outermost first, each preceded by
        ``_wind_restore(snapshot)`` to restore the state captured at
        continuation capture time.

        Finally the stack is replaced by the captured entries, so a nested
        capture inside the resumed continuation cannot observe the handler's.
        """

        to_entries = [item.entry for item in to_winds]
        shared = _shared_prefix_len(_get_wind_stack(), to_entries)

        entered: list[WindBase[Any, Any]] = []
        try:
            for item in to_winds[shared:]:
                item.entry._wind_restore(item.snapshot)
                item.entry._enter_extent()
                entered.append(item.entry)
        except BaseException:
            # _rewind runs before restore_continuation(), so there is no Python
            # frame yet whose __exit__ would unwind these on propagation.
            # Undo them here, innermost first, or the extents are lost.
            for entry in reversed(entered):
                entry._exit_extent()
            raise

        _set_wind_stack(to_entries)


class _Wind[T](WindBase[Ref[T], None]):
    """Context manager that establishes a dynamic-wind guard.

    Usage:

    ```python
    with wind(lambda: open("f.txt")) as ref:
        ref.unwrap().read()

    with wind(lambda: log("before"), lambda: log("after")):
        ...

    with wind(after=lambda: cleanup()):
        ...
    ```
    """

    def __init__(
        self,
        before: Callable[[], AbstractContextManager[T]] | Callable[[], T],
        after: Callable[[T], Any] | Callable[[], Any],
        *,
        auto_exit: bool = True,
    ) -> None:
        super().__init__()
        self._before = before
        self._after = after
        self._auto_exit = auto_exit
        self._cm: AbstractContextManager[T] | None = None  # lifetime: _wind_enter() -> _wind_exit()
        self._ref: _Ref[T] | None = None  # lifetime: ~_Wind

    def _wind_enter(self) -> Ref[T]:
        # A single _cm slot means an auto_exit guard cannot be nested inside
        # itself.  WindBase already tracks the enter/exit balance, so a live
        # _cm here means the same object was re-entered — not that an exit
        # was skipped.
        if self._cm is not None:
            raise RuntimeError("wind entry already active")

        value = self._before()

        if self._auto_exit and isinstance(value, AbstractContextManager):
            self._cm = cast(AbstractContextManager[T], value)
            value = self._cm.__enter__()
            return self._update_ref(value)
        else:
            return self._update_ref(cast(T, value))

    def _wind_exit(
        self,
        exc_type: type[BaseException] | None = None,
        exc_val: BaseException | None = None,
        exc_tb: TracebackType | None = None,
    ) -> bool:
        # WindBase tracks the enter/exit balance, so _wind_enter has run.
        assert self._ref is not None

        suppress = False
        value = self._ref.unwrap()

        if self._cm is not None:
            suppress = bool(self._cm.__exit__(exc_type, exc_val, exc_tb))

        if _accepts_positional(self._after):
            self._after(value)
        else:
            fn = cast(Callable[[], Any], self._after)
            fn()

        # Clear _cm but NOT _ref.  _ref is a heap object shared with the
        # restored frame in multi-shot continuations.  On re-entry,
        # _wind_enter updates _ref.value in-place so the restored frame
        # sees the new value via the same object reference.  Clearing
        # _ref here would break that contract.
        self._cm = None
        return suppress

    def _wind_snapshot(self) -> None:
        return None

    def _update_ref(self, value: T) -> Ref[T]:
        if self._ref is None:
            self._ref = _Ref(value)
        else:
            self._ref.value = value

        return self._ref


class wind_range(WindBase["wind_range", int]):
    """Context manager providing multi-shot-safe range iteration.

    Usage:

    ```python
    with wind_range(10) as r:
        for i in r:
            v = choose()   # multi-shot safe
    ```

    On multi-shot re-entry, the iterator position is restored to the value
    it had when the continuation was captured, so the ``for`` loop resumes
    from the correct position.
    """

    @property
    def start(self) -> int:
        return self._start

    @property
    def stop(self) -> int:
        return self._stop

    @property
    def step(self) -> int:
        return self._step

    @overload
    def __init__(self, stop: int, /) -> None: ...

    @overload
    def __init__(self, start: int, stop: int, step: int = 1, /) -> None: ...

    def __init__(self, start: int, stop: int | None = None, step: int = 1) -> None:
        super().__init__()
        if stop is None:
            # wind_range(stop) form: start=0, stop=start
            start, stop = 0, start
        if step == 0:
            raise ValueError("step must not be zero")

        self._start = start
        self._stop = stop
        self._step = step
        self._pos = start

    def _wind_enter(self) -> "wind_range":
        return self

    def _wind_snapshot(self) -> int:
        return self._pos

    def _wind_restore(self, state: int) -> None:
        self._pos = state

    def __iter__(self) -> "wind_range":
        return self

    def __next__(self) -> int:
        sign = 1 if self._step > 0 else -1
        ended = sign * self._pos >= sign * self._stop
        if ended:
            raise StopIteration
        val = self._pos
        self._pos += self._step
        return val


class wind_count(WindBase["wind_count", Any]):
    """Multi-shot-safe adapter for :func:`itertools.count`."""

    def __init__(self, start: Any = 0, step: Any = 1) -> None:
        super().__init__()
        self._current = start
        self._step = step

    def _wind_enter(self) -> "wind_count":
        return self

    def _wind_snapshot(self) -> Any:
        return self._current

    def _wind_restore(self, state: Any) -> None:
        self._current = state

    def __iter__(self) -> "wind_count":
        return self

    def __next__(self) -> Any:
        value = self._current
        self._current += self._step
        return value


class wind_repeat[T](WindBase["wind_repeat[T]", int | None]):
    """Multi-shot-safe adapter for :func:`itertools.repeat`."""

    def __init__(self, value: T, times: int | None = None) -> None:
        super().__init__()
        if times is not None:
            import operator

            times = max(0, operator.index(times))
        self._value = value
        self._remaining = times

    def _wind_enter(self) -> "wind_repeat[T]":
        return self

    def _wind_snapshot(self) -> int | None:
        return self._remaining

    def _wind_restore(self, state: int | None) -> None:
        self._remaining = state

    def __iter__(self) -> "wind_repeat[T]":
        return self

    def __next__(self) -> T:
        if self._remaining == 0:
            raise StopIteration
        if self._remaining is not None:
            self._remaining -= 1
        return self._value


def _accepts_positional(fn: Callable[..., Any]) -> TypeGuard[Callable[[Any], Any]]:
    """Return True if *fn* accepts at least one positional argument.

    Used to decide whether to call ``after(value)`` or ``after()``
    when the wind guard exits.  Only positional-only, positional-or-keyword,
    and ``*args`` parameters are considered; keyword-only and ``**kwargs``
    are not, because the value is always passed positionally.
    """
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


# Type for captured wind entries: (entry, snapshot) pairs
@dataclass(frozen=True, slots=True)
class _CapturedWindEntry[T, S]:
    entry: WindBase[T, S]
    snapshot: S


type _CapturedWinds = list[_CapturedWindEntry[Any, Any]]

_wind_stack: ContextVar[list[WindBase[Any, Any]]] = ContextVar("dynamic_wind_stack")


# for handlers.py
def capture_wind_stack(caller_gl: gl.greenlet) -> _CapturedWinds:
    """Read the wind stack from a suspended greenlet's context and snapshot."""
    return WindBase._capture(caller_gl)  # pyright: ignore[reportPrivateUsage]


# for handlers.py
def rewind(
    captured: _CapturedWinds,
    current: list[WindBase[Any, Any]] | None = None,
) -> None:
    """Re-enter the *captured* dynamic extents for a multi-shot continuation."""
    if current is not None:
        _set_wind_stack(current)
    WindBase._rewind(captured)  # pyright: ignore[reportPrivateUsage]


# for handlers.py
def current_wind_stack() -> list[WindBase[Any, Any]]:
    """Return the current dynamic-wind path without snapshotting its entries."""

    return _get_wind_stack()


def _shared_prefix_len(
    current: list[WindBase[Any, Any]],
    captured: list[WindBase[Any, Any]],
) -> int:
    """Length of the deepest common ancestor of two dynamic-extent paths.

    A wind stack is the path from the dynamic root to a dynamic point, so the
    longest common prefix (by identity) is the extents both paths are inside.
    """
    n = 0
    for a, b in zip(current, captured):
        if a is not b:
            break
        n += 1
    return n


def _get_wind_stack() -> list[WindBase[Any, Any]]:
    try:
        return list(_wind_stack.get())
    except LookupError:
        return []


def _set_wind_stack(stack: list[WindBase[Any, Any]]) -> None:
    _wind_stack.set(list(stack))


def _push_wind_entry(entry: WindBase[Any, Any]) -> None:
    stack = _get_wind_stack()
    stack.append(entry)
    _set_wind_stack(stack)


def _pop_wind_entry() -> WindBase[Any, Any]:
    stack = _get_wind_stack()
    v = stack.pop()
    _set_wind_stack(stack)
    return v
