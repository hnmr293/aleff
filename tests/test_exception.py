"""Tests for exception integration with effect handlers (Issue #8).

Defines and verifies the semantics for how exceptions interact with
effect handlers in all supported scenarios.
"""

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
    EffectNotHandledError,
)


# ---------------------------------------------------------------------------
# Exception in computation
# ---------------------------------------------------------------------------


class TestComputationException:
    def test_exception_before_effect(self):
        """Exception raised before any effect is performed."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("val")

        with pytest.raises(RuntimeError, match="before effect"):
            h(lambda: (_ for _ in ()).throw(RuntimeError("before effect")))

    def test_exception_after_resume(self):
        """Exception raised in computation after effect is resumed."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return k(42)

        def caller() -> int:
            val = e()
            raise RuntimeError(f"after resume: {val}")

        with pytest.raises(RuntimeError, match="after resume: 42"):
            h(caller)

    def test_handler_catches_computation_exception_via_resume(self):
        """Handler can catch exceptions from computation via k()."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            try:
                return k(42)
            except RuntimeError:
                return -1

        def caller() -> int:
            e()
            raise RuntimeError("computation error")

        result = h(caller)
        assert result == -1


# ---------------------------------------------------------------------------
# Exception in handler function
# ---------------------------------------------------------------------------


class TestHandlerException:
    def test_handler_exception_before_resume(self):
        """Exception in handler fn before calling k()."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            raise ValueError("handler error")

        with pytest.raises(ValueError, match="handler error"):
            h(lambda: e())

    def test_handler_exception_after_resume(self):
        """Exception in handler fn after k() returns."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            result = k(42)
            raise ValueError(f"post-resume: {result}")

        with pytest.raises(ValueError, match="post-resume: 42"):
            h(lambda: e())

    def test_handler_exception_wins_over_computation(self):
        """When both handler and computation raise, handler's exception propagates."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            try:
                return k(42)
            except RuntimeError:
                pass
            raise ValueError("handler error")

        def caller() -> int:
            e()
            raise RuntimeError("computation error")

        with pytest.raises(ValueError, match="handler error"):
            h(caller)


# ---------------------------------------------------------------------------
# Abort (handler doesn't call resume) — GreenletExit must not leak
# ---------------------------------------------------------------------------


class TestAbortNoGreenletExitLeak:
    def test_abort_basic(self):
        """Handler returns without calling k() — computation is aborted."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 99

        result = h(lambda: e())
        assert result == 99

    def test_abort_does_not_leak_greenlet_exit(self):
        """Abort must NOT expose GreenletExit to user code."""
        import greenlet as gl

        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 99

        caught_types: list[type] = []

        def caller() -> int:
            try:
                return e()
            except BaseException as ex:
                caught_types.append(type(ex))
                raise

        result = h(caller)
        assert result == 99
        # GreenletExit must NOT appear in caught exceptions
        assert gl.GreenletExit not in caught_types

    def test_abort_finally_runs(self):
        """Abort should still run finally blocks in the computation."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 99

        finally_ran = False

        def caller() -> int:
            nonlocal finally_ran
            try:
                return e()
            finally:
                finally_ran = True

        result = h(caller)
        assert result == 99
        assert finally_ran

    def test_abort_except_exception_does_not_catch(self):
        """Abort should NOT be caught by 'except Exception'."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 99

        caught = False

        def caller() -> int:
            nonlocal caught
            try:
                return e()
            except Exception:
                caught = True
                return -1

        result = h(caller)
        assert result == 99
        assert not caught

    def test_abort_caller_catches_and_performs_another_effect(self):
        """If caller catches abort and performs another effect, abort persists."""
        e1: Effect[[], int] = effect("e1")
        e2: Effect[[], int] = effect("e2")
        h: Handler[int] = create_handler(e1, e2)

        @h.on(e1)
        def _handle_e1(k: Resume[int, int]):
            return 99  # abort

        @h.on(e2)
        def _handle_e2(k: Resume[int, int]):
            return k(50)

        attempts = 0

        def caller() -> int:
            nonlocal attempts
            try:
                return e1()
            except BaseException:
                attempts += 1
                return e2()  # try another effect after catching abort

        result = h(caller)
        assert result == 99


# ---------------------------------------------------------------------------
# Abort on the exception path
#
# A handler that returns without resuming aborts the caller, so its finally
# blocks and wind guards run.  A handler that *raises* without resuming leaves
# the caller suspended in exactly the same state, and owes it the same abort.
# ---------------------------------------------------------------------------


class TestAbortOnHandlerException:
    def test_handler_exception_runs_caller_finally(self):
        """A handler that raises without resuming still unwinds the caller."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            log.append("handler raises")
            raise ValueError("handler error")

        def caller() -> int:
            try:
                return e()
            finally:
                log.append("caller finally")

        # Holding the traceback keeps the suspended caller greenlet alive, so
        # this observes the abort itself rather than a collection side effect.
        with pytest.raises(ValueError, match="handler error") as excinfo:
            h(caller)
        log.append("caught")

        assert excinfo.value.__traceback__ is not None
        # The caller must be unwound before the exception surfaces -- not later,
        # when the abandoned greenlet happens to be collected.
        assert log == ["handler raises", "caller finally", "caught"]

    def test_handler_exception_runs_caller_wind_after(self):
        """A wind guard in the caller is unwound when the handler raises."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            raise ValueError("handler error")

        def caller() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return e()

        with pytest.raises(ValueError, match="handler error") as excinfo:
            h(caller)

        assert excinfo.value.__traceback__ is not None
        assert log == ["before", "after"]

    def test_unhandled_effect_in_continuation_unwinds_caller(self):
        """A dispatch failure mid-continuation unwinds the caller too."""
        e: Effect[[], int] = effect("e")
        missing: Effect[[], int] = effect("missing")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            return k(1)

        def caller() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                v = e()
                missing()
                return v

        # EffectNotHandledError is generic, so pytest.raises cannot type it;
        # catch it directly and hold it for the same reason as above.
        raised: list[BaseException] = []
        try:
            h(caller)
        except BaseException as ex:
            raised.append(ex)

        assert len(raised) == 1
        assert isinstance(raised[0], EffectNotHandledError)
        assert log == ["before", "after"]

    def test_nested_handler_exception_unwinds_both_callers(self):
        """Each suspended caller in the chain is unwound, innermost first."""
        outer_e: Effect[[], int] = effect("outer_e")
        inner_e: Effect[[], int] = effect("inner_e")

        h_outer: Handler[int] = create_handler(outer_e)
        h_inner: Handler[int] = create_handler(inner_e)

        log: list[str] = []

        @h_outer.on(outer_e)
        def _handle_outer(k: Resume[int, int]) -> int:
            raise ValueError("handler error")

        @h_inner.on(inner_e)
        def _handle_inner(k: Resume[int, int]) -> int:
            return k(outer_e())

        def inner_caller() -> int:
            try:
                return inner_e()
            finally:
                log.append("inner finally")

        def outer_caller() -> int:
            try:
                return h_inner(inner_caller)
            finally:
                log.append("outer finally")

        with pytest.raises(ValueError, match="handler error") as excinfo:
            h_outer(outer_caller)

        assert excinfo.value.__traceback__ is not None
        assert log == ["inner finally", "outer finally"]

    def test_caller_cleanup_exception_propagates(self):
        """A cleanup failure during the abort is not swallowed."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            raise ValueError("handler error")

        def caller() -> int:
            try:
                return e()
            finally:
                raise RuntimeError("cleanup error")

        # Standard finally semantics: the cleanup failure replaces the
        # in-flight exception rather than being discarded.
        with pytest.raises(RuntimeError, match="cleanup error"):
            h(caller)

    def test_handler_exception_after_resume_does_not_double_abort(self):
        """A caller that already ran to completion is not aborted again."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        log: list[str] = []

        @h.on(e)
        def _handle(k: Resume[int, int]) -> int:
            k(42)
            raise ValueError("post-resume")

        def caller() -> int:
            try:
                return e()
            finally:
                log.append("caller finally")

        with pytest.raises(ValueError, match="post-resume"):
            h(caller)

        assert log == ["caller finally"]


class TestAsyncAbortOnHandlerException:
    @pytest.mark.asyncio
    async def test_async_handler_exception_runs_caller_finally(self):
        """The async handler path owes the caller the same abort."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        log: list[str] = []

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]) -> int:
            log.append("handler raises")
            raise ValueError("handler error")

        def caller() -> int:
            try:
                return e()
            finally:
                log.append("caller finally")

        with pytest.raises(ValueError, match="handler error") as excinfo:
            await h(caller)
        log.append("caught")

        assert excinfo.value.__traceback__ is not None
        assert log == ["handler raises", "caller finally", "caught"]

    @pytest.mark.asyncio
    async def test_async_sync_handler_fn_exception_runs_caller_finally(self):
        """A sync handler fn under an async handler takes the same path."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        log: list[str] = []

        # An async handler accepts a sync handler fn at runtime -- it is run via
        # _run_handler_fn_in_greenlet -- but AsyncEffectHandler requires async.
        @h.on(e)  # pyright: ignore[reportArgumentType]
        def _handle(k: ResumeAsync[int, int]) -> int:
            raise ValueError("handler error")

        def caller() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                return e()

        with pytest.raises(ValueError, match="handler error") as excinfo:
            await h(caller)

        assert excinfo.value.__traceback__ is not None
        assert log == ["before", "after"]

    @pytest.mark.asyncio
    async def test_async_unhandled_effect_in_continuation_unwinds_caller(self):
        """The abort must reach the caller across the bridge greenlet.

        Here the abort is driven from the handler fn's greenlet, which is not
        caller_gl.parent, so the unwinding exception has to be routed back to
        the aborting greenlet rather than to the caller's original parent.
        """
        e: Effect[[], int] = effect("e")
        missing: Effect[[], int] = effect("missing")
        h: AsyncHandler[int] = create_async_handler(e)

        log: list[str] = []

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]) -> int:
            return await k(1)

        def caller() -> int:
            with wind(lambda: log.append("before"), lambda: log.append("after")):
                v = e()
                missing()
                return v

        raised: list[BaseException] = []
        try:
            await h(caller)
        except BaseException as ex:
            raised.append(ex)

        assert len(raised) == 1
        assert isinstance(raised[0], EffectNotHandledError)
        assert log == ["before", "after"]


# ---------------------------------------------------------------------------
# Nested handler exception propagation
# ---------------------------------------------------------------------------


class TestNestedException:
    def test_exception_propagates_through_nested_handlers(self):
        """Exception in innermost computation propagates through all handlers."""
        e1: Effect[[], int] = effect("e1")
        e2: Effect[[], int] = effect("e2")

        h1: Handler[int] = create_handler(e1)
        h2: Handler[int] = create_handler(e2)

        @h1.on(e1)
        def _h1(k: Resume[int, int]):
            return k(10)

        @h2.on(e2)
        def _h2(k: Resume[int, int]):
            return k(20)

        def caller() -> int:
            e1()
            raise RuntimeError("nested error")

        with pytest.raises(RuntimeError, match="nested error"):
            h1(lambda: h2(caller))

    def test_inner_handler_catches_exception(self):
        """Inner handler can catch and recover from computation exception."""
        e1: Effect[[], int] = effect("e1")
        e2: Effect[[], int] = effect("e2")

        h1: Handler[int] = create_handler(e1)
        h2: Handler[int] = create_handler(e2)

        @h1.on(e1)
        def _h1(k: Resume[int, int]):
            return k(10)

        @h2.on(e2)
        def _h2(k: Resume[int, int]):
            try:
                return k(20)
            except RuntimeError:
                return -1

        def caller() -> int:
            e1()
            e2()
            raise RuntimeError("error")

        result = h1(lambda: h2(caller))
        assert result == -1


# ---------------------------------------------------------------------------
# Multi-shot + exceptions
# ---------------------------------------------------------------------------


class TestMultiShotException:
    def test_exception_in_one_shot_handler_continues(self):
        """Multi-shot: exception in one shot, handler catches and continues."""
        e: Effect[[], int] = effect("e")
        h: Handler[list[int]] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, list[int]]):
            results: list[list[int]] = []
            for val in [1, 2, 3]:
                try:
                    results.append(k(val))
                except ValueError:
                    results.append([-1])
            return [x for r in results for x in r]

        def caller() -> list[int]:
            val = e()
            if val == 2:
                raise ValueError("bad")
            return [val * 10]

        result = h(caller)
        assert result == [10, -1, 30]

    def test_exception_in_handler_after_multishot(self):
        """Exception in handler fn after multiple k() calls."""
        e: Effect[[], int] = effect("e")
        h: Handler[list[int]] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, list[int]]):
            k(1)
            k(2)
            raise ValueError("after multishot")

        def caller() -> list[int]:
            return [e() * 10]

        with pytest.raises(ValueError, match="after multishot"):
            h(caller)


# ---------------------------------------------------------------------------
# Async exception integration
# ---------------------------------------------------------------------------


class TestAsyncException:
    @pytest.mark.asyncio
    async def test_async_handler_exception_after_resume(self):
        """Async handler fn raises after await k()."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            result = await k(42)
            raise ValueError(f"async post-resume: {result}")

        with pytest.raises(ValueError, match="async post-resume: 42"):
            await h(lambda: e())

    @pytest.mark.asyncio
    async def test_async_handler_catches_computation_exception(self):
        """Async handler catches exception from computation via k()."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            try:
                return await k(42)
            except RuntimeError:
                return -1

        def caller() -> int:
            e()
            raise RuntimeError("computation error")

        result = await h(caller)
        assert result == -1

    @pytest.mark.asyncio
    async def test_async_abort_does_not_leak_greenlet_exit(self):
        """Async abort must NOT expose GreenletExit to user code."""
        import greenlet as gl

        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return 99

        caught_types: list[type] = []

        def caller() -> int:
            try:
                return e()
            except BaseException as ex:
                caught_types.append(type(ex))
                raise

        result = await h(caller)
        assert result == 99
        assert gl.GreenletExit not in caught_types

    @pytest.mark.asyncio
    async def test_async_abort_finally_runs(self):
        """Async abort should still run finally blocks."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return 99

        finally_ran = False

        def caller() -> int:
            nonlocal finally_ran
            try:
                return e()
            finally:
                finally_ran = True

        result = await h(caller)
        assert result == 99
        assert finally_ran


# ---------------------------------------------------------------------------
# Stack cleanup after exceptions
# ---------------------------------------------------------------------------


class TestStackCleanupAfterException:
    def test_stack_cleaned_after_handler_exception(self):
        """Handler stack is cleaned even when handler fn raises."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            raise ValueError("handler error")

        with pytest.raises(ValueError):
            h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()

    def test_stack_cleaned_after_computation_exception(self):
        """Handler stack is cleaned when computation raises."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("val")

        with pytest.raises(RuntimeError):
            h(lambda: (_ for _ in ()).throw(RuntimeError("boom")))

        with pytest.raises(EffectNotHandledError):
            e()

    def test_stack_cleaned_after_abort(self):
        """Handler stack is cleaned after abort."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 0

        h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()
