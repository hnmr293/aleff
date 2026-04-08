"""Tests for performing effects from within a handler function (Issue #7).

Oneshot-specific variant — imports from aleff.oneshot.
"""

import asyncio  # pyright: ignore[reportUnusedImport]

import pytest
import pytest_asyncio  # pyright: ignore[reportUnusedImport]

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
# Sync handler performs outer effect
# ---------------------------------------------------------------------------


class TestSyncHandlerPerformsEffect:
    def test_handler_performs_different_outer_effect(self):
        """Inner handler fn for effect_b performs effect_a handled by outer."""
        effect_a: Effect[[], str] = effect("a")
        effect_b: Effect[[], str] = effect("b")

        h_outer: Handler[str] = create_handler(effect_a)

        @h_outer.on(effect_a)
        def _handle_a(k: Resume[str, str]):
            return k("from_outer")

        h_inner: Handler[str] = create_handler(effect_b)

        @h_inner.on(effect_b)
        def _handle_b(k: Resume[str, str]):
            val = effect_a()
            return k(val)

        result = h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == "from_outer"

    def test_handler_performs_same_effect_caught_by_outer(self):
        """Handler for effect_a performs effect_a -> caught by outer handler."""
        effect_a: Effect[[], str] = effect("a")

        h_outer: Handler[str] = create_handler(effect_a)

        @h_outer.on(effect_a)
        def _handle_a_outer(k: Resume[str, str]):
            return k("outer")

        h_inner: Handler[str] = create_handler(effect_a)

        @h_inner.on(effect_a)
        def _handle_a_inner(k: Resume[str, str]):
            val = effect_a()
            return k(val)

        result = h_outer(lambda: h_inner(lambda: effect_a()))
        assert result == "outer"

    def test_effect_before_resume(self):
        effect_a: Effect[[], int] = effect("a")
        effect_b: Effect[[], int] = effect("b")

        h_outer: Handler[int] = create_handler(effect_a)

        @h_outer.on(effect_a)
        def _handle_a(k: Resume[int, int]):
            return k(42)

        h_inner: Handler[int] = create_handler(effect_b)

        @h_inner.on(effect_b)
        def _handle_b(k: Resume[int, int]):
            val = effect_a()
            return k(val + 1)

        result = h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == 43

    def test_effect_after_resume(self):
        effect_a: Effect[[], int] = effect("a")
        effect_b: Effect[[], int] = effect("b")
        log: list[str] = []

        h_outer: Handler[int] = create_handler(effect_a)

        @h_outer.on(effect_a)
        def _handle_a(k: Resume[int, int]):
            log.append("outer_a")
            return k(10)

        h_inner: Handler[int] = create_handler(effect_b)

        @h_inner.on(effect_b)
        def _handle_b(k: Resume[int, int]):
            result = k(0)
            val = effect_a()
            log.append(f"inner_b_got_{val}")
            return result + val

        result = h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == 10
        assert log == ["outer_a", "inner_b_got_10"]

    def test_handler_performs_multiple_outer_effects(self):
        effect_a: Effect[[], int] = effect("a")
        effect_b: Effect[[], int] = effect("b")
        effect_c: Effect[[], int] = effect("c")

        h_outer: Handler[int] = create_handler(effect_a, effect_b)

        @h_outer.on(effect_a)
        def _handle_a(k: Resume[int, int]):
            return k(10)

        @h_outer.on(effect_b)
        def _handle_b(k: Resume[int, int]):
            return k(20)

        h_inner: Handler[int] = create_handler(effect_c)

        @h_inner.on(effect_c)
        def _handle_c(k: Resume[int, int]):
            a_val = effect_a()
            b_val = effect_b()
            return k(a_val + b_val)

        result = h_outer(lambda: h_inner(lambda: effect_c()))
        assert result == 30

    def test_nested_three_levels(self):
        effect_a: Effect[[], str] = effect("a")
        effect_b: Effect[[], str] = effect("b")
        effect_c: Effect[[], str] = effect("c")

        h_a: Handler[str] = create_handler(effect_a)

        @h_a.on(effect_a)
        def _handle_a(k: Resume[str, str]):
            return k("A")

        h_b: Handler[str] = create_handler(effect_b)

        @h_b.on(effect_b)
        def _handle_b(k: Resume[str, str]):
            val = effect_a()
            return k(val + "_B")

        h_c: Handler[str] = create_handler(effect_c)

        @h_c.on(effect_c)
        def _handle_c(k: Resume[str, str]):
            return k("C")

        result = h_a(lambda: h_b(lambda: h_c(lambda: effect_b())))
        assert result == "A_B"

    def test_outer_handler_aborts(self):
        effect_a: Effect[[], str] = effect("a")
        effect_b: Effect[[], str] = effect("b")
        inner_k_called = False

        h_outer: Handler[str] = create_handler(effect_a)

        @h_outer.on(effect_a)
        def _handle_a(k: Resume[str, str]):
            return "aborted"

        h_inner: Handler[str] = create_handler(effect_b)

        @h_inner.on(effect_b)
        def _handle_b(k: Resume[str, str]):
            nonlocal inner_k_called
            effect_a()
            inner_k_called = True
            return k("unreachable")

        result = h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == "aborted"
        assert not inner_k_called

    def test_shallow_handler_fn_performs_outer_effect(self):
        effect_a: Effect[[], str] = effect("a")
        effect_b: Effect[[], str] = effect("b")

        h_outer: Handler[str] = create_handler(effect_a)

        @h_outer.on(effect_a)
        def _handle_a(k: Resume[str, str]):
            return k("outer_a")

        h_inner: Handler[str] = create_handler(effect_b, shallow=True)

        @h_inner.on(effect_b)
        def _handle_b(k: Resume[str, str]):
            val = effect_a()
            return k(val)

        result = h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == "outer_a"

    def test_handler_effect_not_handled_raises(self):
        effect_a: Effect[[], str] = effect("a")
        effect_b: Effect[[], str] = effect("b")

        h: Handler[str] = create_handler(effect_b)

        @h.on(effect_b)
        def _handle_b(k: Resume[str, str]):
            effect_a()
            return k("unreachable")

        with pytest.raises(EffectNotHandledError):
            h(lambda: effect_b())

    def test_handler_effect_with_args(self):
        effect_add: Effect[[int, int], int] = effect("add")
        effect_compute: Effect[[], int] = effect("compute")

        h_outer: Handler[int] = create_handler(effect_add)

        @h_outer.on(effect_add)
        def _handle_add(k: Resume[int, int], a: int, b: int):
            return k(a + b)

        h_inner: Handler[int] = create_handler(effect_compute)

        @h_inner.on(effect_compute)
        def _handle_compute(k: Resume[int, int]):
            result = effect_add(3, 7)
            return k(result)

        result = h_outer(lambda: h_inner(lambda: effect_compute()))
        assert result == 10

    def test_caller_effects_still_work_after_handler_effect(self):
        effect_a: Effect[[], int] = effect("a")
        effect_b: Effect[[], int] = effect("b")
        call_count = 0

        h_outer: Handler[int] = create_handler(effect_a)

        @h_outer.on(effect_a)
        def _handle_a(k: Resume[int, int]):
            return k(100)

        h_inner: Handler[int] = create_handler(effect_b)

        @h_inner.on(effect_b)
        def _handle_b(k: Resume[int, int]):
            nonlocal call_count
            call_count += 1
            if call_count == 1:
                val = effect_a()
                return k(val)
            else:
                return k(call_count)

        def caller() -> int:
            first = effect_b()
            second = effect_b()
            return first + second

        result = h_outer(lambda: h_inner(caller))
        assert result == 102


# ---------------------------------------------------------------------------
# Async handler performs outer effect
# ---------------------------------------------------------------------------


class TestAsyncHandlerPerformsEffect:
    @pytest.mark.asyncio
    async def test_async_handler_performs_outer_effect(self):
        effect_a: Effect[[], str] = effect("a")
        effect_b: Effect[[], str] = effect("b")

        h_outer: AsyncHandler[str] = create_async_handler(effect_a)

        @h_outer.on(effect_a)
        async def _handle_a(k: ResumeAsync[str, str]):
            return await k("async_outer")

        h_inner: AsyncHandler[str] = create_async_handler(effect_b)

        @h_inner.on(effect_b)
        async def _handle_b(k: ResumeAsync[str, str]):
            val = effect_a()
            return await k(val)

        result = await h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == "async_outer"

    @pytest.mark.asyncio
    async def test_async_handler_performs_same_effect(self):
        effect_a: Effect[[], str] = effect("a")

        h_outer: AsyncHandler[str] = create_async_handler(effect_a)

        @h_outer.on(effect_a)
        async def _handle_a_outer(k: ResumeAsync[str, str]):
            return await k("outer")

        h_inner: AsyncHandler[str] = create_async_handler(effect_a)

        @h_inner.on(effect_a)
        async def _handle_a_inner(k: ResumeAsync[str, str]):
            val = effect_a()
            return await k(val)

        result = await h_outer(lambda: h_inner(lambda: effect_a()))
        assert result == "outer"

    @pytest.mark.asyncio
    async def test_async_effect_before_resume(self):
        effect_a: Effect[[], int] = effect("a")
        effect_b: Effect[[], int] = effect("b")

        h_outer: AsyncHandler[int] = create_async_handler(effect_a)

        @h_outer.on(effect_a)
        async def _handle_a(k: ResumeAsync[int, int]):
            return await k(42)

        h_inner: AsyncHandler[int] = create_async_handler(effect_b)

        @h_inner.on(effect_b)
        async def _handle_b(k: ResumeAsync[int, int]):
            val = effect_a()
            return await k(val + 1)

        result = await h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == 43

    @pytest.mark.asyncio
    async def test_async_outer_handler_aborts(self):
        effect_a: Effect[[], str] = effect("a")
        effect_b: Effect[[], str] = effect("b")
        inner_k_called = False

        h_outer: AsyncHandler[str] = create_async_handler(effect_a)

        @h_outer.on(effect_a)
        async def _handle_a(k: ResumeAsync[str, str]):
            return "aborted"

        h_inner: AsyncHandler[str] = create_async_handler(effect_b)

        @h_inner.on(effect_b)
        async def _handle_b(k: ResumeAsync[str, str]):
            nonlocal inner_k_called
            effect_a()
            inner_k_called = True
            return await k("unreachable")

        result = await h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == "aborted"
        assert not inner_k_called


# ---------------------------------------------------------------------------
# Mixed sync/async handler effects
# ---------------------------------------------------------------------------


class TestMixedHandlerEffects:
    @pytest.mark.asyncio
    async def test_sync_handler_fn_performs_effect_caught_by_async_outer(self):
        effect_a: Effect[[], str] = effect("a")
        effect_b: Effect[[], str] = effect("b")

        h_outer: AsyncHandler[str] = create_async_handler(effect_a)

        @h_outer.on(effect_a)
        async def _handle_a(k: ResumeAsync[str, str]):
            return await k("async_outer")

        h_inner: Handler[str] = create_handler(effect_b)

        @h_inner.on(effect_b)
        def _handle_b(k: Resume[str, str]):
            val = effect_a()
            return k(val)

        result = await h_outer(lambda: h_inner(lambda: effect_b()))
        assert result == "async_outer"
