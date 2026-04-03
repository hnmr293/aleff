from typing import Callable, Any
from .intf import Effect, handler, async_handler


def effects(fn: Callable[..., Any]) -> frozenset[Effect[..., Any]]:
    """Return the set of effects used by the given function."""

    return getattr(fn, "__effects__", frozenset())


def unhandled_effects[V](
    fn: Callable[..., Any],
    *handlers: handler[V] | async_handler[V],
) -> frozenset[Effect[..., Any]]:
    """Return the set of effects used by *fn* that are not handled by *handlers*."""

    fn_effects = effects(fn)

    handled_effects: set[Effect[..., Any]] = set()
    for h in handlers:
        handled_effects.update(h.effects)

    return fn_effects - handled_effects
