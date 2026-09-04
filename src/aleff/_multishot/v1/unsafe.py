from collections.abc import Callable
from functools import wraps
from typing import cast

from ._aleff import _has_continuation_adapter, _unsafe_call  # pyright: ignore[reportPrivateUsage]


def _is_ctypes_callable(value: object) -> bool:
    try:
        from _ctypes import CFuncPtr
    except ImportError:
        return False
    return isinstance(value, CFuncPtr)


def aleffy[**P, R](func: Callable[P, R], /) -> Callable[P, R]:
    """Opt a callable into the experimental unsafe C-extension boundary.

    The current backend requires GIL-enabled CPython 3.12 through 3.14 on Linux
    x86-64. ``ctypes`` function pointers are unsupported. Native resources that
    remain live across an effect must be safe for multi-shot stack copying.
    """

    if not callable(func):
        raise TypeError("aleffy requires a callable")
    if _is_ctypes_callable(func):
        raise TypeError("aleffy does not support ctypes callables")
    if _has_continuation_adapter(func):
        return func

    @wraps(func)
    def wrapped(*args: P.args, **kwargs: P.kwargs) -> R:
        return cast(R, _unsafe_call(func, args, kwargs))

    return wrapped
