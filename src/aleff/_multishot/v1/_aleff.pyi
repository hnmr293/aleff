"""Type stubs for the _aleff C extension module."""

import types
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

def snapshot_from_frame(
    frame: types.FrameType,
    depth: int = -1,
    handled_exception: object = None,
) -> FrameSnapshot[Any, Any]:
    """Capture a frame chain starting from the given frame object.

    The frame should be from a suspended greenlet (gr_frame) so that
    stacktop values are valid.

    Parameters:
        frame: A frame object (e.g. greenlet.gr_frame).
        depth: Maximum number of frames to capture. -1 for all.
        handled_exception: Active exception from the suspended caller, if any.
    """
    ...

def _snapshot_from_frame_with_adapters(
    frame: types.FrameType,
    depth: int,
    handled_exception: object,
    adapter_token: object,
) -> FrameSnapshot[Any, Any]: ...
def _suspend_adapters() -> object: ...
def _restore_adapters(token: object) -> None: ...
def _has_continuation_adapter(func: object) -> bool: ...
def _unsafe_call(func: object, args: tuple[object, ...], kwargs: dict[str, object]) -> object: ...
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

def restore_async_continuation[R, V](
    snapshot: FrameSnapshot[R, V],
    outcome: object,
    start: int = 1,
    is_exception: bool = False,
    from_coroutine: bool = False,
    replacements: dict[object, object] | None = None,
) -> tuple[bool, object, int, object, bool]:
    """Advance restored frames to the next coroutine stage or completion."""
    ...

HAS_RESTORE: int
"""1 if restore_continuation is available, 0 otherwise."""
