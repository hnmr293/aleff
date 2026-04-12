from ._aleff import HAS_RESTORE as _HAS_RESTORE

if not _HAS_RESTORE:
    raise ImportError(
        "aleff.multishot requires _PyEval_EvalFrameDefault, which is"
        " not available in this environment. Use aleff.oneshot instead."
    )

from .intf import (
    Effect,
    EffectNotHandledError,
    Resume,
    ResumeAsync,
    Handler,
    AsyncHandler,
    Ref,
)

from .effects import (
    effect,
)

from .handlers import (
    create_handler,
    create_async_handler,
)

from .utils import (
    effects,
    unhandled_effects,
)

from .misc import loglevel

from .wind import (
    WindBase,
    wind,
    wind_range,
)

__all__ = [
    "Effect",
    "EffectNotHandledError",
    "Resume",
    "ResumeAsync",
    "Handler",
    "AsyncHandler",
    "effect",
    "create_handler",
    "create_async_handler",
    "effects",
    "unhandled_effects",
    "loglevel",
    "WindBase",
    "wind",
    "wind_range",
    "Ref",
]
