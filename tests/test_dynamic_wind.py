"""Tests for wind context manager.

wind(before, after, *, auto_exit=True) establishes a dynamic extent guard:
- Entering the with block calls before() and pushes a wind entry
- Exiting the with block pops the wind entry, calls cm.__exit__ if applicable, calls after()
- On multi-shot resume, before() is called again before re-entering the extent
- The return value of before() is wrapped in a Ref for multi-shot safety
"""

import pytest

from aleff import (
    effect,
    Effect,
    Resume,
    Handler,
    create_handler,
    wind,
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
