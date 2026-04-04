from asyncio import Lock, iscoroutine
from contextvars import ContextVar, copy_context
from inspect import iscoroutinefunction
from typing import Any, Callable, cast, Coroutine
import greenlet as gl
from .intf import (
    Effect,
    Resume,
    ResumeAsync,
    EffectNotHandledError,
    EffectHandler,
    AsyncEffectHandler,
    Caller,
    handler,
    async_handler,
)
from .effects import EffectContext
from .misc import debug
from .._aleff import FrameSnapshot, restore_continuation, snapshot_from_frame


def create_handler(*effects: Effect[..., Any]) -> handler[Any]:
    """Create a synchronous handler that handles the given effects.

    Register implementations with :meth:`handler.on`, then call the handler
    with a caller function to run the computation.
    """
    return _handler[Any](*effects)


def create_async_handler(*effects: Effect[..., Any]) -> async_handler[Any]:
    """Create an asynchronous handler that handles the given effects.

    Handler functions are ``async def`` and receive :class:`ResumeAsync`.
    The caller function runs in a greenlet; effect invocations are
    synchronous from the caller's perspective.
    """
    return _async_handler[Any](*effects)


##
# greenlet-based implementation with multi-shot support
#
# Resume checks caller_gl.dead:
#   alive  → one-shot: caller_gl.switch(value)
#   dead   → multi-shot: restore_continuation(snapshot, value) in new greenlet
#
# A fresh Resume is created at each effect dispatch in _drive/_drive_async,
# capturing the current caller_gl and snapshot. This avoids statefulness
# and prevents infinite recursion on multi-shot.
##


class _Resume[R, V](Resume[R, V]):
    def __init__(self, caller_gl: gl.greenlet, snapshot: FrameSnapshot[R, V]) -> None:
        self._caller_gl = caller_gl
        self._snapshot = snapshot

    def __call__(self, value: R) -> V:
        caller_gl = self._caller_gl

        if not caller_gl.dead:
            debug("||> @caller (one-shot)")
            v = caller_gl.switch(value)
            debug(f"||< @caller = {v!r}")
            return _drive(caller_gl, v)

        debug("||> @caller (multi-shot)")

        ss = self._snapshot

        def _body() -> V:
            return restore_continuation(ss, value)

        new_gl = gl.greenlet(_body)
        new_gl.gr_context = copy_context()
        v = _drive(new_gl, new_gl.switch())

        debug(f"||< @caller (multi-shot) = {v!r}")

        return v


class _ResumeAsync[R, V](ResumeAsync[R, V]):
    def __init__(self, caller_gl: gl.greenlet, snapshot: FrameSnapshot[R, V]) -> None:
        self._caller_gl = caller_gl
        self._snapshot = snapshot

    async def __call__(self, value: R) -> V:
        caller_gl = self._caller_gl

        if not caller_gl.dead:
            debug("||> @caller (one-shot async)")
            v = caller_gl.switch(value)
            debug(f"||< @caller = {v!r}")
            return await _drive_async(caller_gl, v)

        debug("||> @caller (multi-shot async)")

        ss = self._snapshot

        def _body() -> V:
            return restore_continuation(ss, value)

        new_gl = gl.greenlet(_body)
        new_gl.gr_context = copy_context()
        v = await _drive_async(new_gl, new_gl.switch())

        debug(f"||< @caller (multi-shot async) = {v!r}")

        return v


def _pre_drive(
    caller_gl: Any,
    value: EffectContext[..., Any],
) -> tuple[
    Effect[..., Any], "_handler[Any] | _async_handler[Any]", Callable[..., Any], tuple[Any, ...], dict[str, Any]
]:
    if not isinstance(value, EffectContext):  # pyright: ignore[reportUnnecessaryIsInstance]
        raise RuntimeError(f"invalid value passed to caller: {value!r}")

    effect = value.effect
    args = value.args
    kwargs = value.kwargs

    found = _get_item(effect)
    if found is None:
        raise EffectNotHandledError(effect)

    handler, fn = found

    debug(f"||> ... found handler {handler} | {fn.__name__}")

    return effect, handler, fn, args, kwargs


def _drive[V](caller_gl: Any, value: V | EffectContext[..., Any]) -> V:
    debug("||> @main")

    if caller_gl.dead:
        # computation finished
        debug(f"||= return = {value!r}")
        debug("||< @main")
        assert not isinstance(value, EffectContext)
        return value

    # effect performed
    # switch to the handler greenlet

    effect, handler, fn, args, kwargs = _pre_drive(caller_gl, cast(EffectContext[..., Any], value))

    if isinstance(handler, _async_handler):
        # async handler found in sync context — relay to parent greenlet
        parent = gl.getcurrent().parent
        assert parent is not None
        debug(f"||> relay {effect} to async handler")
        resume_value = parent.switch(value)
        v = caller_gl.switch(resume_value)
        debug(f"||< relay {effect}")
        return _drive(caller_gl, v)

    # Take snapshot from the handler greenlet. The caller greenlet is
    # suspended at this point, so its frames have valid stacktop values.
    snapshot = snapshot_from_frame(caller_gl.gr_frame)

    resume: Resume[Any, Any] = _Resume(caller_gl, snapshot)
    v = fn(resume, *args, **kwargs)

    if not caller_gl.dead:
        # resume not called in the handler
        # discard the result
        debug(f"||< **abort** perform {effect} = {v!r}")
        caller_gl.throw(gl.GreenletExit)

    debug(f"||< perform {effect} = {v!r}")

    debug("||< @main")
    return v


async def _drive_async[V](caller_gl: Any, value: V | EffectContext[..., Any]) -> V:
    debug("||> @main")

    if caller_gl.dead:
        # computation finished
        assert not isinstance(value, EffectContext)
        if iscoroutine(value):
            v = await value
        else:
            v = value
        debug(f"||= return = {value!r}")
        debug("||< @main")
        return cast(V, v)

    # effect performed
    # switch to the handler greenlet

    effect, _, fn, args, kwargs = _pre_drive(caller_gl, cast(EffectContext[..., Any], value))

    snapshot = snapshot_from_frame(caller_gl.gr_frame)
    resume: ResumeAsync[Any, Any] = _ResumeAsync(caller_gl, snapshot)

    if iscoroutinefunction(fn):
        v = await fn(resume, *args, **kwargs)
    else:
        v = fn(resume, *args, **kwargs)

    if not caller_gl.dead:
        # resume not called in the handler
        # discard the result
        debug(f"||< **abort** perform {effect} = {v!r}")
        caller_gl.throw(gl.GreenletExit)

    debug(f"||< perform {effect} = {v!r}")

    debug("||< @main")
    return v


_num = 0


class _handler_base[
    EffectType,
    EffectHandlerType: Callable[..., Any],
    CallerType: Callable[[], Any],
]:
    def __init__(self, *effects: EffectType):
        global _num

        self._effects = tuple(effects)
        self._unbound_effects = set(effects)
        self._reserved_effects: list[tuple[EffectType, EffectHandlerType]] = []
        self._n = _num
        _num += 1
        debug(f"@ {self}")

    def check(self, caller: CallerType) -> None:
        if len(self._unbound_effects) > 0:
            unboud_effects = ", ".join(str(e) for e in self._unbound_effects)
            raise ValueError(f"not all effects are handled: {unboud_effects}")


class _handler[V](
    _handler_base[
        Effect[..., Any],
        EffectHandler[..., V, Any],
        Caller[V],
    ],
    handler[V],
):
    @property
    def effects(self) -> frozenset[Effect[..., Any]]:
        return frozenset(self._effects)

    def on[**P, R](self, effect: Effect[P, R]) -> Callable[[EffectHandler[P, V, R]], EffectHandler[P, V, R]]:
        debug(f"|+ {effect} | {self}")

        def decorator(fn: EffectHandler[P, V, R]) -> EffectHandler[P, V, R]:
            self._reserved_effects.append((effect, fn))
            return fn

        # raises an error if the same effect is handled multiple times
        if effect not in self._effects:
            raise ValueError(f"{effect} is not declared in the handler")
        if effect not in self._unbound_effects:
            raise ValueError(f"{effect} is already handled")
        self._unbound_effects.remove(effect)

        return decorator

    def __call__(self, caller: Caller[V], *, check: bool = True) -> V:
        debug(f"|> {self}")

        if check:
            self.check(caller)

        token = object()

        # setup
        _put_handlers(token, self, self._reserved_effects)

        try:
            caller_gl = gl.greenlet(caller)
            caller_gl.gr_context = copy_context()

            debug(f"|> @caller | {self}")

            v = _drive(caller_gl, caller_gl.switch())

            debug(f"|< @caller = {v!r}")

            return v
        finally:
            _remove_all_handlers(token)

    def __str__(self) -> str:
        return f"#handler({self._n}) | {', '.join(e.name for e in self._effects)}"


class _async_handler[V](
    _handler_base[
        Effect[..., Any],
        AsyncEffectHandler[..., V, Any],
        Caller[V | Coroutine[Any, Any, V]],
    ],
    async_handler[V],
):
    @property
    def effects(self) -> frozenset[Effect[..., Any]]:
        return frozenset(self._effects)

    def on[**P, R](
        self,
        effect: Effect[P, R],
    ) -> Callable[[AsyncEffectHandler[P, V, R]], AsyncEffectHandler[P, V, R]]:
        debug(f"|+ {effect} | {self}")

        def decorator(fn: AsyncEffectHandler[P, V, R]) -> AsyncEffectHandler[P, V, R]:
            self._reserved_effects.append((effect, fn))
            return fn

        # raises an error if the same effect is handled multiple times
        if effect not in self._effects:
            raise ValueError(f"{effect} is not declared in the handler")
        if effect not in self._unbound_effects:
            raise ValueError(f"{effect} is already handled")
        self._unbound_effects.remove(effect)

        return decorator

    async def __call__(self, caller: Caller[V | Coroutine[Any, Any, V]], *, check: bool = True) -> V:
        debug(f"|> {self}")

        if check:
            self.check(caller)

        token = object()

        # setup
        await _put_handlers_async(token, self, self._reserved_effects)

        try:
            caller_gl = gl.greenlet(caller)
            caller_gl.gr_context = copy_context()

            debug(f"|> @caller | {self}")

            v = await _drive_async(caller_gl, caller_gl.switch())

            debug(f"|< @caller = {v!r}")

            return v
        finally:
            await _remove_all_handlers_async(token)

    def __str__(self) -> str:
        return f"#handler({self._n}) | {', '.join(e.name for e in self._effects)}"


# ---

# Handler stack
# (token, handler, effect, fn)
# NB. two greenlets cannot share the ContextVar
#     so only handler_gl should access this stack

type _StackItem[**P, R, V] = tuple[object, _handler[V] | _async_handler[V], Effect[P, R], Callable[P, V]]

_stack: ContextVar[list[_StackItem[..., Any, Any]]] = ContextVar("handler_stack")
_lock = Lock()


def _get_stack() -> list[_StackItem[..., Any, Any]]:
    try:
        return _stack.get()
    except LookupError:
        stack: list[_StackItem[..., Any, Any]] = []
        _stack.set(stack)
        return stack


def _set_stack(stack: list[_StackItem[..., Any, Any]]) -> None:
    _stack.set(stack)


def _get_item[**P, R](effect: Effect[P, R]) -> tuple[_handler[Any] | _async_handler[Any], Callable[P, Any]] | None:
    s = _get_stack()
    for _, h, e, f in reversed(s):
        if e is effect:
            return h, f
    return None


def _put_handlers[**P, R, V](
    token: object,
    handler: _handler[V],
    effects: list[tuple[Effect[P, R], Callable[P, V]]],
) -> None:
    stack = _get_stack()
    for effect, fn in effects:
        stack.append((token, handler, effect, fn))
    _set_stack(stack)


async def _put_handlers_async[**P, R, V](
    token: object,
    handler: _async_handler[V],
    effects: list[tuple[Effect[P, R], Callable[P, Coroutine[Any, Any, V]]]],
) -> None:
    async with _lock:
        stack = _get_stack()
        for effect, fn in effects:
            stack.append((token, handler, effect, fn))
        _set_stack(stack)


def _remove_all_handlers(token: object) -> None:
    stack = [x for x in _get_stack() if x[0] is not token]
    _set_stack(stack)


async def _remove_all_handlers_async(token: object) -> None:
    async with _lock:
        stack = _get_stack()
        stack = [x for x in stack if x[0] is not token]
        _set_stack(stack)
