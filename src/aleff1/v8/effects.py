from dataclasses import field, dataclass
from functools import wraps
from typing import Any, Callable, cast, overload
import greenlet as gl
from .intf import Effect, Caller, EffectNotHandledError
from .misc import debug


@overload
def effect(name: str, /) -> Effect[..., Any]: ...


@overload
def effect[V](*effects: Effect[..., Any]) -> Callable[[Caller[V]], Caller[V]]: ...


def effect[**P, R, V](*args: str | Effect[P, R]) -> Effect[..., Any] | Callable[[Caller[V]], Caller[V]]:
    """Create an effect or an effect-set decorator.

    * ``effect("name")`` — create a new :class:`Effect` with the given name.
    * ``@effect(e1, e2, ...)`` — decorate a function to declare which effects
      it uses.  The decorated function gains an ``__effects__`` attribute
      (a ``frozenset`` of effects) that can be retrieved with
      :func:`get_effects`.
    """
    if len(args) == 0:
        raise TypeError("at least one argument is required")

    if isinstance(args[0], str):
        return _Effect[..., Any](args[0])

    if not all(isinstance(arg, Effect) for arg in args):
        raise TypeError("all arguments must be Effect")

    args = cast(tuple[Effect[..., Any]], args)
    decorator = _create_effect_annotator(args)
    return decorator


def _create_effect_annotator[V](effects: tuple[Effect[..., Any]]) -> Callable[[Caller[V]], Caller[V]]:
    def decorator(fn: Caller[V]) -> Caller[V]:
        wrapped = wraps(fn)(fn)
        setattr(wrapped, "__effects__", frozenset(effects))
        return wrapped

    return decorator


@dataclass(frozen=True)
class EffectContext[**P, R]:
    effect: Effect[P, R]
    args: tuple[Any, ...] = ()
    kwargs: dict[str, Any] = field(default_factory=dict[str, Any])

    def __repr__(self) -> str:
        return f"({self.effect} | args={self.args!r}, kwargs={self.kwargs!r})"


class _Effect[**P, R](Effect[P, R]):
    def __init__(self, name: str):
        self._name = name

    @property
    def name(self) -> str:
        return self._name

    def __call__[V](self, *args: P.args, **kwargs: P.kwargs) -> R:
        debug(f"||> perform {self}")

        # pass to the main greenlet
        handler_gl = gl.getcurrent().parent
        if handler_gl is None:
            raise EffectNotHandledError(self)
        return handler_gl.switch(EffectContext(self, args, kwargs))

    def __str__(self) -> str:
        return f"<effect {self.name}>"
