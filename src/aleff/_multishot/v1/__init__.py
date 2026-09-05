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

from .winds import (
    WindBase,
    wind,
    wind_range,
)

from .unsafe import aleffy

from .monitoring import (
    CFrameContinuationWarning,
    c_warnings_enabled,
    disable_c_warnings,
    enable_c_warnings,
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
    "aleffy",
    "CFrameContinuationWarning",
    "c_warnings_enabled",
    "disable_c_warnings",
    "enable_c_warnings",
]
