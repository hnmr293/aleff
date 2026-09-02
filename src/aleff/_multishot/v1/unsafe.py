from collections.abc import Callable
from functools import wraps
from typing import cast

from ._aleff import _unsafe_call  # pyright: ignore[reportPrivateUsage]


def aleffy[**P, R](func: Callable[P, R], /) -> Callable[P, R]:
    """Opt a callable into the experimental unsafe C-continuation boundary."""

    if not callable(func):
        raise TypeError("aleffy requires a callable")

    @wraps(func)
    def wrapped(*args: P.args, **kwargs: P.kwargs) -> R:
        return cast(R, _unsafe_call(func, args, kwargs))

    return wrapped
