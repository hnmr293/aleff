"""Tests for wind_range context manager.

wind_range provides multi-shot-safe range iteration by saving and
restoring the iterator position via the wind snapshot/restore mechanism.

Multi-shot tests use ``v = choose(); choices = choices + (v,)`` instead of
``choices = (*choices, choose())``.  The latter creates an intermediate
mutable list on the heap that is shared across shots.
"""

import pytest

from aleff import (
    effect,
    Effect,
    Resume,
    Handler,
    create_handler,
    wind,
    wind_range,
)
from aleff._multishot.v1.wind import _get_wind_stack  # pyright: ignore[reportPrivateUsage]


# ---------------------------------------------------------------------------
# Basic (no effects)
# ---------------------------------------------------------------------------


class TestWindRangeBasic:
    def test_iterates_correctly(self):
        """wind_range produces the same values as range()."""
        result: list[int] = []
        with wind_range(5) as r:
            for i in r:
                result.append(i)
        assert result == [0, 1, 2, 3, 4]

    def test_empty_range(self):
        """wind_range(0) produces no values."""
        result: list[int] = []
        with wind_range(0) as r:
            for i in r:
                result.append(i)
        assert result == []

    def test_range_one(self):
        """wind_range(1) produces [0]."""
        result: list[int] = []
        with wind_range(1) as r:
            for i in r:
                result.append(i)
        assert result == [0]

    def test_nested_wind_range(self):
        """Nested wind_range loops."""
        result: list[tuple[int, int]] = []
        with wind_range(3) as outer:
            for i in outer:
                with wind_range(2) as inner:
                    for j in inner:
                        result.append((i, j))
        assert result == [(0, 0), (0, 1), (1, 0), (1, 1), (2, 0), (2, 1)]

    def test_no_effects_behaves_like_range(self):
        """Without effects, wind_range behaves identically to range."""
        total = 0
        with wind_range(10) as r:
            for i in r:
                total += i
        assert total == sum(range(10))

    # -- start, stop, step interface --

    def test_start_stop(self):
        """wind_range(start, stop) produces values from start to stop-1."""
        result: list[int] = []
        with wind_range(2, 6) as r:
            for i in r:
                result.append(i)
        assert result == list(range(2, 6))

    def test_start_stop_step(self):
        """wind_range(start, stop, step) produces values with stride."""
        result: list[int] = []
        with wind_range(1, 10, 3) as r:
            for i in r:
                result.append(i)
        assert result == list(range(1, 10, 3))

    def test_negative_step(self):
        """wind_range with negative step counts downward."""
        result: list[int] = []
        with wind_range(10, 0, -2) as r:
            for i in r:
                result.append(i)
        assert result == list(range(10, 0, -2))

    def test_step_positive_empty(self):
        """wind_range(5, 2) with positive step produces no values."""
        result: list[int] = []
        with wind_range(5, 2) as r:
            for i in r:
                result.append(i)
        assert result == []

    def test_step_negative_empty(self):
        """wind_range(2, 5, -1) produces no values."""
        result: list[int] = []
        with wind_range(2, 5, -1) as r:
            for i in r:
                result.append(i)
        assert result == []

    def test_step_zero_raises(self):
        """wind_range with step=0 raises ValueError like range()."""
        with pytest.raises(ValueError):
            wind_range(0, 10, 0)

    def test_single_arg_matches_range(self):
        """wind_range(n) matches range(n) for various n."""
        for n in [0, 1, 5, 10]:
            result: list[int] = []
            with wind_range(n) as r:
                for i in r:
                    result.append(i)
            assert result == list(range(n))

    def test_two_arg_matches_range(self):
        """wind_range(a, b) matches range(a, b) for various a, b."""
        cases = [(0, 5), (3, 3), (5, 2), (-3, 3), (0, 0)]
        for a, b in cases:
            result: list[int] = []
            with wind_range(a, b) as r:
                for i in r:
                    result.append(i)
            assert result == list(range(a, b)), f"wind_range({a}, {b})"

    def test_three_arg_matches_range(self):
        """wind_range(a, b, c) matches range(a, b, c) for various a, b, c."""
        cases = [(0, 10, 2), (10, 0, -1), (0, 10, 3), (-5, 5, 2), (5, -5, -3)]
        for a, b, c in cases:
            result: list[int] = []
            with wind_range(a, b, c) as r:
                for i in r:
                    result.append(i)
            assert result == list(range(a, b, c)), f"wind_range({a}, {b}, {c})"


# ---------------------------------------------------------------------------
# Multi-shot
# ---------------------------------------------------------------------------


class TestWindRangeMultiShot:
    def test_cartesian_product_2(self):
        """choose() per iteration with wind_range(2) produces 2^2 = 4 results."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, ...]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, ...]]]):
            return k(0) + k(1)

        def run() -> list[tuple[int, ...]]:
            choices: tuple[int, ...] = ()
            with wind_range(2) as r:
                for _ in r:
                    v = choose()
                    choices = choices + (v,)
            return [choices]

        result = h(run)
        # Cartesian product {0,1}^2, depth-first order
        assert result == [(0, 0), (0, 1), (1, 0), (1, 1)]

    def test_cartesian_product_3(self):
        """choose() per iteration with wind_range(3) produces 2^3 = 8 results."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, ...]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, ...]]]):
            return k(0) + k(1)

        def run() -> list[tuple[int, ...]]:
            choices: tuple[int, ...] = ()
            with wind_range(3) as r:
                for _ in r:
                    v = choose()
                    choices = choices + (v,)
            return [choices]

        result = h(run)
        assert result == [
            (0, 0, 0),
            (0, 0, 1),
            (0, 1, 0),
            (0, 1, 1),
            (1, 0, 0),
            (1, 0, 1),
            (1, 1, 0),
            (1, 1, 1),
        ]

    def test_single_effect_mid_loop(self):
        """Single choose() at a specific iteration — loop continues correctly."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(100) + k(200)

        def run() -> list[int]:
            total = 0
            with wind_range(4) as r:
                for i in r:
                    if i == 2:
                        total += choose()
                    else:
                        total += i
            return [total]

        result = h(run)
        # At i=2: snapshot, total=0+1=1 (immutable int, frame-local)
        # Shot 1: 1 + 100 + 3 = 104
        # Shot 2: restore pos=3, total=1, 1 + 200 + 3 = 204
        assert result == [104, 204]

    def test_effect_before_loop(self):
        """Effect before the loop — wind_range restores pos for each shot."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(10) + k(20)

        def run() -> list[int]:
            total = 0
            with wind_range(3) as r:
                base = choose()
                total = base
                for i in r:
                    total += i
            return [total]

        result = h(run)
        # Shot 1: base=10, loop i=0,1,2 → 10+0+1+2 = 13
        # Shot 2: restore pos=0, base=20 → 20+0+1+2 = 23
        assert result == [13, 23]

    def test_three_way_multishot(self):
        """3-way multi-shot with wind_range(2) produces 3^2 = 9 results."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, ...]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, ...]]]):
            return k(0) + k(1) + k(2)

        def run() -> list[tuple[int, ...]]:
            choices: tuple[int, ...] = ()
            with wind_range(2) as r:
                for _ in r:
                    v = choose()
                    choices = choices + (v,)
            return [choices]

        result = h(run)
        assert result == [
            (0, 0),
            (0, 1),
            (0, 2),
            (1, 0),
            (1, 1),
            (1, 2),
            (2, 0),
            (2, 1),
            (2, 2),
        ]

    def test_start_stop_multishot(self):
        """Multi-shot with wind_range(start, stop) restores position correctly."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(100) + k(200)

        def run() -> list[int]:
            total = 0
            with wind_range(5, 8) as r:
                for i in r:
                    if i == 6:
                        total += choose()
                    else:
                        total += i
            return [total]

        result = h(run)
        # range(5,8) = [5,6,7]. choose at i=6, total=5 at that point.
        # Shot 1: 5 + 100 + 7 = 112
        # Shot 2: 5 + 200 + 7 = 212
        assert result == [112, 212]

    def test_step_multishot(self):
        """Multi-shot with wind_range(start, stop, step) restores correctly."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, ...]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, ...]]]):
            return k(0) + k(1)

        def run() -> list[tuple[int, ...]]:
            choices: tuple[int, ...] = ()
            with wind_range(0, 6, 2) as r:
                for _ in r:
                    v = choose()
                    choices = choices + (v,)
            return [choices]

        result = h(run)
        # range(0,6,2) = [0,2,4], 3 iterations, 2^3 = 8 leaves
        assert result == [
            (0, 0, 0), (0, 0, 1), (0, 1, 0), (0, 1, 1),
            (1, 0, 0), (1, 0, 1), (1, 1, 0), (1, 1, 1),
        ]

    def test_loop_index_available(self):
        """Loop index from wind_range is correctly captured per shot."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, int]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, int]]]):
            return k(100) + k(200)

        def run() -> list[tuple[int, int]]:
            with wind_range(3) as r:
                for i in r:
                    v = choose()
                    return [(i, v)]
            assert False, "unreachable"

        result = h(run)
        # return on first iteration, so i=0 always
        assert result == [(0, 100), (0, 200)]

    def test_immutable_total_per_shot(self):
        """Immutable accumulator (int) is independent per shot."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(10) + k(20)

        def run() -> list[int]:
            total = 0
            with wind_range(3) as r:
                for _ in r:
                    v = choose()
                    total = total + v
            return [total]

        result = h(run)
        # 3 iterations, each choose() forks 2 ways → 2^3 = 8 leaves
        # Each leaf is a sum of 3 choices from {10, 20}
        assert sorted(result) == sorted(
            [
                10 + 10 + 10,  # 30
                10 + 10 + 20,  # 40
                10 + 20 + 10,  # 40
                10 + 20 + 20,  # 50
                20 + 10 + 10,  # 40
                20 + 10 + 20,  # 50
                20 + 20 + 10,  # 50
                20 + 20 + 20,  # 60
            ]
        )


# ---------------------------------------------------------------------------
# Stack cleanup
# ---------------------------------------------------------------------------


class TestWindRangeStackCleanup:
    def test_stack_empty_after_normal_exit(self):
        """Wind stack is empty after wind_range exits normally."""
        with wind_range(5) as r:
            for _ in r:
                pass
        assert _get_wind_stack() == []

    def test_stack_empty_after_multishot(self):
        """Wind stack is empty after multi-shot with wind_range completes."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        def run() -> list[int]:
            with wind_range(3) as r:
                for i in r:
                    return [choose() + i]
            assert False, "unreachable"

        h(run)
        assert _get_wind_stack() == []


# ---------------------------------------------------------------------------
# Corner cases
# ---------------------------------------------------------------------------


class TestWindRangeCornerCases:
    def test_abort(self):
        """Handler abort with wind_range still cleans up."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return -1  # abort

        def run() -> int:
            with wind_range(5) as r:
                for i in r:
                    if i == 2:
                        return e()
            return 0

        result = h(run, check=False)
        assert result == -1
        assert _get_wind_stack() == []

    def test_exception_in_body(self):
        """Exception in loop body propagates and cleans up."""
        with pytest.raises(ValueError, match="boom"):
            with wind_range(5) as r:
                for i in r:
                    if i == 3:
                        raise ValueError("boom")
        assert _get_wind_stack() == []

    def test_wind_and_wind_range_combined(self):
        """wind and wind_range can be nested together."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(10) + k(20)

        log: list[str] = []

        def run() -> list[int]:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                with wind_range(2) as r:
                    for i in r:
                        return [choose() + i]
            assert False, "unreachable"

        h(run)
        assert log.count("before") == 2
        assert log.count("after") == 2
        assert _get_wind_stack() == []
