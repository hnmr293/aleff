"""Tests for shallow handlers (multishot implementation).

A shallow handler handles an effect only once, then is removed from the
handler stack.  Subsequent occurrences of the same effect are NOT caught
by this handler unless it is explicitly re-installed.
"""

import asyncio

import pytest
import pytest_asyncio  # pyright: ignore[reportUnusedImport]

from aleff import (
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
# Sync shallow handler -- normal cases
# ---------------------------------------------------------------------------


class TestShallowSyncNormal:
    def test_single_effect_resume(self):
        """Shallow handler handles a single effect occurrence and resumes."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("hello")

        result = h(lambda: e())
        assert result == "hello"

    def test_effect_with_arguments(self):
        """Shallow handler receives effect arguments correctly."""
        add: Effect[[int, int], int] = effect("add")
        h: Handler[int] = create_handler(add, shallow=True)

        @h.on(add)
        def _handle(k: Resume[int, int], a: int, b: int):
            return k(a + b)

        result = h(lambda: add(3, 7))
        assert result == 10

    def test_abort_without_resume(self):
        """Shallow handler can abort (return without calling k)."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 42

        result = h(lambda: e(), check=False)
        assert result == 42

    def test_effect_not_triggered(self):
        """When the effect is never triggered, handler returns caller result."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("unused")

        result = h(lambda: "direct")
        assert result == "direct"

    def test_second_occurrence_not_caught(self):
        """After handling once, the same effect is NOT caught again."""
        e: Effect[[], int] = effect("e")
        call_count = 0

        h_shallow: Handler[int] = create_handler(e, shallow=True)

        @h_shallow.on(e)
        def _handle(k: Resume[int, int]):
            nonlocal call_count
            call_count += 1
            return k(1)

        h_outer: Handler[int] = create_handler(e)

        @h_outer.on(e)
        def _outer(k: Resume[int, int]):
            return k(99)

        def run():
            a = e()  # caught by shallow
            b = e()  # NOT caught by shallow -> caught by outer
            return a + b

        result = h_outer(lambda: h_shallow(run))
        assert call_count == 1
        assert result == 100  # 1 + 99

    def test_multiple_effects_all_removed_after_one_fires(self):
        """When a shallow handler has multiple effects, ALL are removed after one fires."""
        e1: Effect[[], int] = effect("e1")
        e2: Effect[[], int] = effect("e2")

        h_shallow: Handler[int] = create_handler(e1, e2, shallow=True)

        @h_shallow.on(e1)
        def _handle1(k: Resume[int, int]):
            return k(10)

        @h_shallow.on(e2)
        def _handle2(k: Resume[int, int]):
            return k(20)

        h_outer: Handler[int] = create_handler(e1, e2)

        @h_outer.on(e1)
        def _outer1(k: Resume[int, int]):
            return k(100)

        @h_outer.on(e2)
        def _outer2(k: Resume[int, int]):
            return k(200)

        def run():
            a = e1()  # caught by shallow
            b = e2()  # shallow already removed -> caught by outer
            return a + b

        result = h_outer(lambda: h_shallow(run))
        assert result == 210  # 10 + 200

    def test_shallow_inside_deep_nested(self):
        """Shallow handler nested inside deep handler.
        After shallow is removed, deep handler catches subsequent effects."""
        e: Effect[[], int] = effect("e")

        h_deep: Handler[int] = create_handler(e)

        @h_deep.on(e)
        def _deep(k: Resume[int, int]):
            return k(100)

        h_shallow: Handler[int] = create_handler(e, shallow=True)

        @h_shallow.on(e)
        def _shallow(k: Resume[int, int]):
            return k(1)

        def run():
            a = e()  # caught by shallow -> 1
            b = e()  # shallow gone, caught by deep -> 100
            return a + b

        result = h_deep(lambda: h_shallow(run))
        assert result == 101

    def test_deep_inside_shallow_nested(self):
        """Deep handler nested inside shallow handler.
        The inner deep handler catches its effect normally."""
        e_shallow: Effect[[], int] = effect("e_shallow")
        e_deep: Effect[[], int] = effect("e_deep")

        h_shallow: Handler[int] = create_handler(e_shallow, shallow=True)

        @h_shallow.on(e_shallow)
        def _shallow(k: Resume[int, int]):
            return k(1)

        h_deep: Handler[int] = create_handler(e_deep)

        @h_deep.on(e_deep)
        def _deep(k: Resume[int, int]):
            return k(10)

        def run():
            a = e_deep()  # caught by deep
            b = e_deep()  # caught by deep again (deep re-installs)
            c = e_shallow()  # caught by shallow
            return a + b + c

        result = h_shallow(lambda: h_deep(run))
        assert result == 21  # 10 + 10 + 1

    def test_multiple_shallow_handlers_same_effect_stacked(self):
        """Multiple shallow handlers for the same effect: only the innermost is removed."""
        e: Effect[[], int] = effect("e")

        h_outer: Handler[int] = create_handler(e, shallow=True)

        @h_outer.on(e)
        def _outer(k: Resume[int, int]):
            return k(100)

        h_inner: Handler[int] = create_handler(e, shallow=True)

        @h_inner.on(e)
        def _inner(k: Resume[int, int]):
            return k(1)

        def run():
            a = e()  # caught by inner shallow
            b = e()  # inner removed, caught by outer shallow
            return a + b

        result = h_outer(lambda: h_inner(run))
        assert result == 101  # 1 + 100

    def test_shallow_handler_manual_reinstall(self):
        """Handler function can manually re-install a shallow handler."""
        e: Effect[[], int] = effect("e")
        call_count = 0

        def make_handler():
            h: Handler[int] = create_handler(e, shallow=True)

            @h.on(e)
            def _handle(k: Resume[int, int]):
                nonlocal call_count
                call_count += 1
                # Re-install by wrapping the continuation with a new handler
                return h(lambda: k(call_count))

            return h

        h = make_handler()

        def run():
            a = e()
            b = e()
            c = e()
            return a + b + c

        result = h(run)
        assert result == 6  # 1 + 2 + 3
        assert call_count == 3

    def test_recursive_computation(self):
        """Shallow handler in a recursive computation."""
        e: Effect[[], int] = effect("e")

        h_deep: Handler[int] = create_handler(e)

        @h_deep.on(e)
        def _deep(k: Resume[int, int]):
            return k(0)

        h_shallow: Handler[int] = create_handler(e, shallow=True)

        @h_shallow.on(e)
        def _shallow(k: Resume[int, int]):
            return k(1)

        def recurse(n: int) -> int:
            if n == 0:
                return 0
            return e() + recurse(n - 1)

        # Only the first e() call hits shallow (returns 1), rest hit deep (return 0)
        result = h_deep(lambda: h_shallow(lambda: recurse(5)))
        assert result == 1


# ---------------------------------------------------------------------------
# Sync shallow handler -- error cases
# ---------------------------------------------------------------------------


class TestShallowSyncErrors:
    def test_second_occurrence_unhandled_raises(self):
        """After shallow handler is removed, unhandled effect raises."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return k(1)

        def run():
            e()  # caught by shallow
            e()  # no handler -> EffectNotHandledError
            pytest.fail("unreachable")

        with pytest.raises(EffectNotHandledError):
            h(run)

    def test_handler_exception_propagates(self):
        """Exception in shallow handler function propagates."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            raise ValueError("handler error")

        with pytest.raises(ValueError, match="handler error"):
            h(lambda: e())

    def test_resumed_computation_exception_propagates(self):
        """Exception in resumed computation propagates correctly."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("val")

        def run():
            e()
            raise ValueError("computation error")

        with pytest.raises(ValueError, match="computation error"):
            h(run)


# ---------------------------------------------------------------------------
# Sync shallow handler -- stack cleanup
# ---------------------------------------------------------------------------


class TestShallowSyncStackCleanup:
    def test_stack_cleaned_after_handler(self):
        """Handler stack is cleaned after shallow handler completes."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("val")

        h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()

    def test_stack_cleaned_after_exception(self):
        """Stack cleaned even when exception occurs in shallow handler."""
        e: Effect[[], str] = effect("e")
        h: Handler[str] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            raise ValueError("oops")

        with pytest.raises(ValueError):
            h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()

    def test_stack_cleaned_after_abort(self):
        """Stack cleaned when shallow handler aborts."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 0

        h(lambda: e(), check=False)

        with pytest.raises(EffectNotHandledError):
            e()


# ---------------------------------------------------------------------------
# Sync shallow handler -- check parameter
# ---------------------------------------------------------------------------


class TestShallowSyncCheck:
    def test_check_false_skips_validation(self):
        """check=False works with shallow handlers."""
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        h: Handler[str] = create_handler(e1, e2, shallow=True)

        @h.on(e1)
        def _handle(k: Resume[str, str]):
            return k("ok")

        result = h(lambda: e1(), check=False)
        assert result == "ok"


# ---------------------------------------------------------------------------
# Sync shallow handler -- multi-shot
# ---------------------------------------------------------------------------


class TestShallowSyncMultiShot:
    def test_multishot_resume_shallow(self):
        """Multi-shot resume with shallow handler: each shot does NOT have the handler."""
        e: Effect[[], int] = effect("e")

        h_deep: Handler[list[int]] = create_handler(e)

        @h_deep.on(e)
        def _deep(k: Resume[int, list[int]]):
            return k(99)

        h_shallow: Handler[list[int]] = create_handler(e, shallow=True)

        @h_shallow.on(e)
        def _shallow(k: Resume[int, list[int]]):
            r1 = k(1)
            r2 = k(2)
            return r1 + r2

        def run():
            a = e()  # caught by shallow, resumes with 1 and 2
            b = e()  # shallow gone, caught by deep -> 99
            return [a + b]

        result = h_deep(lambda: h_shallow(run))
        assert result == [100, 101]  # [1+99, 2+99]

    def test_multishot_shallow_abort(self):
        """Multi-shot abort with shallow handler."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e, shallow=True)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return -1  # abort

        result = h(lambda: e())
        assert result == -1


# ---------------------------------------------------------------------------
# Async shallow handler -- normal cases
# ---------------------------------------------------------------------------


class TestShallowAsyncNormal:
    @pytest.mark.asyncio
    async def test_single_effect_resume(self):
        """Async shallow handler handles a single effect and resumes."""
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e, shallow=True)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("hello")

        result = await h(lambda: e())
        assert result == "hello"

    @pytest.mark.asyncio
    async def test_effect_with_arguments(self):
        """Async shallow handler receives effect arguments."""
        add: Effect[[int, int], int] = effect("add")
        h: AsyncHandler[int] = create_async_handler(add, shallow=True)

        @h.on(add)
        async def _handle(k: ResumeAsync[int, int], a: int, b: int):
            return await k(a + b)

        result = await h(lambda: add(3, 7))
        assert result == 10

    @pytest.mark.asyncio
    async def test_abort_without_resume(self):
        """Async shallow handler abort."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e, shallow=True)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return 42

        result = await h(lambda: e(), check=False)
        assert result == 42

    @pytest.mark.asyncio
    async def test_await_in_handler(self):
        """Async shallow handler can await before resuming."""
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e, shallow=True)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            await asyncio.sleep(0.001)
            return await k("after_sleep")

        result = await h(lambda: e())
        assert result == "after_sleep"

    @pytest.mark.asyncio
    async def test_second_occurrence_not_caught(self):
        """Async: after handling once, the same effect is NOT caught again."""
        e: Effect[[], int] = effect("e")
        call_count = 0

        h_shallow: AsyncHandler[int] = create_async_handler(e, shallow=True)

        @h_shallow.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            nonlocal call_count
            call_count += 1
            return await k(1)

        h_outer: AsyncHandler[int] = create_async_handler(e)

        @h_outer.on(e)
        async def _outer(k: ResumeAsync[int, int]):
            return await k(99)

        def run():
            a = e()
            b = e()
            return a + b

        result = await h_outer(lambda: h_shallow(run))
        assert call_count == 1
        assert result == 100

    @pytest.mark.asyncio
    async def test_multiple_effects_all_removed(self):
        """Async: all effects removed after one fires."""
        e1: Effect[[], int] = effect("e1")
        e2: Effect[[], int] = effect("e2")

        h_shallow: AsyncHandler[int] = create_async_handler(e1, e2, shallow=True)

        @h_shallow.on(e1)
        async def _handle1(k: ResumeAsync[int, int]):
            return await k(10)

        @h_shallow.on(e2)
        async def _handle2(k: ResumeAsync[int, int]):
            return await k(20)

        h_outer: AsyncHandler[int] = create_async_handler(e1, e2)

        @h_outer.on(e1)
        async def _outer1(k: ResumeAsync[int, int]):
            return await k(100)

        @h_outer.on(e2)
        async def _outer2(k: ResumeAsync[int, int]):
            return await k(200)

        def run():
            a = e1()
            b = e2()
            return a + b

        result = await h_outer(lambda: h_shallow(run))
        assert result == 210

    @pytest.mark.asyncio
    async def test_shallow_inside_deep(self):
        """Async shallow inside deep: deep catches after shallow removed."""
        e: Effect[[], int] = effect("e")

        h_deep: AsyncHandler[int] = create_async_handler(e)

        @h_deep.on(e)
        async def _deep(k: ResumeAsync[int, int]):
            return await k(100)

        h_shallow: AsyncHandler[int] = create_async_handler(e, shallow=True)

        @h_shallow.on(e)
        async def _shallow(k: ResumeAsync[int, int]):
            return await k(1)

        def run():
            a = e()
            b = e()
            return a + b

        result = await h_deep(lambda: h_shallow(run))
        assert result == 101


# ---------------------------------------------------------------------------
# Async shallow handler -- error cases
# ---------------------------------------------------------------------------


class TestShallowAsyncErrors:
    @pytest.mark.asyncio
    async def test_second_occurrence_unhandled_raises(self):
        """Async: unhandled after shallow removal raises."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e, shallow=True)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return await k(1)

        def run():
            e()
            e()
            pytest.fail("unreachable")

        with pytest.raises(EffectNotHandledError):
            await h(run)

    @pytest.mark.asyncio
    async def test_handler_exception_propagates(self):
        """Async shallow handler exception propagates."""
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e, shallow=True)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            raise ValueError("handler error")

        with pytest.raises(ValueError, match="handler error"):
            await h(lambda: e())

    @pytest.mark.asyncio
    async def test_resumed_computation_exception_propagates(self):
        """Async: exception in resumed computation propagates."""
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e, shallow=True)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("val")

        def run():
            e()
            raise ValueError("computation error")

        with pytest.raises(ValueError, match="computation error"):
            await h(run)


# ---------------------------------------------------------------------------
# Async shallow handler -- stack cleanup
# ---------------------------------------------------------------------------


class TestShallowAsyncStackCleanup:
    @pytest.mark.asyncio
    async def test_stack_cleaned_after_handler(self):
        """Async: handler stack cleaned after shallow handler completes."""
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e, shallow=True)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("val")

        await h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()

    @pytest.mark.asyncio
    async def test_stack_cleaned_after_exception(self):
        """Async: stack cleaned after exception."""
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e, shallow=True)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            raise ValueError("oops")

        with pytest.raises(ValueError):
            await h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()


# ---------------------------------------------------------------------------
# Async shallow handler -- multi-shot
# ---------------------------------------------------------------------------


class TestShallowAsyncMultiShot:
    @pytest.mark.asyncio
    async def test_multishot_resume_shallow(self):
        """Async multi-shot with shallow: each shot does NOT have the handler."""
        e: Effect[[], int] = effect("e")

        h_deep: AsyncHandler[list[int]] = create_async_handler(e)

        @h_deep.on(e)
        async def _deep(k: ResumeAsync[int, list[int]]):
            return await k(99)

        h_shallow: AsyncHandler[list[int]] = create_async_handler(e, shallow=True)

        @h_shallow.on(e)
        async def _shallow(k: ResumeAsync[int, list[int]]):
            r1 = await k(1)
            r2 = await k(2)
            return r1 + r2

        def run():
            a = e()
            b = e()
            return [a + b]

        result = await h_deep(lambda: h_shallow(run))
        assert result == [100, 101]


# ---------------------------------------------------------------------------
# Mixed sync/async shallow handler nesting
# ---------------------------------------------------------------------------


class TestShallowMixedNesting:
    @pytest.mark.asyncio
    async def test_shallow_and_deep_interleaved(self):
        """Interleaved shallow and deep handlers for different effects."""
        e1: Effect[[], int] = effect("e1")
        e2: Effect[[], int] = effect("e2")

        h_deep: AsyncHandler[int] = create_async_handler(e1)

        @h_deep.on(e1)
        async def _deep(k: ResumeAsync[int, int]):
            return await k(10)

        h_shallow: AsyncHandler[int] = create_async_handler(e2, shallow=True)

        @h_shallow.on(e2)
        async def _shallow(k: ResumeAsync[int, int]):
            return await k(1)

        def run():
            a = e1()  # deep -> 10
            b = e2()  # shallow -> 1
            c = e1()  # deep again -> 10
            return a + b + c

        result = await h_deep(lambda: h_shallow(run))
        assert result == 21

    @pytest.mark.asyncio
    async def test_sync_shallow_inside_async_deep(self):
        """Sync shallow handler inside async deep handler for the same effect.
        After the shallow handler is removed, the second effect should be
        relayed to the async deep handler."""
        e: Effect[[], int] = effect("e")

        h_deep: AsyncHandler[int] = create_async_handler(e)

        @h_deep.on(e)
        async def _deep(k: ResumeAsync[int, int]):
            return await k(100)

        h_shallow: Handler[int] = create_handler(e, shallow=True)

        @h_shallow.on(e)
        def _shallow(k: Resume[int, int]):
            return k(1)

        def run():
            a = e()  # caught by shallow -> 1
            b = e()  # shallow gone, caught by deep -> 100
            return a + b

        result = await h_deep(lambda: h_shallow(run))
        assert result == 101
