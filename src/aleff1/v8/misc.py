from logging import getLogger


_logger = getLogger(__name__)


def loglevel(level: int):
    """Set the log level for internal debug messages."""
    _logger.setLevel(level)


def debug(msg: str):
    _logger.debug(msg)
