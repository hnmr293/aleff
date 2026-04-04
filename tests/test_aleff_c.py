"""Tests for the _aleff C extension (frame snapshot/restore)."""

import sys

import pytest

from aleff._aleff import FrameSnapshot, snapshot_frames, snapshot_num_frames


# ---------------------------------------------------------------------------
# Module and type availability
# ---------------------------------------------------------------------------


class TestModuleAvailability:
    def test_import(self):
        import aleff._aleff  # noqa: F401  # pyright: ignore[reportUnusedImport]

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

        def recurse(n: int) -> FrameSnapshot:
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


# ---------------------------------------------------------------------------
# snapshot_frames: error cases
# ---------------------------------------------------------------------------


class TestSnapshotFramesErrors:
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
# Version check
# ---------------------------------------------------------------------------


class TestVersionRequirement:
    def test_python_312_or_later(self):
        """_aleff requires Python 3.12+."""
        assert sys.version_info >= (3, 12)
