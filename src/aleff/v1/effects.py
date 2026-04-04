from dataclasses import field, dataclass
from functools import wraps
from typing import Any, Callable, cast, overload, Sequence
import greenlet as gl
from .intf import Effect, EffectNotHandledError
from .misc import debug


@overload
def effect(name: str, /) -> Effect[..., Any]: ...


@overload
def effect[**P, R](*functions: Callable[..., Any]) -> Callable[[Callable[P, R]], Callable[P, R]]: ...


def effect[**P, R](*args: str | Callable[..., Any]) -> Effect[..., Any] | Callable[[Callable[P, R]], Callable[P, R]]:
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
        # create a new Effect
        return _Effect[..., Any](args[0])

    # create an effect-set decorator

    # Effect is also callable
    if not all(isinstance(arg, Callable) for arg in args):
        raise TypeError("all arguments must be callable")

    args = cast(tuple[Callable[..., Any]], args)
    decorator = _create_effect_annotator(args)
    return decorator


def _create_effect_annotator[**P, R](
    functions: tuple[Callable[..., Any]],
) -> Callable[[Callable[P, R]], Callable[P, R]]:
    def decorator(fn: Callable[P, R]) -> Callable[P, R]:
        wrapped = wraps(fn)(fn)
        setattr(wrapped, "__effects__", _flatten_effects(functions))
        return wrapped

    return decorator


def _flatten_effects(fns: Sequence[Callable[..., Any]]) -> frozenset[Effect[..., Any]]:
    effects = set[Effect[..., Any]]()
    for fn in fns:
        effects.update(_get_effects(fn))
    return frozenset(effects)


def _get_effects(fn: Callable[..., Any]) -> set[Effect[..., Any]]:
    if isinstance(fn, Effect):
        return {fn}
    return getattr(fn, "__effects__", set())


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
