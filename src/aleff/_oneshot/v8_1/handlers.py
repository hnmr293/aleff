from asyncio import Lock, iscoroutine
from contextvars import ContextVar
from functools import wraps, partial
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
    Handler,
    AsyncHandler,
)
from .effects import EffectContext
from .misc import debug


def create_handler(*effects: Effect[..., Any]) -> Handler[Any]:
    """Create a synchronous handler that handles the given effects.

    Register implementations with :meth:`handler.on`, then call the handler
    with a caller function to run the computation.
    """
    return _handler[Any](*effects)


def create_async_handler(*effects: Effect[..., Any]) -> AsyncHandler[Any]:
    """Create an asynchronous handler that handles the given effects.

    Handler functions are ``async def`` and receive :class:`ResumeAsync`.
    The caller function runs in a greenlet; effect invocations are
    synchronous from the caller's perspective.
    """
    return _async_handler[Any](*effects)


##
# greenlet-based implementation
#
# handler_gl --|caller_gl.switch()|-> caller_gl
#   effect --|handler_gl.switch(eff, args)|-> handler_gl
#     resume v --|caller_gl.switch(eff, v)|-> caller_gl
#     return v --> *finish* v
##


class _Resume[R, V](Resume[R, V]):
    def __call__(self, value: R) -> V:
        caller_gl = _get_caller()

        debug("||> @caller")

        v = caller_gl.switch(value)

        debug(f"||< @caller = {v!r}")

        return _drive(caller_gl, v)


class _ResumeAsync[R, V](ResumeAsync[R, V]):
    async def __call__(self, value: R) -> V:
        caller_gl = _get_caller()

        debug("||> @caller")

        v = caller_gl.switch(value)

        debug(f"||< @caller = {v!r}")

        return await _drive_async(caller_gl, v)


def _pre_drive[V](caller_gl: Any, value: EffectContext[..., Any]):
    if not isinstance(value, EffectContext):  # pyright: ignore[reportUnnecessaryIsInstance]
        raise RuntimeError(f"invalid value passed to caller: {value!r}")

    effect: Effect[..., Any]
    effect, args, kwargs = value.effect, value.args, value.kwargs  # type: ignore

    found = _get_item(effect)
    if found is None:
        raise EffectNotHandledError(effect)

    handler, fn = found

    debug(f"||> ... found handler {handler} | {fn.__name__}")

    _set_caller(caller_gl)

    return effect, fn, args, kwargs


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

    effect, fn, args, kwargs = _pre_drive(caller_gl, cast(EffectContext[..., Any], value))
    v = fn(*args, **kwargs)

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

    effect, fn, args, kwargs = _pre_drive(caller_gl, cast(EffectContext[..., Any], value))

    if iscoroutinefunction(fn):
        v = await fn(*args, **kwargs)
    else:
        v = fn(*args, **kwargs)

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
    ReducedHandlerType: Callable[..., Any],
    CallerType: Callable[[], Any],
]:
    def __init__(self, *effects: EffectType):
        global _num

        self._effects = tuple(effects)
        self._unbound_effects = set(effects)
        self._reserved_effects: list[tuple[EffectType, ReducedHandlerType]] = []
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
        Callable[..., V],
        Caller[V],
    ],
    Handler[V],
):
    @property
    def effects(self) -> frozenset[Effect[..., Any]]:
        return frozenset(self._effects)

    def on[**P, R](self, effect: Effect[P, R]) -> Callable[[EffectHandler[P, V, R]], Callable[P, V]]:
        debug(f"|+ {effect} | {self}")

        resume = _Resume[Any, Any]()

        def decorator(fn: EffectHandler[P, V, Any]) -> Callable[P, V]:
            fn = wraps(fn)(partial(fn, resume))  # type: ignore
            f = cast(Callable[P, V], fn)
            self._reserved_effects.append((effect, f))
            return f

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
        Callable[..., Coroutine[Any, Any, V]],
        Caller[V | Coroutine[Any, Any, V]],
    ],
    AsyncHandler[V],
):
    @property
    def effects(self) -> frozenset[Effect[..., Any]]:
        return frozenset(self._effects)

    def on[**P, R](
        self,
        effect: Effect[P, R],
    ) -> Callable[[AsyncEffectHandler[P, V, R]], Callable[P, Coroutine[Any, Any, V]]]:
        debug(f"|+ {effect} | {self}")

        resume = _ResumeAsync[Any, Any]()

        def decorator(fn: AsyncEffectHandler[P, V, Any]) -> Callable[P, Coroutine[Any, Any, V]]:
            fn = wraps(fn)(partial(fn, resume))  # type: ignore
            f = cast(Callable[P, Coroutine[Any, Any, V]], fn)
            self._reserved_effects.append((effect, f))
            return f

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

            debug(f"|> @caller | {self}")

            v = await _drive_async(caller_gl, caller_gl.switch())

            debug(f"|< @caller = {v!r}")

            return v
        finally:
            await _remove_all_handlers_async(token)

    def __str__(self) -> str:
        return f"#handler({self._n}) | {', '.join(e.name for e in self._effects)}"


# ---

_caller_gl = ContextVar("caller_gl", default=None)


def _get_caller() -> Any:
    g = _caller_gl.get()
    if g is None:
        raise RuntimeError("no caller found")
    return g


def _set_caller(g: Any) -> None:
    _caller_gl.set(g)


# ---

# handler stask
# NB. two greenlets cannot share the ContextVar
#     so only handler_gl should access this stack

# (runner, effect, handler)
type _StackItem[**P, R, V] = tuple[object, _handler[V] | _async_handler[V], Effect[P, R], Callable[P, V]]

_stack = ContextVar[list[_StackItem[..., Any, Any]]]("handler_stask", default=[])
_lock = Lock()


def _get_stack() -> list[_StackItem[..., Any, Any]]:
    return _stack.get()


def _set_stack(stack: list[_StackItem[..., Any, Any]]) -> None:
    _stack.set(stack)


def _get_item[**P, R](effect: Effect[P, R]) -> tuple[_handler[Any] | _async_handler[Any], Callable[P, Any]] | None:
    s: list[_StackItem[..., Any, Any]] = _get_stack()
    for _, h, e, f in reversed(s):
        if e is effect:
            return h, f
    return None


def _put_handlers[**P, R, V](
    token: object,
    handler: _handler[V],
    effects: list[tuple[Effect[P, R], Callable[P, V]]],
) -> None:
    stask = _get_stack()
    for effect, fn in effects:
        stask.append((token, handler, effect, fn))
    _set_stack(stask)


async def _put_handlers_async[**P, R, V](
    token: object,
    handler: _async_handler[V],
    effects: list[tuple[Effect[P, R], Callable[P, Coroutine[Any, Any, V]]]],
) -> None:
    async with _lock:
        stask = _get_stack()
        for effect, fn in effects:
            stask.append((token, handler, effect, fn))
        _set_stack(stask)


def _remove_all_handlers[**P, R, V](token: object) -> None:
    stack = [x for x in _get_stack() if x[0] is not token]
    _set_stack(stack)


async def _remove_all_handlers_async[**P, R, V](token: object) -> None:
    async with _lock:
        stack = _get_stack()
        stack = [x for x in stack if x[0] is not token]
        _set_stack(stack)
