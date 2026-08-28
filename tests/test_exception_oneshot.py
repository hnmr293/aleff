"""Tests for exception integration with effect handlers (Issue #8).

Defines and verifies the semantics for how exceptions interact with
effect handlers in all supported scenarios.
"""

import pytest

from aleff.oneshot import (
    effect,
    Effect,
    Resume,
    ResumeAsync,
    Handler,
    AsyncHandler,
    create_handler,
    create_async_handler,
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
