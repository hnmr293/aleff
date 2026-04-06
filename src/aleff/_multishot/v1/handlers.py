from asyncio import Lock, iscoroutine
from contextvars import ContextVar, copy_context
from dataclasses import dataclass
from inspect import iscoroutinefunction
from typing import Any, Awaitable, Callable, cast, Coroutine
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
from .misc import debug, eff_str
from ._aleff import FrameSnapshot, restore_continuation, snapshot_from_frame


def create_handler(*effects: Effect[..., Any], shallow: bool = False) -> Handler[Any]:
    """Create a synchronous handler that handles the given effects.

    Register implementations with :meth:`handler.on`, then call the handler
    with a caller function to run the computation.

    If *shallow* is ``True``, the handler is removed from the handler stack
    after handling one effect occurrence.  Subsequent occurrences of the
    same effect will not be caught by this handler.
    """
    return _Handler[Any](*effects, shallow=shallow)


def create_async_handler(*effects: Effect[..., Any], shallow: bool = False) -> AsyncHandler[Any]:
    """Create an asynchronous handler that handles the given effects.

    Handler functions are ``async def`` and receive :class:`ResumeAsync`.
    The caller function runs in a greenlet; effect invocations are
    synchronous from the caller's perspective.

    If *shallow* is ``True``, the handler is removed from the handler stack
    after handling one effect occurrence.
    """
    return _AsyncHandler[Any](*effects, shallow=shallow)


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

            # Redirect caller_gl.parent so that effect invocations
            # (which do ``gl.getcurrent().parent.switch(...)``) arrive
            # at the greenlet that is currently driving the handler,
            # not the one that originally created caller_gl.
            # This is essential for shallow handler re-installation:
            # when a handler does ``h(lambda: k(value))``, k() is
            # called from a *new* handler greenlet, and the caller
            # must switch back to it — not to the original root.
            old_parent = caller_gl.parent
            caller_gl.parent = gl.getcurrent()
            try:
                v = caller_gl.switch(value)
            finally:
                if not caller_gl.dead:
                    assert old_parent is not None
                    caller_gl.parent = old_parent

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

            # See _Resume.__call__ for why the parent is redirected.
            old_parent = caller_gl.parent
            caller_gl.parent = gl.getcurrent()
            try:
                v = caller_gl.switch(value)
            finally:
                if not caller_gl.dead:
                    assert old_parent is not None
                    caller_gl.parent = old_parent
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


@dataclass(frozen=True, slots=True)
class _EffectDispatch:
    """Result of looking up a handler for a performed effect."""

    effect: Effect[..., Any]
    token: object
    handler: "_Handler[Any] | _AsyncHandler[Any]"
    fn: Callable[..., Any]
    args: tuple[Any, ...]
    kwargs: dict[str, Any]


def _pre_drive(
    caller_gl: Any,
    value: EffectContext[..., Any],
) -> _EffectDispatch:
    if not isinstance(value, EffectContext):  # pyright: ignore[reportUnnecessaryIsInstance]
        raise RuntimeError(f"invalid value passed to caller: {value!r}")

    effect = value.effect
    args = value.args
    kwargs = value.kwargs

    found = _get_item(effect)
    if found is None:
        raise EffectNotHandledError(effect)

    token, handler, fn = found

    debug(f"||> ... found handler {handler} | {fn.__name__}")

    return _EffectDispatch(effect, token, handler, fn, args, kwargs)


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

    d = _pre_drive(caller_gl, cast(EffectContext[..., Any], value))

    if isinstance(d.handler, _AsyncHandler):
        # async handler found in sync context — relay to parent greenlet.
        #
        # Normally, when a sync handler is nested inside an async handler,
        # the sync handler's greenlet has a parent greenlet (the async
        # handler's caller greenlet) that can relay the effect upward.
        #
        # However, if the async handler's caller is an `async def`, the
        # greenlet returns a coroutine immediately and the coroutine body
        # runs in the root greenlet (via `await` in _drive_async).  In
        # that case no parent exists to relay to.
        #
        # To fix this, make the caller passed to the async handler a
        # regular (sync) function instead of an `async def`.
        parent = gl.getcurrent().parent
        if parent is None:
            raise RuntimeError(
                f"{eff_str(d.effect)} is handled by an async handler, but"
                " cannot be relayed from the current sync context."
                " The caller passed to the outer async handler should"
                " be a regular function, not an async def."
            )
        debug(f"||> relay {eff_str(d.effect)} to async handler")
        resume_value = parent.switch(value)
        v = caller_gl.switch(resume_value)
        debug(f"||< relay {eff_str(d.effect)}")
        return _drive(caller_gl, v)

    # For shallow handlers, remove all entries before calling fn so that
    # resumed computation will not find this handler on the stack.
    if d.handler.shallow:
        _remove_all_handlers(d.token)

    # Take snapshot from the handler greenlet. The caller greenlet is
    # suspended at this point, so its frames have valid stacktop values.
    snapshot = snapshot_from_frame(caller_gl.gr_frame)

    resume: Resume[Any, Any] = _Resume(caller_gl, snapshot)
    v = d.fn(resume, *d.args, **d.kwargs)

    if not caller_gl.dead:
        # resume not called in the handler
        # discard the result
        debug(f"||< **abort** perform {eff_str(d.effect)} = {v!r}")
        caller_gl.throw(gl.GreenletExit)

    debug(f"||< perform {eff_str(d.effect)} = {v!r}")

    debug("||< @main")
    return v


class _AwaitRequest[V]:
    """Wraps an awaitable switched from a bridge greenlet to _drive_async.

    When an async caller is wrapped in a sync bridge greenlet (see
    _async_handler.__call__), each ``await`` in the original coroutine
    is translated into ``parent.switch(_AwaitRequest(awaitable))``.
    _drive_async recognises this sentinel, awaits the inner awaitable,
    and returns the result to the bridge greenlet.
    """

    __slots__ = ("awaitable",)

    def __init__(self, awaitable: Awaitable[V]) -> None:
        self.awaitable = awaitable


async def _drive_async[V](caller_gl: Any, value: V | _AwaitRequest[V] | EffectContext[..., Any]) -> V:
    debug("||> @main")

    if isinstance(value, _AwaitRequest):
        # Bridge greenlet needs an awaitable resolved
        debug("||> bridge await")
        req = cast(_AwaitRequest[V], value)
        try:
            result = await req.awaitable
        except BaseException as e:
            return await _drive_async(caller_gl, caller_gl.throw(e))
        else:
            return await _drive_async(caller_gl, caller_gl.switch(result))

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

    d = _pre_drive(caller_gl, cast(EffectContext[..., Any], value))

    # For shallow handlers, remove all entries before calling fn so that
    # resumed computation will not find this handler on the stack.
    if d.handler.shallow:
        _remove_all_handlers(d.token)

    snapshot = snapshot_from_frame(caller_gl.gr_frame)
    resume: ResumeAsync[Any, Any] = _ResumeAsync(caller_gl, snapshot)

    if iscoroutinefunction(d.fn):
        v = await d.fn(resume, *d.args, **d.kwargs)
    else:
        v = d.fn(resume, *d.args, **d.kwargs)

    if not caller_gl.dead:
        # resume not called in the handler
        # discard the result
        debug(f"||< **abort** perform {eff_str(d.effect)} = {v!r}")
        caller_gl.throw(gl.GreenletExit)

    debug(f"||< perform {eff_str(d.effect)} = {v!r}")

    debug("||< @main")
    return v


_num = 0


class _handler_base[
    EffectHandlerType: Callable[..., Any],
    CallerType: Callable[[], Any],
]:
    def __init__(self, *effects: Effect[..., Any], shallow: bool = False):
        global _num

        self._effects = tuple(effects)
        self._unbound_effects = set(effects)
        self._reserved_effects: list[tuple[Effect[..., Any], EffectHandlerType]] = []
        self._shallow = shallow
        self._n = _num
        _num += 1
        debug(f"@ {self}")

    @property
    def shallow(self) -> bool:
        return self._shallow

    def check(self, caller: CallerType) -> None:
        if len(self._unbound_effects) > 0:
            unboud_effects = ", ".join(eff_str(e) for e in self._unbound_effects)
            raise ValueError(f"not all effects are handled: {unboud_effects}")


class _Handler[V](
    _handler_base[
        EffectHandler[..., V, Any],
        Caller[V],
    ],
    Handler[V],
):
    @property
    def effects(self) -> frozenset[Effect[..., Any]]:
        return frozenset(self._effects)

    def on[**P, R](self, effect: Effect[P, R]) -> Callable[[EffectHandler[P, V, R]], EffectHandler[P, V, R]]:
        debug(f"|+ {eff_str(effect)} | {self}")

        def decorator(fn: EffectHandler[P, V, R]) -> EffectHandler[P, V, R]:
            self._reserved_effects.append((effect, fn))
            return fn

        # raises an error if the same effect is handled multiple times
        if effect not in self._effects:
            raise ValueError(f"{eff_str(effect)} is not declared in the handler")
        if effect not in self._unbound_effects:
            raise ValueError(f"{eff_str(effect)} is already handled")
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


class _AsyncHandler[V](
    _handler_base[
        AsyncEffectHandler[..., V, Any],
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
    ) -> Callable[[AsyncEffectHandler[P, V, R]], AsyncEffectHandler[P, V, R]]:
        debug(f"|+ {eff_str(effect)} | {self}")

        def decorator(fn: AsyncEffectHandler[P, V, R]) -> AsyncEffectHandler[P, V, R]:
            self._reserved_effects.append((effect, fn))
            return fn

        # raises an error if the same effect is handled multiple times
        if effect not in self._effects:
            raise ValueError(f"{eff_str(effect)} is not declared in the handler")
        if effect not in self._unbound_effects:
            raise ValueError(f"{eff_str(effect)} is already handled")
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
            # If the caller is an async function, wrap it in a sync bridge
            # so that it runs inside a greenlet.  This allows nested sync
            # handlers to relay async effects through the greenlet parent
            # chain.  Each ``await`` in the original coroutine is converted
            # to a parent.switch(_AwaitRequest(...)) which _drive_async
            # resolves.
            actual_caller: Callable[[], Any] = caller
            if iscoroutinefunction(caller):
                _orig = caller

                def actual_caller() -> Any:
                    coro = _orig()
                    value: Any = None
                    parent = gl.getcurrent().parent
                    assert parent is not None
                    try:
                        while True:
                            awaitable = coro.send(value)
                            value = parent.switch(_AwaitRequest(awaitable))
                    except StopIteration as e:
                        return e.value

            caller_gl = gl.greenlet(actual_caller)
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

type _StackItem[**P, R, V] = tuple[object, _Handler[V] | _AsyncHandler[V], Effect[P, R], Callable[P, V]]

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


def _get_item[**P, R](
    effect: Effect[P, R],
) -> tuple[object, _Handler[Any] | _AsyncHandler[Any], Callable[P, Any]] | None:
    s = _get_stack()
    for token, h, e, f in reversed(s):
        if e is effect:
            return token, h, f
    return None


def _put_handlers[**P, R, V](
    token: object,
    handler: _Handler[V],
    effects: list[tuple[Effect[P, R], Callable[P, V]]],
) -> None:
    stack = _get_stack()
    for effect, fn in effects:
        stack.append((token, handler, effect, fn))
    _set_stack(stack)


async def _put_handlers_async[**P, R, V](
    token: object,
    handler: _AsyncHandler[V],
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
