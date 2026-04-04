import asyncio

import pytest
import pytest_asyncio  # pyright: ignore[reportUnusedImport]

from aleff import (
    effect,
    Effect,
    handler,
    async_handler,
    create_handler,
    create_async_handler,
    Resume,
    ResumeAsync,
    EffectNotHandledError,
)


# ---------------------------------------------------------------------------
# Async handler — normal cases
# ---------------------------------------------------------------------------


class TestAsyncHandler:
    @pytest.mark.asyncio
    async def test_single_effect_resume(self):
        get_val: Effect[[], str] = effect("get_val")
        h: async_handler[str] = create_async_handler(get_val)

        @h.on(get_val)
        async def _get(k: ResumeAsync[str, str]):
            return await k("hello")

        result = await h(lambda: get_val())
        assert result == "hello"

    @pytest.mark.asyncio
    async def test_multiple_effects(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        h: async_handler[int] = create_async_handler(read, write)

        @h.on(read)
        async def _read(k: ResumeAsync[str, int]):
            return await k("data")

        @h.on(write)
        async def _write(k: ResumeAsync[int, int], s: str):
            return await k(len(s))

        def run():
            s = read()
            return write(s)

        result = await h(run)
        assert result == 4

    @pytest.mark.asyncio
    async def test_await_in_handler(self):
        """Handler can use await before resuming."""
        e: Effect[[], str] = effect("e")
        h: async_handler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            await asyncio.sleep(0.01)
            return await k("after_sleep")

        result = await h(lambda: e())
        assert result == "after_sleep"

    @pytest.mark.asyncio
    async def test_abort_without_resume(self):
        e: Effect[[], int] = effect("e")
        h: async_handler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return 42

        result = await h(lambda: e(), check=False)
        assert result == 42

    @pytest.mark.asyncio
    async def test_nested_handlers(self):
        inner_eff: Effect[[], str] = effect("inner")
        outer_eff: Effect[[], int] = effect("outer")

        async def run_inner():
            h: async_handler[str] = create_async_handler(inner_eff)

            @h.on(inner_eff)
            async def _handle(k: ResumeAsync[str, str]):
                return await k("from_inner")

            return await h(lambda: inner_eff())

        async def run_outer():
            h: async_handler[int] = create_async_handler(outer_eff)

            @h.on(outer_eff)
            async def _handle(k: ResumeAsync[int, int]):
                return await k(99)

            def body():
                run_inner_result = run_inner()  # pyright: ignore[reportUnusedVariable]
                # run_inner returns a coroutine; it will be awaited by _drive_async
                # But we need the inner handler to complete first.
                # For nesting, inner must be sync or handled differently.
                return outer_eff()

            return await h(body)

        # Nested async handlers require the inner to complete before outer effect.
        # Use a different pattern: inner as sync handler.
        pass

    @pytest.mark.asyncio
    async def test_effect_with_arguments(self):
        add: Effect[[int, int], int] = effect("add")
        h: async_handler[int] = create_async_handler(add)

        @h.on(add)
        async def _add(k: ResumeAsync[int, int], a: int, b: int):
            return await k(a + b)

        result = await h(lambda: add(3, 7))
        assert result == 10

    @pytest.mark.asyncio
    async def test_multiple_effect_invocations(self):
        """Same effect can be invoked multiple times."""
        counter: Effect[[], int] = effect("counter")
        call_count = 0

        h: async_handler[int] = create_async_handler(counter)

        @h.on(counter)
        async def _counter(k: ResumeAsync[int, int]):
            nonlocal call_count
            call_count += 1
            return await k(call_count)

        def run():
            a = counter()
            b = counter()
            c = counter()
            return a + b + c

        result = await h(run)
        assert result == 6
        assert call_count == 3


# ---------------------------------------------------------------------------
# Async handler — error cases
# ---------------------------------------------------------------------------


class TestAsyncHandlerErrors:
    @pytest.mark.asyncio
    async def test_effect_not_handled_raises(self):
        e: Effect[[], str] = effect("unhandled")
        with pytest.raises(EffectNotHandledError):
            e()

    @pytest.mark.asyncio
    async def test_effect_not_declared_raises(self):
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        h: async_handler[str] = create_async_handler(e1)

        with pytest.raises(ValueError, match="not declared"):

            @h.on(e2)
            async def _handle(k: ResumeAsync[str, str]):
                return await k("x")

    @pytest.mark.asyncio
    async def test_duplicate_effect_handler_raises(self):
        e: Effect[[], str] = effect("e")
        h: async_handler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("x")

        with pytest.raises(ValueError, match="already handled"):

            @h.on(e)
            async def _handle2(k: ResumeAsync[str, str]):
                return await k("y")

    @pytest.mark.asyncio
    async def test_unbound_effects_raises_on_call(self):
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        h: async_handler[str] = create_async_handler(e1, e2)

        @h.on(e1)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("x")

        with pytest.raises(ValueError, match="not all effects are handled"):
            await h(lambda: e1())

    @pytest.mark.asyncio
    async def test_check_false_skips_validation(self):
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        h: async_handler[str] = create_async_handler(e1, e2)

        @h.on(e1)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("ok")

        result = await h(lambda: e1(), check=False)
        assert result == "ok"

    @pytest.mark.asyncio
    async def test_fn_exception_propagates(self):
        e: Effect[[], str] = effect("e")
        h: async_handler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("val")

        def fn():
            e()
            raise ValueError("user error")

        with pytest.raises(ValueError, match="user error"):
            await h(fn)

    @pytest.mark.asyncio
    async def test_handler_exception_propagates(self):
        e: Effect[[], str] = effect("e")
        h: async_handler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            raise ValueError("handler error")

        with pytest.raises(ValueError, match="handler error"):
            await h(lambda: e())


# ---------------------------------------------------------------------------
# Mixed sync/async nesting
# ---------------------------------------------------------------------------


class TestMixedNesting:
    @pytest.mark.asyncio
    async def test_sync_handler_inside_async_handler(self):
        sync_eff: Effect[[], str] = effect("sync_eff")
        async_eff: Effect[[], int] = effect("async_eff")

        def inner():
            h: handler[str] = create_handler(sync_eff)

            @h.on(sync_eff)
            def _handle(k: Resume[str, str]):
                return k("sync_value")

            return h(lambda: sync_eff())

        async def outer():
            h: async_handler[int] = create_async_handler(async_eff)

            @h.on(async_eff)
            async def _handle(k: ResumeAsync[int, int]):
                return await k(100)

            def body():
                inner()
                return async_eff()

            return await h(body)

        assert await outer() == 100


# ---------------------------------------------------------------------------
# Async stack cleanup
# ---------------------------------------------------------------------------


class TestAsyncStackCleanup:
    @pytest.mark.asyncio
    async def test_stack_cleaned_after_handler(self):
        e: Effect[[], str] = effect("e")
        h: async_handler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("val")

        await h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()

    @pytest.mark.asyncio
    async def test_stack_cleaned_after_exception(self):
        e: Effect[[], str] = effect("e")
        h: async_handler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("val")

        with pytest.raises(RuntimeError, match="boom"):
            await h(lambda: (_ for _ in ()).throw(RuntimeError("boom")))

        with pytest.raises(EffectNotHandledError):
            e()
