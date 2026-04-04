"""Type stubs for the _aleff C extension module."""

from typing import Any

class FrameSnapshot[R, V]:
    """Snapshot of a Python frame chain for multi-shot continuations.

    Type parameters:
        R: The type of the resume value (injected at restoration).
        V: The type of the computation's final result.
    """
    ...

def snapshot_frames(depth: int = -1) -> FrameSnapshot[Any, Any]:
    """Capture the current Python frame chain as a FrameSnapshot.

    Parameters:
        depth: Maximum number of frames to capture. -1 for all frames.

    Returns:
        A FrameSnapshot containing deep copies of the captured frames.

    Raises:
        RuntimeError: If depth is 0 or no frames are available.
    """
    ...

def snapshot_num_frames[R, V](snapshot: FrameSnapshot[R, V]) -> int:
    """Return the number of frames in a FrameSnapshot.

    Raises:
        TypeError: If snapshot is not a FrameSnapshot.
    """
    ...

def restore_continuation[R, V](snapshot: FrameSnapshot[R, V], value: R, skip: int = 1) -> V:
    """Restore a continuation from a FrameSnapshot and resume execution.

    Creates a fresh copy of the frame chain from the snapshot,
    injects `value` as the return value of the effect call,
    and resumes execution via _PyEval_EvalFrameDefault.

    Parameters:
        snapshot: A FrameSnapshot object.
        value: The value to resume the continuation with.
        skip: Number of innermost frames to skip (default 1).

    Returns:
        The result of the resumed computation.

    Raises:
        RuntimeError: If _PyEval_EvalFrameDefault is not available.
        ValueError: If no frames remain after skipping.
    """
    ...

HAS_RESTORE: int
"""1 if restore_continuation is available, 0 otherwise."""
