import warnings as _warnings
from importlib.metadata import version

__version__ = version("aleff")

try:
    from .multishot import *  # noqa: F403
except ImportError:
    _warnings.warn(
        "aleff.multishot is not available in this environment.\nUse 'import aleff.oneshot' for one-shot handlers.",
        ImportWarning,
        stacklevel=1,
    )
