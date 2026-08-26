"""Tests for wind context manager.

wind(before, after, *, auto_exit=True) establishes a dynamic extent guard:
- Entering the with block calls before() and pushes a wind entry
- Exiting the with block pops the wind entry, calls cm.__exit__ if applicable, calls after()
- On multi-shot resume, before() is called again before re-entering the extent
- The return value of before() is wrapped in a Ref for multi-shot safety
"""

from contextlib import contextmanager
from typing import Any, Iterator

import pytest

from aleff import (
    effect,
    Effect,
    Resume,
    ResumeAsync,
    Handler,
    AsyncHandler,
    create_handler,
    create_async_handler,
    wind,
    wind_range,
    Ref,
)
from aleff._multishot.v1.winds import _get_wind_stack  # pyright: ignore[reportPrivateUsage]


# ---------------------------------------------------------------------------
# Basic behavior (no effects)
# ---------------------------------------------------------------------------


class TestWindBasic:
    def test_before_and_after_order(self):
        """before() runs on enter, after() runs on exit."""
        log: list[str] = []
        with wind(lambda: log.append("before"), lambda: log.append("after")):
            log.append("body")
        assert log == ["before", "body", "after"]

    def test_after_only(self):
        """after keyword argument works without before."""
        log: list[str] = []
        with wind(after=lambda: log.append("after")):
            log.append("body")
        assert log == ["body", "after"]

    def test_before_only(self):
        """before without after works."""
        log: list[str] = []
        with wind(lambda: log.append("before")):
            log.append("body")
        assert log == ["before", "body"]

    def test_no_before_no_after(self):
        """wind with no arguments acts as a no-op guard."""
        with wind():
            pass

    def test_ref_wraps_before_return(self):
        """as target receives a Ref wrapping before()'s return value."""
        with wind(lambda: 42) as ref:
            assert isinstance(ref, Ref)
            assert ref.unwrap() == 42

    def test_ref_wraps_none_when_no_before(self):
        """as target is a Ref(None) when before is not provided."""
        with wind(after=lambda: None) as ref:
            assert isinstance(ref, Ref)
            assert ref.unwrap() is None

    def test_ref_wraps_none_when_before_returns_none(self):
        """as target is a Ref(None) when before returns None."""
        with wind(lambda: None) as ref:
            assert isinstance(ref, Ref)
            assert ref.unwrap() is None

    def test_after_runs_on_exception(self):
        """after() runs even if the body raises."""
        log: list[str] = []
        with pytest.raises(ValueError, match="boom"):
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                raise ValueError("boom")
        assert log == ["before", "after"]

    def test_exception_propagates(self):
        """Exception from the body propagates."""
        with pytest.raises(ValueError, match="from body"):
            with wind():
                raise ValueError("from body")

    def test_before_exception_skips_body_and_after(self):
        """If before() raises, the body and after() are not executed."""
        log: list[str] = []

        def bad_before():
            raise ValueError("before failed")

        with pytest.raises(ValueError, match="before failed"):
            with wind(bad_before, lambda: log.append("after")):
                log.append("body")
        assert log == []

    def test_after_exception_propagates(self):
        """If after() raises, its exception propagates."""

        def bad_after():
            raise ValueError("after failed")

        with pytest.raises(ValueError, match="after failed"):
            with wind(after=bad_after):
                pass

    def test_after_exception_masks_body_exception(self):
        """If both body and after() raise, after()'s exception wins."""

        def bad_after():
            raise RuntimeError("after failed")

        with pytest.raises(RuntimeError, match="after failed"):
            with wind(after=bad_after):
                raise ValueError("body failed")


# ---------------------------------------------------------------------------
# auto_exit: before() returning a context manager
# ---------------------------------------------------------------------------


class TestWindAutoExit:
    def test_auto_exit_enters_and_exits_cm(self):
        """When before() returns a cm and auto_exit=True, __enter__/__exit__ are called."""
        log: list[str] = []

        class CM:
            def __enter__(self):
                log.append("cm-enter")
                return self

            def __exit__(self, *exc_info: object) -> bool:
                log.append("cm-exit")
                return False

        with wind(lambda: CM()) as ref:
            log.append("body")
            assert isinstance(ref.unwrap(), CM)
        assert log == ["cm-enter", "body", "cm-exit"]

    def test_auto_exit_false_skips_cm(self):
        """When auto_exit=False, __enter__/__exit__ are not called."""
        log: list[str] = []

        class CM:
            def __enter__(self):
                log.append("cm-enter")
                return self

            def __exit__(self, *exc_info: object) -> bool:
                log.append("cm-exit")
                return False

        with wind(lambda: CM(), auto_exit=False) as ref:
            log.append("body")
            assert isinstance(ref.unwrap(), CM)
        assert log == ["body"]

    def test_auto_exit_cm_receives_exception_info(self):
        """cm.__exit__ receives exception info from the body."""
        captured_exc: list[type | None] = []

        class CM:
            def __enter__(self):
                return self

            def __exit__(
                self,
                exc_type: type[BaseException] | None,
                exc_val: BaseException | None,
                exc_tb: object,
            ) -> bool:
                captured_exc.append(exc_type)
                return False

        with pytest.raises(ValueError):
            with wind(lambda: CM()):
                raise ValueError("oops")
        assert captured_exc == [ValueError]

    def test_auto_exit_cm_suppresses_exception(self):
        """cm.__exit__ returning True suppresses the exception."""

        class SuppressCM:
            def __enter__(self):
                return self

            def __exit__(self, *exc_info: object) -> bool:
                return True

        # Should not raise
        with wind(lambda: SuppressCM()):
            raise ValueError("suppressed")

    def test_after_with_argument_receives_before_result(self):
        """after(value) receives before()'s return value when it accepts an argument."""
        received: list[int] = []

        def after_fn(v: int) -> None:
            received.append(v)

        with wind(lambda: 42, after_fn):
            pass
        assert received == [42]

    def test_after_without_argument(self):
        """after() with no parameter works fine."""
        log: list[str] = []
        with wind(lambda: 42, lambda: log.append("after")):
            pass
        assert log == ["after"]

    def test_auto_exit_with_after(self):
        """auto_exit cm and explicit after both run."""
        log: list[str] = []

        class CM:
            def __enter__(self):
                log.append("cm-enter")
                return self

            def __exit__(self, *exc_info: object) -> bool:
                log.append("cm-exit")
                return False

        with wind(lambda: CM(), lambda: log.append("after")):
            log.append("body")
        assert log == ["cm-enter", "body", "cm-exit", "after"]


# ---------------------------------------------------------------------------
# Nested wind (no effects)
# ---------------------------------------------------------------------------


class TestWindNested:
    def test_nested_enter_exit_order(self):
        """Nested wind: outer-before, inner-before, inner-after, outer-after."""
        log: list[str] = []
        with wind(lambda: log.append("outer-before"), lambda: log.append("outer-after")):
            with wind(lambda: log.append("inner-before"), lambda: log.append("inner-after")):
                log.append("body")
        assert log == [
            "outer-before",
            "inner-before",
            "body",
            "inner-after",
            "outer-after",
        ]

    def test_nested_inner_exception_all_afters_run(self):
        """When inner body raises, both after() thunks run in correct order."""
        log: list[str] = []
        with pytest.raises(ValueError, match="inner boom"):
            with wind(lambda: log.append("outer-before"), lambda: log.append("outer-after")):
                with wind(lambda: log.append("inner-before"), lambda: log.append("inner-after")):
                    raise ValueError("inner boom")
        assert log == [
            "outer-before",
            "inner-before",
            "inner-after",
            "outer-after",
        ]

    def test_triple_nested(self):
        """Three levels of nesting work correctly."""
        log: list[str] = []
        with wind(lambda: log.append("A-before"), lambda: log.append("A-after")):
            with wind(lambda: log.append("B-before"), lambda: log.append("B-after")):
                with wind(lambda: log.append("C-before"), lambda: log.append("C-after")):
                    log.append("body")
        assert log == [
            "A-before",
            "B-before",
            "C-before",
            "body",
            "C-after",
            "B-after",
            "A-after",
        ]


# ---------------------------------------------------------------------------
# One-shot effects inside wind
# ---------------------------------------------------------------------------


class TestWindOneShotEffect:
    def test_effect_inside_body(self):
        """One-shot effect inside body: before and after called once each."""
        get_val: Effect[[], int] = effect("get_val")
        h: Handler[int] = create_handler(get_val)

        @h.on(get_val)
        def _get(k: Resume[int, int]):
            return k(10)

        log: list[str] = []

        def run() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return get_val() * 2

        result = h(run)
        assert result == 20
        assert log == ["before", "after"]

    def test_multiple_effects_inside_body(self):
        """Multiple one-shot effects inside body."""
        read: Effect[[], int] = effect("read")
        h: Handler[int] = create_handler(read)
        call_count = 0

        @h.on(read)
        def _read(k: Resume[int, int]):
            nonlocal call_count
            call_count += 1
            return k(call_count)

        log: list[str] = []

        def run() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                a = read()
                b = read()
                return a + b

        result = h(run)
        assert result == 3  # 1 + 2
        assert log == ["before", "after"]

    def test_effect_outside_wind(self):
        """Effect performed outside wind does not trigger before/after."""
        get_val: Effect[[], int] = effect("get_val")
        h: Handler[int] = create_handler(get_val)

        @h.on(get_val)
        def _get(k: Resume[int, int]):
            return k(5)

        log: list[str] = []

        def run() -> int:
            v = get_val()
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return v * 10

        result = h(run)
        assert result == 50
        assert log == ["before", "after"]

    def test_nested_wind_with_effect(self):
        """Effect inside nested wind with one-shot."""
        get_val: Effect[[], int] = effect("get_val")
        h: Handler[int] = create_handler(get_val)

        @h.on(get_val)
        def _get(k: Resume[int, int]):
            return k(7)

        log: list[str] = []

        def run() -> int:
            with wind(lambda: log.append("outer-before"), lambda: log.append("outer-after")):
                with wind(lambda: log.append("inner-before"), lambda: log.append("inner-after")):
                    return get_val()

        result = h(run)
        assert result == 7
        assert log == [
            "outer-before",
            "inner-before",
            "inner-after",
            "outer-after",
        ]


# ---------------------------------------------------------------------------
# Multi-shot effects inside wind
# ---------------------------------------------------------------------------


class TestWindMultiShot:
    def test_multishot_before_after_per_shot(self):
        """Multi-shot: before() and after() called once per shot."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        log: list[str] = []

        def run() -> list[int]:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return [choose() * 10]

        result = h(run)
        assert result == [10, 20]
        assert log == ["before", "after", "before", "after"]

    def test_multishot_three_shots(self):
        """Multi-shot with three resumes."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2) + k(3)

        log: list[str] = []

        def run() -> list[int]:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return [choose()]

        result = h(run)
        assert result == [1, 2, 3]
        assert log == ["before", "after", "before", "after", "before", "after"]

    def test_multishot_nested_wind(self):
        """Nested wind with multi-shot: all before/after called per shot."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        log: list[str] = []

        def run() -> list[int]:
            with wind(lambda: log.append("outer-before"), lambda: log.append("outer-after")):
                with wind(lambda: log.append("inner-before"), lambda: log.append("inner-after")):
                    return [choose()]

        result = h(run)
        assert result == [1, 2]
        assert log == [
            # first shot (one-shot)
            "outer-before",
            "inner-before",
            "inner-after",
            "outer-after",
            # second shot (multi-shot)
            "outer-before",
            "inner-before",
            "inner-after",
            "outer-after",
        ]

    def test_multishot_effect_outside_wind(self):
        """Multi-shot effect outside wind: before/after called per shot naturally."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        log: list[str] = []

        def run() -> list[int]:
            v = choose()
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return [v * 10]

        result = h(run)
        assert result == [10, 20]
        assert log == ["before", "after", "before", "after"]

    def test_multishot_winding_state_per_shot(self):
        """Each multi-shot resume produces independent before/after pairs."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(10) + k(20)

        log: list[str] = []

        def run() -> list[int]:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                v = choose()
                log.append(f"body-{v}")
                return [v]

        result = h(run)
        assert result == [10, 20]
        assert log == [
            "before",
            "body-10",
            "after",
            "before",
            "body-20",
            "after",
        ]

    def test_multishot_ref_updated_on_reentry(self):
        """Ref.unwrap() returns the new value from before() on multi-shot re-entry."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        call_count = 0

        def before():
            nonlocal call_count
            call_count += 1
            return call_count

        def run() -> list[int]:
            with wind(before) as ref:
                choose()
                return [ref.unwrap()]

        result = h(run)
        # Shot 1 (one-shot): before() returns 1, ref.unwrap() == 1, result [1]
        # Shot 2 (multi-shot): _do_winds calls before() → returns 2,
        #   ref._value updated to 2, ref.unwrap() == 2, result [2]
        assert result == [1, 2]


# ---------------------------------------------------------------------------
# Abort (handler doesn't call k)
# ---------------------------------------------------------------------------


class TestWindAbort:
    def test_abort_calls_after(self):
        """Handler abort (no resume) still calls after()."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return -1  # abort

        log: list[str] = []

        def run() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return e()

        result = h(run, check=False)
        assert result == -1
        assert log == ["before", "after"]

    def test_abort_nested_all_afters_run(self):
        """Nested wind with abort: all after() thunks run."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return -1  # abort

        log: list[str] = []

        def run() -> int:
            with wind(lambda: log.append("outer-before"), lambda: log.append("outer-after")):
                with wind(lambda: log.append("inner-before"), lambda: log.append("inner-after")):
                    return e()

        result = h(run, check=False)
        assert result == -1
        assert log == [
            "outer-before",
            "inner-before",
            "inner-after",
            "outer-after",
        ]


# ---------------------------------------------------------------------------
# Cross-extent: multi-shot from different dynamic extents
# ---------------------------------------------------------------------------


class TestWindCrossExtent:
    def test_shared_outer_extent(self):
        """wind wrapping the handler invocation shares outer extent.

        When the caller has [outer_entry, inner_entry] in its wind stack
        and the handler is also inside the outer wind, the multi-shot
        transition should only call before() for inner_entry (the shared
        outer_entry is already active).
        """
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        log: list[str] = []

        def caller() -> list[int]:
            with wind(lambda: log.append("inner-before"), lambda: log.append("inner-after")):
                return [choose()]

        def run() -> list[int]:
            with wind(lambda: log.append("outer-before"), lambda: log.append("outer-after")):
                return h(caller)

        result = run()
        assert result == [1, 2]
        assert log == [
            "outer-before",
            # first shot (one-shot)
            "inner-before",
            "inner-after",
            # second shot (multi-shot) - only inner rewound
            "inner-before",
            "inner-after",
            "outer-after",
        ]


# ---------------------------------------------------------------------------
# Multi-shot with exception inside body
# ---------------------------------------------------------------------------


class TestWindMultiShotException:
    def test_multishot_exception_runs_after_per_shot(self):
        """Multi-shot where one shot raises: after() still runs for each shot."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[int] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, int]):
            try:
                r1 = k(1)
            except ValueError:
                r1 = -1
            r2 = k(2)
            return r1 + r2

        log: list[str] = []

        def run() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                v = choose()
                if v == 1:
                    raise ValueError("bad")
                return v * 10

        result = h(run)
        assert result == -1 + 20
        assert log == ["before", "after", "before", "after"]


# ---------------------------------------------------------------------------
# Wind stack cleanup
# ---------------------------------------------------------------------------


class TestWindStackCleanup:
    def test_stack_empty_after_normal_exit(self):
        """Wind stack is empty after a normal with-block exit."""
        with wind(lambda: None, lambda: None):
            assert len(_get_wind_stack()) == 1
        assert _get_wind_stack() == []

    def test_stack_empty_after_exception(self):
        """Wind stack is empty after the body raises."""
        with pytest.raises(ValueError):
            with wind(lambda: None, lambda: None):
                raise ValueError("boom")
        assert _get_wind_stack() == []

    def test_stack_empty_after_before_exception(self):
        """Wind stack is empty when before() raises (entry never pushed)."""

        def bad_before():
            raise ValueError("before failed")

        with pytest.raises(ValueError):
            with wind(bad_before):
                pass
        assert _get_wind_stack() == []

    def test_stack_empty_after_after_exception(self):
        """Wind stack is empty even when after() raises."""

        def bad_after():
            raise ValueError("after failed")

        with pytest.raises(ValueError):
            with wind(lambda: None, bad_after):
                pass
        assert _get_wind_stack() == []

    def test_nested_stack_depth(self):
        """Wind stack depth matches nesting level at each point."""
        depths: list[int] = []
        with wind(lambda: None, lambda: None):
            depths.append(len(_get_wind_stack()))
            with wind(lambda: None, lambda: None):
                depths.append(len(_get_wind_stack()))
                with wind(lambda: None, lambda: None):
                    depths.append(len(_get_wind_stack()))
                depths.append(len(_get_wind_stack()))
            depths.append(len(_get_wind_stack()))
        depths.append(len(_get_wind_stack()))
        assert depths == [1, 2, 3, 2, 1, 0]

    def test_stack_empty_after_nested_inner_exception(self):
        """Wind stack is empty after an exception in nested wind body."""
        with pytest.raises(ValueError):
            with wind(lambda: None, lambda: None):
                with wind(lambda: None, lambda: None):
                    raise ValueError("nested boom")
        assert _get_wind_stack() == []

    def test_stack_empty_after_handler_with_wind(self):
        """Wind stack is empty after a handler invocation that uses wind."""
        get_val: Effect[[], int] = effect("get_val")
        h: Handler[int] = create_handler(get_val)

        @h.on(get_val)
        def _get(k: Resume[int, int]):
            return k(10)

        def run() -> int:
            with wind(lambda: None, lambda: None):
                return get_val()

        h(run)
        assert _get_wind_stack() == []

    def test_stack_empty_after_abort(self):
        """Wind stack is empty after handler abort."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return -1

        def run() -> int:
            with wind(lambda: None, lambda: None):
                return e()

        h(run, check=False)
        assert _get_wind_stack() == []

    def test_stack_empty_after_multishot(self):
        """Wind stack is empty after multi-shot handler completes."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        def run() -> list[int]:
            with wind(lambda: None, lambda: None):
                return [choose()]

        h(run)
        assert _get_wind_stack() == []

    def test_stack_empty_after_multishot_with_exception(self):
        """Wind stack is empty after multi-shot where a shot raises."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[int] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, int]):
            try:
                k(1)
            except ValueError:
                pass
            return k(2)

        def run() -> int:
            with wind(lambda: None, lambda: None):
                v = choose()
                if v == 1:
                    raise ValueError("bad")
                return v

        h(run)
        assert _get_wind_stack() == []


# ---------------------------------------------------------------------------
# Wind stack / handler stack interaction
# ---------------------------------------------------------------------------


class TestWindHandlerInteraction:
    def test_wind_inside_handler_fn(self):
        """wind used inside a handler function (handler greenlet context)."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            with wind(lambda: log.append("handler-before"), lambda: log.append("handler-after")):
                return k(42)

        def run():
            return e()

        result = h(run)
        assert result == 42
        assert log == ["handler-before", "handler-after"]
        assert _get_wind_stack() == []

    def test_wind_in_caller_and_handler_independent(self):
        """Wind in caller and wind in handler fn don't interfere."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            with wind(lambda: log.append("handler-before"), lambda: log.append("handler-after")):
                return k(10)

        def run() -> int:
            with wind(lambda: log.append("caller-before"), lambda: log.append("caller-after")):
                return e() * 2

        result = h(run)
        assert result == 20
        assert "caller-before" in log
        assert "caller-after" in log
        assert "handler-before" in log
        assert "handler-after" in log
        assert _get_wind_stack() == []

    def test_handler_cleanup_does_not_affect_wind_stack(self):
        """Handler stack cleanup (_remove_all_handlers) leaves wind stack intact."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        wind_stack_during_handler: list[int] = []

        @h.on(e)
        def _handle(k: Resume[int, int]):
            # Handler fn runs in handler greenlet.
            # Check that handler's wind stack is separate from caller's.
            wind_stack_during_handler.append(len(_get_wind_stack()))
            return k(10)

        def run() -> int:
            with wind(lambda: None, lambda: None):
                return e()

        h(run)
        # The handler greenlet has no wind entries of its own
        # (the caller's wind entries are in the caller's context)
        assert wind_stack_during_handler == [0]
        assert _get_wind_stack() == []

    def test_interleaved_wind_and_handler_nesting(self):
        """wind and handler nesting interleaved correctly."""
        outer_e: Effect[[], int] = effect("outer_e")
        inner_e: Effect[[], int] = effect("inner_e")
        h_outer: Handler[int] = create_handler(outer_e)
        h_inner: Handler[int] = create_handler(inner_e)

        @h_outer.on(outer_e)
        def _outer(k: Resume[int, int]):
            return k(100)

        @h_inner.on(inner_e)
        def _inner(k: Resume[int, int]):
            return k(10)

        log: list[str] = []

        def run() -> int:
            with wind(lambda: log.append("wind-1-before"), lambda: log.append("wind-1-after")):
                a = outer_e()
                with wind(lambda: log.append("wind-2-before"), lambda: log.append("wind-2-after")):
                    b = h_inner(lambda: inner_e())
                    return a + b

        result = h_outer(run)
        assert result == 110
        assert log == [
            "wind-1-before",
            "wind-2-before",
            "wind-2-after",
            "wind-1-after",
        ]
        assert _get_wind_stack() == []


# ---------------------------------------------------------------------------
# __enter__ error paths
# ---------------------------------------------------------------------------


class TestWindEnterErrors:
    def test_cm_enter_raises_does_not_push_to_stack(self):
        """If cm.__enter__() raises, the wind entry is not left on the stack."""

        class BadCM:
            def __enter__(self):
                raise RuntimeError("enter failed")

            def __exit__(self, *exc_info: object) -> bool:
                return False

        with pytest.raises(RuntimeError, match="enter failed"):
            with wind(lambda: BadCM()):
                pass
        assert _get_wind_stack() == []

    def test_cm_exit_raises_stack_still_clean(self):
        """If cm.__exit__() raises, the wind stack is still cleaned up."""

        class BadExitCM:
            def __enter__(self):
                return self

            def __exit__(self, *exc_info: object) -> bool:
                raise RuntimeError("exit failed")

        with pytest.raises(RuntimeError, match="exit failed"):
            with wind(lambda: BadExitCM()):
                pass
        assert _get_wind_stack() == []


# ---------------------------------------------------------------------------
# Ref corner cases
# ---------------------------------------------------------------------------


class TestRefCornerCases:
    def test_ref_unwrap_after_exit(self):
        """Ref.unwrap() still returns the last value after with-block exit."""
        with wind(lambda: 99) as ref:
            pass
        assert ref.unwrap() == 99

    def test_ref_identity_preserved_across_multishot(self):
        """The same Ref object is reused across multi-shot re-entries."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        ref_ids: list[int] = []

        def run() -> list[int]:
            with wind(lambda: 0) as ref:
                choose()
                ref_ids.append(id(ref))
                return [ref.unwrap()]

        h(run)
        # Both shots should see the same Ref object (heap-shared)
        assert len(ref_ids) == 2
        assert ref_ids[0] == ref_ids[1]


# ---------------------------------------------------------------------------
# Handler-side dynamic extents must survive multi-shot re-entry (#36)
#
# A multi-shot resume runs the continuation in a fresh greenlet whose context
# is copied from the *invoker* of k() -- the handler.  The wind entries visible
# there belong to the handler's branch of the dynamic tree, not to the
# continuation's.  k() returns to the handler, so that branch stays active and
# must never be unwound by the re-entry.
# ---------------------------------------------------------------------------


class TestWindHandlerExtentIsolation:
    def test_wind_inside_handler_fn_two_shots(self):
        """A wind in the handler fn pairs 1:1 when k is resumed twice."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return k(1) + k(2)

        def body() -> int:
            log.append("body start")
            v = e()
            log.append(f"body v={v}")
            return v

        assert h(body) == 3
        assert log == ["body start", "before", "body v=1", "body v=2", "after"]
        assert _get_wind_stack() == []

    def test_wind_inside_handler_fn_three_shots(self):
        """The extra after() must not scale with the number of shots."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return k(1) + k(2) + k(3)

        assert h(lambda: e()) == 6
        assert log.count("before") == 1
        assert log.count("after") == 1
        assert log[-1] == "after"
        assert _get_wind_stack() == []

    def test_nested_wind_inside_handler_fn_multishot(self):
        """Nested handler-side winds unwind innermost-first, exactly once."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            with wind(lambda: log.append("before(outer)"), lambda: log.append("after(outer)")):
                with wind(lambda: log.append("before(inner)"), lambda: log.append("after(inner)")):
                    return k(1) + k(2)

        assert h(lambda: e()) == 3
        assert log == [
            "before(outer)",
            "before(inner)",
            "after(inner)",
            "after(outer)",
        ]
        assert _get_wind_stack() == []

    def test_wind_in_caller_and_handler_multishot(self):
        """Caller-side and handler-side extents are independent under multi-shot."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            with wind(lambda: log.append("before(H)"), lambda: log.append("after(H)")):
                return k(1) + k(2)

        def body() -> int:
            with wind(lambda: log.append("before(C)"), lambda: log.append("after(C)")):
                v = e()
                log.append(f"v={v}")
                return v

        assert h(body) == 3
        # The caller's extent is re-entered per shot; the handler's is not.
        assert log == [
            "before(C)",
            "before(H)",
            "v=1",
            "after(C)",
            "before(C)",
            "v=2",
            "after(C)",
            "after(H)",
        ]
        assert _get_wind_stack() == []

    def test_escaping_continuation_under_unrelated_wind(self):
        """An escaped k() must not unwind a wind it knows nothing about.

        No handler-side wind is involved here: the continuation is stored by the
        handler, h() returns, and k is then invoked from inside an unrelated
        dynamic extent at top level.
        """
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []
        saved: list[Resume[int, int]] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            saved.append(k)
            return 0

        assert h(lambda: e()) == 0

        with wind(lambda: log.append("before"), lambda: log.append("after")):
            assert saved[0](99) == 99

        assert log == ["before", "after"]
        assert _get_wind_stack() == []

    def test_nested_multishot_does_not_unwind_rewound_wind(self):
        """An inner resume must not unwind what an outer rewind re-entered."""
        inner_e: Effect[[], int] = effect("inner")
        outer_e: Effect[[int], int] = effect("outer")

        h_outer: Handler[Any] = create_handler(outer_e)
        h_inner: Handler[Any] = create_handler(inner_e)

        log: list[str] = []

        @h_outer.on(outer_e)
        def _handle_outer(k: Resume[int, Any], v: int) -> list[Any]:
            return [k(v), k(v + 100)]

        @h_inner.on(inner_e)
        def _handle_inner(k: Resume[int, Any]) -> Any:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return k(outer_e(7))

        h_outer(lambda: h_inner(lambda: inner_e()))

        # The outer handler drives the inner handler fn twice, so the guard is
        # entered twice -- and must be exited exactly twice.
        assert log.count("before") == 2
        assert log.count("after") == 2
        assert log == ["before", "after", "before", "after"]
        assert _get_wind_stack() == []

    def test_recursive_multishot_wind_in_handler(self):
        """Recursive multi-shot must not amplify the handler-side after()."""
        choose: Effect[[list[int]], int] = effect("choose")
        h: Handler[Any] = create_handler(choose)

        log: list[str] = []
        first = [True]

        @h.on(choose)
        def _choose(k: Resume[int, Any], opts: list[int]) -> list[Any]:
            if first[0]:
                first[0] = False
                with wind(lambda: log.append("before"), lambda: log.append("after")):
                    return [k(o) for o in opts]
            return [k(o) for o in opts]

        def body() -> int:
            a = choose([1, 2])
            b = choose([10, 20])
            return a + b

        h(body)
        assert log == ["before", "after"]
        assert _get_wind_stack() == []

    def test_wind_outside_handler_invocation_multishot(self):
        """A wind wrapping h(...) is shared, so it is neither re-entered nor exited.

        Regression guard: a fix that simply clears the re-entry greenlet's wind
        stack would re-enter this extent once per shot.
        """
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            return k(1) + k(2)

        def run() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return h(lambda: e())

        assert run() == 3
        assert log == ["before", "after"]
        assert _get_wind_stack() == []

    def test_wind_range_inside_handler_fn_multishot(self):
        """A handler-side wind_range keeps its position across shots.

        Its snapshot belongs to the handler's branch and must never be restored
        by a continuation re-entry, which would rewind the loop.
        """
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        seen: list[int] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            total = 0
            with wind_range(3) as r:
                for i in r:
                    seen.append(i)
                    total += k(i)
            return total

        assert h(lambda: e()) == 0 + 1 + 2
        assert seen == [0, 1, 2]
        assert _get_wind_stack() == []

    def test_cm_resource_stays_open_across_shots_in_handler(self):
        """An auto_exit resource in the handler fn is not released between shots."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @contextmanager
        def resource() -> Iterator[dict[str, bool]]:
            log.append("open")
            state = {"closed": False}
            try:
                yield state
            finally:
                state["closed"] = True
                log.append("close")

        closed_after_shot: list[bool] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            with wind(resource) as ref:
                a = k(1)
                closed_after_shot.append(ref.unwrap()["closed"])
                b = k(2)
                closed_after_shot.append(ref.unwrap()["closed"])
                return a + b

        assert h(lambda: e()) == 3
        # The resource must still be usable inside the with block after every shot.
        assert closed_after_shot == [False, False]
        assert log == ["open", "close"]
        assert _get_wind_stack() == []

    def test_exception_in_later_shot_with_handler_wind(self):
        """after() runs once when a later shot raises out of the continuation."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                k(1)
                return k(2)

        def body() -> int:
            v = e()
            if v == 2:
                raise ValueError("boom")
            return v

        with pytest.raises(ValueError, match="boom"):
            h(body)

        assert log == ["before", "after"]
        assert _get_wind_stack() == []


class TestWindAsyncHandlerExtentIsolation:
    @pytest.mark.asyncio
    async def test_wind_inside_async_handler_fn_multishot(self):
        """The async re-entry path has the same isolation requirement."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        log: list[str] = []

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]) -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return await k(1) + await k(2) + await k(3)

        assert await h(lambda: e()) == 6
        assert log == ["before", "after"]
        assert _get_wind_stack() == []

    @pytest.mark.asyncio
    async def test_wind_in_async_caller_and_handler_multishot(self):
        """Caller-side extents still rewind per shot under an async handler."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        log: list[str] = []

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]) -> int:
            with wind(lambda: log.append("before(H)"), lambda: log.append("after(H)")):
                return await k(1) + await k(2)

        def body() -> int:
            with wind(lambda: log.append("before(C)"), lambda: log.append("after(C)")):
                return e()

        assert await h(body) == 3
        assert log == [
            "before(C)",
            "before(H)",
            "after(C)",
            "before(C)",
            "after(C)",
            "after(H)",
        ]
        assert _get_wind_stack() == []


# ---------------------------------------------------------------------------
# Rewind failure handling
#
# rewind() runs before restore_continuation(), so no Python frame exists yet
# that would __exit__ the extents it has just re-entered.  If a before() raises
# part-way through, the re-entry must undo itself or those extents are lost.
# ---------------------------------------------------------------------------


class TestWindRewindRollback:
    def test_before_raising_on_reentry_unwinds_partial_extents(self):
        """A before() that raises mid-rewind must not orphan the outer extent."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []
        calls = {"n": 0}

        def flaky_before() -> None:
            calls["n"] += 1
            log.append(f"before(inner)#{calls['n']}")
            if calls["n"] == 2:
                raise RuntimeError("inner before failed")

        outer = wind(lambda: log.append("before(outer)"), lambda: log.append("after(outer)"))
        inner = wind(flaky_before, lambda: log.append("after(inner)"))

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            return k(1) + k(2)

        def body() -> int:
            with outer:
                with inner:
                    return e()

        with pytest.raises(RuntimeError, match="inner before failed"):
            h(body)

        assert log == [
            "before(outer)",
            "before(inner)#1",
            "after(inner)",
            "after(outer)",
            # second shot: outer is re-entered, inner's before() then fails
            "before(outer)",
            "before(inner)#2",
            # the re-entered outer extent must be unwound, not lost
            "after(outer)",
        ]
        assert log.count("before(outer)") == log.count("after(outer)")
        assert _get_wind_stack() == []


# ---------------------------------------------------------------------------
# enter/exit pairing is enforced by WindBase
# ---------------------------------------------------------------------------


class TestWindPairingGuard:
    def test_exit_without_entry_raises(self):
        """__exit__ on a wind that was never entered is an error."""
        w = wind(lambda: None, lambda: None)
        with pytest.raises(RuntimeError, match="wind exit without active entry"):
            w.__exit__(None, None, None)

    def test_exit_more_times_than_entered_raises(self):
        """A second __exit__ after a balanced with-block is an error."""
        w = wind(lambda: None, lambda: None)
        with w:
            pass
        with pytest.raises(RuntimeError, match="wind exit without active entry"):
            w.__exit__(None, None, None)
        assert _get_wind_stack() == []

    def test_same_wind_object_can_nest(self):
        """Recursive reuse of one wind object keeps working (no auto_exit cm)."""
        log: list[str] = []
        w = wind(lambda: log.append("before"), lambda: log.append("after"))

        def rec(n: int) -> None:
            with w:
                if n:
                    rec(n - 1)

        rec(2)
        assert log == ["before"] * 3 + ["after"] * 3
        assert _get_wind_stack() == []
