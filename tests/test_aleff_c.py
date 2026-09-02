"""Tests for the _aleff C extension (frame snapshot/restore)."""

import sys
import subprocess
import textwrap
from typing import Any

import pytest
import greenlet

from aleff._multishot.v1._aleff import (
    _restore_adapters,  # pyright: ignore[reportPrivateUsage]
    _snapshot_from_frame_with_adapters,  # pyright: ignore[reportPrivateUsage]
    _suspend_adapters,  # pyright: ignore[reportPrivateUsage]
    FrameSnapshot,
    snapshot_frames,
    snapshot_from_frame,
    snapshot_num_frames,
    restore_async_continuation,
    restore_continuation,
    HAS_RESTORE,
)


pytestmark = pytest.mark.publish_wheel


# ---------------------------------------------------------------------------
# Module and type availability
# ---------------------------------------------------------------------------


class TestModuleAvailability:
    def test_import(self):
        import aleff._multishot.v1._aleff  # noqa: F401  # pyright: ignore[reportUnusedImport]

    def test_snapshot_type_exists(self):
        assert FrameSnapshot is not None

    def test_functions_exist(self):
        assert callable(snapshot_frames)
        assert callable(snapshot_num_frames)


# ---------------------------------------------------------------------------
# snapshot_frames: basic operation
# ---------------------------------------------------------------------------


class TestSnapshotFrames:
    def test_returns_frame_snapshot(self):
        result = snapshot_frames()
        assert isinstance(result, FrameSnapshot)

    def test_captures_at_least_one_frame(self):
        snapshot = snapshot_frames()
        assert snapshot_num_frames(snapshot) >= 1

    def test_depth_limits_frames(self):
        snapshot = snapshot_frames(1)
        assert snapshot_num_frames(snapshot) == 1

    def test_captures_nested_frames(self):
        def level2():
            return snapshot_frames()

        def level1():
            return level2()

        snapshot = level1()
        # Should capture at least level2, level1, and test method frames
        assert snapshot_num_frames(snapshot) >= 3

    def test_depth_parameter(self):
        def level3():
            return snapshot_frames(2)

        def level2():
            return level3()

        def level1():
            return level2()

        snapshot = level1()
        assert snapshot_num_frames(snapshot) == 2

    def test_depth_larger_than_frame_count(self):
        """depth exceeding actual frame count captures all available frames."""
        snapshot = snapshot_frames(10000)
        assert snapshot_num_frames(snapshot) >= 1

    def test_deep_recursion(self):
        """Snapshot works inside deep recursion."""

        def recurse(n: int) -> FrameSnapshot[Any, Any]:
            if n == 0:
                return snapshot_frames()
            return recurse(n - 1)

        snapshot = recurse(50)
        assert snapshot_num_frames(snapshot) >= 50

    def test_snapshot_same_point_twice(self):
        """Two snapshots from the same call site are both valid."""
        s1 = snapshot_frames(1)
        s2 = snapshot_frames(1)
        assert snapshot_num_frames(s1) == 1
        assert snapshot_num_frames(s2) == 1
        assert s1 is not s2


class TestSnapshotFromFrame:
    def test_restore_releases_owner_frame_after_success(self):
        """A restored token must not keep its suspend frame alive."""
        import gc

        def suspend_and_restore() -> None:
            token = _suspend_adapters()
            _restore_adapters(token)

        gc.collect()
        refs_before = (
            sys.getrefcount(suspend_and_restore),
            sys.getrefcount(suspend_and_restore.__code__),
        )

        for _ in range(5):
            suspend_and_restore()
        gc.collect()

        refs_after = (
            sys.getrefcount(suspend_and_restore),
            sys.getrefcount(suspend_and_restore.__code__),
        )
        assert refs_after == refs_before

    @pytest.mark.parametrize("handled_exception", [None, object()])
    def test_non_exception_marker_is_treated_as_no_handled_exception(self, handled_exception: object):
        parent = greenlet.getcurrent()

        def suspend():
            parent.switch()

        child = greenlet.greenlet(suspend)
        child.switch()
        assert child.gr_frame is not None

        snapshot = snapshot_from_frame(child.gr_frame, 1, handled_exception)

        assert snapshot_num_frames(snapshot) == 1

    def test_rejects_internal_send_metadata_argument(self):
        parent = greenlet.getcurrent()

        def suspend():
            parent.switch()

        child = greenlet.greenlet(suspend)
        child.switch()
        assert child.gr_frame is not None

        with pytest.raises(TypeError, match="function takes at most 3 arguments"):
            snapshot_from_frame(child.gr_frame, 1, None, [(2, 2147483646)])  # type: ignore[call-arg]

    def test_private_adapter_snapshot_accepts_suspension_token(self):
        parent = greenlet.getcurrent()

        def suspend():
            token = _suspend_adapters()
            try:
                parent.switch(token)
            finally:
                _restore_adapters(token)

        child = greenlet.greenlet(suspend)
        token = child.switch()
        assert child.gr_frame is not None

        snapshot = _snapshot_from_frame_with_adapters(child.gr_frame, 1, None, token)

        assert snapshot_num_frames(snapshot) == 1
        child.switch()

    def test_private_adapter_snapshot_rejects_non_token(self):
        parent = greenlet.getcurrent()

        def suspend():
            parent.switch()

        child = greenlet.greenlet(suspend)
        child.switch()
        assert child.gr_frame is not None

        with pytest.raises(ValueError, match="invalid PyCapsule"):
            _snapshot_from_frame_with_adapters(child.gr_frame, 1, None, object())

    def test_restore_rejects_a_token_from_another_greenlet(self):
        parent = greenlet.getcurrent()

        def suspend():
            token = _suspend_adapters()
            try:
                parent.switch(token)
            finally:
                _restore_adapters(token)

        child = greenlet.greenlet(suspend)
        token = child.switch()

        with pytest.raises(RuntimeError, match="owner"):
            _restore_adapters(token)

        child.switch()

    def test_restore_rejects_a_token_after_owner_greenlet_finished(self):
        parent = greenlet.getcurrent()

        def suspend():
            parent.switch(_suspend_adapters())

        child = greenlet.greenlet(suspend)
        token = child.switch()
        child.switch()
        assert child.dead

        with pytest.raises(RuntimeError, match="owner"):
            _restore_adapters(token)

    def test_restore_rejects_double_restore(self):
        parent = greenlet.getcurrent()

        def suspend():
            token = _suspend_adapters()
            parent.switch(token)
            _restore_adapters(token)
            with pytest.raises(RuntimeError, match="already restored"):
                _restore_adapters(token)
            parent.switch()

        child = greenlet.greenlet(suspend)
        child.switch()
        child.switch()
        child.switch()

    def test_nested_tokens_restore_in_their_owner(self):
        parent = greenlet.getcurrent()

        def suspend():
            outer = _suspend_adapters()
            inner = _suspend_adapters()
            parent.switch()
            _restore_adapters(inner)
            _restore_adapters(outer)

        child = greenlet.greenlet(suspend)
        child.switch()
        child.switch()
        assert child.dead

    def test_restore_rejects_non_token(self):
        with pytest.raises((TypeError, ValueError)):
            _restore_adapters(object())

    def test_send_stack_depth_does_not_depend_on_importable_python_analyzer(self):
        code = textwrap.dedent(
            """
            import dis
            import sys
            import types

            analyzer = types.ModuleType("aleff._multishot.v1._send_metadata")

            def malicious_metadata(frame):
                current = next(
                    (
                        instruction
                        for instruction in dis.get_instructions(frame.f_code)
                        if instruction.offset == frame.f_lasti
                    ),
                    None,
                )
                if current is None or current.opname != "SEND":
                    return False, 0, 0
                return True, frame.f_code.co_stacksize, current.argval

            analyzer.send_stack_metadata = malicious_metadata
            sys.modules[analyzer.__name__] = analyzer

            import asyncio
            from aleff import create_async_handler, effect

            choose = effect("choose")
            handler = create_async_handler(choose)

            @handler.on(choose)
            async def handle_choose(k):
                return await k(1) + await k(2)

            async def values():
                yield choose()

            async def run():
                try:
                    raise ValueError("enter exception handler")
                except ValueError:
                    iterator = values()
                    return [10, 20, await anext(iterator)]

            assert asyncio.run(handler(run)) == [10, 20, 1, 10, 20, 2]
            """
        )

        result = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)

        assert result.returncode == 0, result.stderr


# ---------------------------------------------------------------------------
# snapshot_frames: error cases
# ---------------------------------------------------------------------------


class TestSnapshotFramesErrors:
    def test_released_memoryview_clone_failure_releases_prior_local_references(self):
        import gc

        class Sentinel:
            pass

        sentinel = Sentinel()
        released_view = memoryview(b"released")
        released_view.release()

        def capture(current_sentinel: Sentinel, current_view: memoryview) -> None:
            assert current_sentinel is sentinel
            assert current_view is released_view
            snapshot_frames(1)

        gc.collect()
        references_before = sys.getrefcount(sentinel)

        for _ in range(3):
            with pytest.raises(ValueError, match="released memoryview"):
                capture(sentinel, released_view)

        gc.collect()
        assert sys.getrefcount(sentinel) == references_before

    def test_depth_zero_raises(self):
        """depth=0 means no frames — should raise."""
        with pytest.raises(RuntimeError):
            snapshot_frames(0)

    def test_invalid_depth_type_raises(self):
        """Non-integer depth raises TypeError."""
        with pytest.raises(TypeError):
            snapshot_frames("abc")  # type: ignore

    def test_invalid_depth_type_float_raises(self):
        with pytest.raises(TypeError):
            snapshot_frames(1.5)  # type: ignore


# ---------------------------------------------------------------------------
# snapshot_num_frames: type checking
# ---------------------------------------------------------------------------


class TestSnapshotNumFrames:
    def test_rejects_non_snapshot(self):
        with pytest.raises(TypeError):
            snapshot_num_frames("not a snapshot")  # type: ignore

    def test_rejects_none(self):
        with pytest.raises(TypeError):
            snapshot_num_frames(None)  # type: ignore


# ---------------------------------------------------------------------------
# FrameSnapshot: lifecycle
# ---------------------------------------------------------------------------


class TestSnapshotLifecycle:
    def test_snapshot_is_not_garbage_collected_prematurely(self):
        """Snapshot should hold strong refs and survive garbage collection."""
        import gc

        def create():
            return snapshot_frames()

        snapshot = create()
        gc.collect()
        # Should not segfault
        assert snapshot_num_frames(snapshot) >= 1

    def test_multiple_snapshots_independent(self):
        """Multiple snapshots from different points are independent."""

        def a():
            return snapshot_frames(1)

        def b():
            return snapshot_frames(1)

        s1 = a()
        s2 = b()
        assert isinstance(s1, FrameSnapshot)
        assert isinstance(s2, FrameSnapshot)
        assert s1 is not s2

    def test_snapshot_not_constructible(self):
        """FrameSnapshot cannot be directly instantiated."""
        with pytest.raises(TypeError):
            FrameSnapshot()


# ---------------------------------------------------------------------------
# restore_continuation: argument validation
# ---------------------------------------------------------------------------


class TestRestoreContinuationErrors:
    def test_rejects_non_snapshot(self):
        with pytest.raises(TypeError):
            restore_continuation("not a snapshot", 42)  # type: ignore

    def test_rejects_missing_value(self):
        with pytest.raises(TypeError):
            restore_continuation()  # type: ignore

    def test_skip_too_large_raises(self):
        """skip >= num_frames means no frames to restore."""
        snapshot = snapshot_frames(1)
        n = snapshot_num_frames(snapshot)
        with pytest.raises(ValueError):
            restore_continuation(snapshot, 42, n)

    def test_has_restore_flag(self):
        """HAS_RESTORE indicates _PyEval_EvalFrameDefault availability."""
        assert isinstance(HAS_RESTORE, int)

    def test_has_restore_is_true(self):
        """_PyEval_EvalFrameDefault must be found on all supported platforms."""
        assert HAS_RESTORE == 1, (
            "_PyEval_EvalFrameDefault was not found; restore_continuation will not work on this platform"
        )


class TestRestoreAsyncContinuationErrors:
    def test_negative_start_raises(self):
        snapshot = snapshot_frames(1)

        with pytest.raises(ValueError, match="invalid async continuation frame index"):
            restore_async_continuation(snapshot, 42, -1)

    def test_start_past_frame_count_raises(self):
        snapshot = snapshot_frames(1)
        frame_count = snapshot_num_frames(snapshot)

        with pytest.raises(ValueError, match="invalid async continuation frame index"):
            restore_async_continuation(snapshot, 42, frame_count + 1)

    def test_start_at_frame_count_returns_completed_outcome(self):
        snapshot = snapshot_frames(1)
        frame_count = snapshot_num_frames(snapshot)

        done, payload, next_frame, initial, is_exception = restore_async_continuation(snapshot, 42, frame_count)

        assert done is True
        assert payload == 42
        assert next_frame == frame_count
        assert initial is None
        assert is_exception is False

    def test_rejects_non_dict_replacements(self):
        snapshot = snapshot_frames(1)
        frame_count = snapshot_num_frames(snapshot)

        with pytest.raises(TypeError, match="replacements must be a dict or None"):
            restore_async_continuation(snapshot, 42, frame_count, False, False, [])  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# restore_continuation: greenlet integration
#
# These tests use greenlet directly (not aleff handlers) to verify
# that restore_continuation correctly resumes a snapshotted frame chain.
# ---------------------------------------------------------------------------


class TestRestoreContinuationGreenlet:
    """Test restore_continuation using greenlet + manual perform/resume pattern.

    The pattern:
      1. A greenlet runs user code that calls `perform()`
      2. `perform()` snapshots frames and switches back to the main greenlet
      3. The main greenlet calls `restore_continuation(snapshot, value)` in a
         new greenlet to resume the computation with `value`
    """

    @staticmethod
    def _perform(tag: str) -> Any:
        """Simulate an effect: snapshot frames and switch to parent.

        From the main greenlet's perspective, this returns (tag, snapshot).
        From the caller's perspective (after resume), this returns the resume value.
        """
        snapshot = snapshot_frames()
        main_gl = greenlet.getcurrent().parent
        assert main_gl is not None
        # Switch to main with (tag, snapshot); main will resume us or create a new greenlet
        return main_gl.switch((tag, snapshot))

    def test_basic_resume(self):
        """restore_continuation resumes from a snapshot with a value."""
        assert HAS_RESTORE, "_PyEval_EvalFrameDefault not available"

        def user_code() -> str:
            val: str = self._perform("get")
            return f"got:{val}"

        # Run in a greenlet
        gl = greenlet.greenlet(user_code)
        tag, snapshot = gl.switch()
        assert tag == "get"

        # First resume: normal greenlet switch (one-shot)
        result = gl.switch("hello")
        assert result == "got:hello"

        # Second resume: restore_continuation in a new greenlet
        def resume_body() -> str:
            return restore_continuation(snapshot, "world")

        gl2 = greenlet.greenlet(resume_body)
        result2 = gl2.switch()
        assert result2 == "got:world"

    def test_resume_multiple_times(self):
        """Same snapshot can be restored multiple times with different values."""
        assert HAS_RESTORE, "_PyEval_EvalFrameDefault not available"

        def user_code() -> int:
            x: int = self._perform("choose")
            return x * 10

        gl = greenlet.greenlet(user_code)
        tag, snapshot = gl.switch()
        assert tag == "choose"

        # First: one-shot
        result1 = gl.switch(1)
        assert result1 == 10

        # Multiple restores from same snapshot
        results: list[int] = []
        for v in [2, 3, 4]:

            def body(val: int = v) -> int:
                return restore_continuation(snapshot, val)

            g = greenlet.greenlet(body)
            results.append(g.switch())

        assert results == [20, 30, 40]

    def test_resume_with_nested_calls(self):
        """restore_continuation works when perform is called through nested functions."""
        assert HAS_RESTORE, "_PyEval_EvalFrameDefault not available"

        def inner() -> int:
            return self._perform("val")

        def middle() -> int:
            return inner() + 100

        def outer() -> int:
            return middle() + 1000

        gl = greenlet.greenlet(outer)
        _tag, snapshot = gl.switch()

        # One-shot
        result1 = gl.switch(5)
        assert result1 == 1105

        # Multi-shot
        def body() -> int:
            return restore_continuation(snapshot, 9)

        gl2 = greenlet.greenlet(body)
        result2 = gl2.switch()
        assert result2 == 1109

    def test_local_mutable_state_is_shared(self):
        """Mutable objects in locals are shared (Scheme continuation semantics).

        The frame copy shares the same list object, so mutations accumulate.
        This matches Scheme's call/cc behavior: continuations share the heap.
        """
        assert HAS_RESTORE, "_PyEval_EvalFrameDefault not available"

        def user_code() -> list[int]:
            items: list[int] = []
            v: int = self._perform("choose")
            items.append(v)
            return items

        gl = greenlet.greenlet(user_code)
        _tag, snapshot = gl.switch()

        result1 = gl.switch(1)
        assert result1 == [1]

        def body(val: int) -> list[int]:
            return restore_continuation(snapshot, val)

        gl2 = greenlet.greenlet(lambda: body(2))
        result2 = gl2.switch()
        assert result2 == [1, 2]  # shared list

        gl3 = greenlet.greenlet(lambda: body(3))
        result3 = gl3.switch()
        assert result3 == [1, 2, 3]  # continues accumulating

    def test_heap_shared_between_shots(self):
        """Objects from outside the continuation are shared (Scheme semantics)."""
        assert HAS_RESTORE, "_PyEval_EvalFrameDefault not available"

        shared: list[int] = []

        def user_code() -> list[int]:
            v: int = self._perform("choose")
            shared.append(v)
            return list(shared)

        gl = greenlet.greenlet(user_code)
        _tag, snapshot = gl.switch()

        result1 = gl.switch(1)
        assert result1 == [1]

        def body() -> list[int]:
            return restore_continuation(snapshot, 2)

        gl2 = greenlet.greenlet(body)
        result2 = gl2.switch()
        # shared is outside the continuation, so mutations accumulate
        assert result2 == [1, 2]

    def test_exception_in_restored_continuation(self):
        """Exceptions in restored continuations propagate normally."""
        assert HAS_RESTORE, "_PyEval_EvalFrameDefault not available"

        def user_code() -> int:
            v: int = self._perform("choose")
            if v < 0:
                raise ValueError("negative")
            return v

        gl = greenlet.greenlet(user_code)
        _tag, snapshot = gl.switch()

        # Normal
        result = gl.switch(42)
        assert result == 42

        # Exception in restore
        def body() -> int:
            return restore_continuation(snapshot, -1)

        gl2 = greenlet.greenlet(body)
        with pytest.raises(ValueError, match="negative"):
            gl2.switch()


# ---------------------------------------------------------------------------
# Version check
# ---------------------------------------------------------------------------


class TestVersionRequirement:
    def test_python_312_or_later(self):
        """_aleff requires Python 3.12+."""
        assert sys.version_info >= (3, 12)
