from typing import Any, Callable, Concatenate, Coroutine, Protocol, runtime_checkable


##
# type notations:
#   P: effect parameters
#   R: effect return type
#   V: handler return type
##


@runtime_checkable
class Effect[**P, R](Protocol):
    """An algebraic effect declaration.

    Effects are created via the ``effect()`` factory and invoked like regular
    function calls.  A handler intercepts these calls and provides the
    implementation at runtime.
    """

    @property
    def name(self) -> str: ...

    def __call__[V](self, *args: P.args, **kwargs: P.kwargs) -> R: ...


@runtime_checkable
class Resume[R, V](Protocol):
    """Continuation passed to synchronous effect handlers.

    Calling ``k(value)`` resumes the suspended computation with *value* and
    drives it to completion (or to the next effect).  The return value of
    ``k()`` is the final result of the handled computation.
    """

    def __call__(self, value: R) -> V: ...


@runtime_checkable
class ResumeAsync[R, V](Protocol):
    """Continuation passed to asynchronous effect handlers.

    Same semantics as :class:`Resume`, but ``await k(value)`` is required
    because the handler function is ``async def``.
    """

    async def __call__(self, value: R) -> V: ...


type Caller[V] = Callable[[], V]
type AsyncCaller[V] = Coroutine[Any, Any, V]
type EffectHandler[**P, V, R] = Callable[Concatenate[Resume[R, V], P], V]
type AsyncEffectHandler[**P, V, R] = Callable[Concatenate[ResumeAsync[R, V], P], AsyncCaller[V]]


class Handler[V](Protocol):
    """Protocol for synchronous effect handlers.

    Create instances via :func:`create_handler`.  Register effect
    implementations with :meth:`on`, then invoke the handler with a
    caller function::

        h = create_handler(read, write)

        @h.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        result = h(lambda: read())
    """

    @property
    def effects(self) -> frozenset[Effect[..., Any]]:
        """The effects declared for this handler."""
        ...

    @property
    def shallow(self) -> bool:
        """Whether this is a shallow handler."""
        ...

    def on[**P, R](self, effect: Effect[P, R]) -> Callable[[EffectHandler[P, V, R]], EffectHandler[P, V, R]]:
        """Register a handler function for *effect*.  Returns a decorator."""
        ...

    def check(self, caller: Caller[V]) -> None:
        """Raise ``ValueError`` if any declared effect has no registered handler."""
        ...

    def __call__(self, caller: Caller[V], *, check: bool = True) -> V:
        """Run *caller* with the registered effect handlers active."""
        ...


class AsyncHandler[V](Protocol):
    """Protocol for asynchronous effect handlers.

    Create instances via :func:`create_async_handler`.  Handler functions
    are ``async def`` and receive a :class:`ResumeAsync` continuation::

        h = create_async_handler(read)

        @h.on(read)
        async def _read(k: ResumeAsync[str, int]):
            return await k("data")

        result = await h(lambda: read())
    """

    @property
    def effects(self) -> frozenset[Effect[..., Any]]:
        """The effects declared for this handler."""
        ...

    @property
    def shallow(self) -> bool:
        """Whether this is a shallow handler."""
        ...

    def on[**P, R](
        self,
        effect: Effect[P, R],
    ) -> Callable[[AsyncEffectHandler[P, V, R]], AsyncEffectHandler[P, V, R]]:
        """Register an async handler function for *effect*.  Returns a decorator."""
        ...

    def check(self, caller: Caller[V | Coroutine[Any, Any, V]]) -> None:
        """Raise ``ValueError`` if any declared effect has no registered handler."""
        ...

    async def __call__(self, caller: Caller[V | Coroutine[Any, Any, V]], *, check: bool = True) -> V:
        """Run *caller* with the registered async effect handlers active."""
        ...


class EffectNotHandledError[**P, R](RuntimeError):
    """Raised when an effect is invoked but no handler is active for it."""

    def __init__(self, effect: Effect[P, R]):
        from .misc import eff_str

        super().__init__(f"no handler for the effect: {eff_str(effect)}")
        self.effect = effect
