from logging import getLogger
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .intf import Effect

_logger = getLogger(__name__)


def loglevel(level: int):
    """Set the log level for internal debug messages."""
    _logger.setLevel(level)


def debug(msg: str):
    _logger.debug(msg)


def eff_str(e: "Effect[..., Any]") -> str:
    """Format an effect for display (e.g. ``<effect choose>``)."""
    return f"<effect {e.name}>"
