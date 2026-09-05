import asyncio
from collections.abc import Generator

import pytest

from aleff import (
    effect,
    Effect,
    Handler,
    AsyncHandler,
    create_handler,
    create_async_handler,
    Resume,
    ResumeAsync,
    EffectNotHandledError,
)


pytestmark = pytest.mark.publish_wheel


class _TaskProtocolAwaitable:
    """Awaitable that verifies Task-style resumption after its Future completes."""

    def __init__(self, value: int) -> None:
        loop = asyncio.get_running_loop()
        self._future: asyncio.Future[int] = loop.create_future()
        loop.call_soon(self._future.set_result, value)

    def __await__(self) -> Generator[asyncio.Future[int], None, int]:
        if not self._future.done():
            self._future._asyncio_future_blocking = True
            resume_value = yield self._future
            assert resume_value is None
        return self._future.result()


# ---------------------------------------------------------------------------
# Async handler — normal cases
# ---------------------------------------------------------------------------


class TestAsyncHandler:
    @pytest.mark.asyncio
    async def test_single_effect_resume(self):
        get_val: Effect[[], str] = effect("get_val")
        h: AsyncHandler[str] = create_async_handler(get_val)

        @h.on(get_val)
        async def _get(k: ResumeAsync[str, str]):
            return await k("hello")

        result = await h(lambda: get_val())
        assert result == "hello"

    @pytest.mark.asyncio
    async def test_multiple_effects(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        h: AsyncHandler[int] = create_async_handler(read, write)

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
        h: AsyncHandler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            await asyncio.sleep(0.01)
            return await k("after_sleep")

        result = await h(lambda: e())
        assert result == "after_sleep"

    @pytest.mark.asyncio
    async def test_await_sleep_zero_in_handler(self):
        """sleep(0) in a handler yields and then resumes the continuation."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            await asyncio.sleep(0)
            return await k(1)

        assert await h(lambda: e()) == 1

    @pytest.mark.asyncio
    async def test_await_sleep_zero_in_handler_yields_to_event_loop(self):
        """sleep(0) preserves its contract to yield for one loop iteration."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)
        order: list[str] = []

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            order.append("before_sleep")
            asyncio.get_running_loop().call_soon(order.append, "scheduled")
            await asyncio.sleep(0)
            order.append("after_sleep")
            return await k(1)

        assert await h(lambda: e()) == 1
        assert order == ["before_sleep", "scheduled", "after_sleep"]

    @pytest.mark.asyncio
    async def test_await_negative_sleep_in_handler(self):
        """A non-positive sleep delay follows asyncio's zero-delay path."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            await asyncio.sleep(-1)
            return await k(1)

        assert await h(lambda: e()) == 1

    @pytest.mark.asyncio
    async def test_awaited_future_exception_is_caught_in_handler(self):
        """An exception from an awaited Future is thrown into the handler coroutine."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)
        future: asyncio.Future[None] = asyncio.get_running_loop().create_future()

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            try:
                await future
            except ValueError as exc:
                assert str(exc) == "boom"
                await asyncio.sleep(0)
                return await k(1)

            pytest.fail("the Future exception was not thrown into the handler")

        asyncio.get_running_loop().call_soon(future.set_exception, ValueError("boom"))
        assert await h(lambda: e()) == 1

    @pytest.mark.asyncio
    async def test_awaited_future_resumes_handler_with_none(self):
        """A completed Future resumes the handler using Task's send(None) protocol."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            value = await _TaskProtocolAwaitable(7)
            return await k(value)

        assert await h(lambda: e()) == 7

    @pytest.mark.asyncio
    @pytest.mark.parametrize("delay", [0, 3600], ids=["bare-yield", "future"])
    async def test_cancellation_during_handler_await_is_propagated_and_finalized(self, delay: float):
        """Cancellation reaches the handler and permits asynchronous finalization."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)
        started = asyncio.Event()
        cancelled = asyncio.Event()
        finalized = asyncio.Event()

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            try:
                started.set()
                await asyncio.sleep(delay)
                return await k(1)
            except asyncio.CancelledError:
                cancelled.set()
                raise
            finally:
                await asyncio.sleep(0)
                finalized.set()

        task = asyncio.create_task(h(lambda: e()))
        await started.wait()
        task.cancel()

        with pytest.raises(asyncio.CancelledError):
            await task

        assert cancelled.is_set()
        assert finalized.is_set()

    @pytest.mark.asyncio
    async def test_abort_without_resume(self):
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return 42

        result = await h(lambda: e(), check=False)
        assert result == 42

    @pytest.mark.asyncio
    async def test_nested_handlers(self):
        """An inner async handler runs to completion inside the outer handler fn.

        The `order` list is a contract, not an artefact of the current implementation:
        nesting means the inner handler finishes before the outer continuation resumes.
        """
        inner_eff: Effect[[], str] = effect("inner")
        outer_eff: Effect[[], int] = effect("outer")
        order: list[str] = []

        async def run_inner():
            h: AsyncHandler[str] = create_async_handler(inner_eff)

            @h.on(inner_eff)
            async def _handle(k: ResumeAsync[str, str]):
                order.append("inner_handle")
                return await k("from_inner")

            order.append("inner_enter")
            result = await h(lambda: inner_eff())
            order.append("inner_exit")
            return result

        async def run_outer():
            h: AsyncHandler[int] = create_async_handler(outer_eff)

            @h.on(outer_eff)
            async def _handle(k: ResumeAsync[int, int]):
                order.append("outer_handle")
                result = await k(len(await run_inner()))
                order.append("outer_resumed")
                return result

            return await h(lambda: outer_eff())

        assert await run_outer() == 10
        assert order == ["outer_handle", "inner_enter", "inner_handle", "inner_exit", "outer_resumed"]

    @pytest.mark.asyncio
    async def test_nested_handlers_in_async_body(self):
        """An inner async handler can run inside an async body, before an outer effect.

        The `order` list is a contract: the inner handler completes before the outer
        effect is performed, and the outer continuation resumes after that.

        Deliberately not mirrored in the one-shot suite: performing an effect from an
        `async def` body raises `EffectNotHandledError` there, with or without an inner
        handler, so the one-shot failure is unrelated to nesting.
        """
        inner_eff: Effect[[], str] = effect("inner_in_body")
        outer_eff: Effect[[], int] = effect("outer_in_body")
        order: list[str] = []

        h_inner: AsyncHandler[str] = create_async_handler(inner_eff)

        @h_inner.on(inner_eff)
        async def _inner(k: ResumeAsync[str, str]):
            order.append("inner_handle")
            return await k("I")

        h_outer: AsyncHandler[str] = create_async_handler(outer_eff)

        @h_outer.on(outer_eff)
        async def _outer(k: ResumeAsync[int, str]):
            order.append("outer_handle")
            result = await k(99)
            order.append("outer_resumed")
            return result

        async def body():
            got = await h_inner(lambda: inner_eff())
            order.append("inner_done")
            n = outer_eff()
            return f"{got}:{n}"

        assert await h_outer(body) == "I:99"
        assert order == ["inner_handle", "inner_done", "outer_handle", "outer_resumed"]

    @pytest.mark.asyncio
    @pytest.mark.parametrize("delay", [0, 0.001], ids=["bare-yield", "future"])
    async def test_async_caller_can_await_before_performing_effect(self, delay: float):
        """An async caller can await either Task yield path before an effect."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return await k(1)

        async def caller():
            await asyncio.sleep(delay)
            return e()

        assert await h(caller) == 1

    @pytest.mark.asyncio
    async def test_awaited_future_exception_is_caught_in_async_caller(self):
        """An exception from an awaited Future is thrown into the caller coroutine."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)
        future: asyncio.Future[None] = asyncio.get_running_loop().create_future()

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return await k(1)

        async def caller():
            try:
                await future
            except ValueError as exc:
                assert str(exc) == "boom"
                await asyncio.sleep(0)
                return e()

            pytest.fail("the Future exception was not thrown into the caller")

        asyncio.get_running_loop().call_soon(future.set_exception, ValueError("boom"))
        assert await h(caller) == 1

    @pytest.mark.asyncio
    async def test_awaited_future_resumes_async_caller_with_none(self):
        """A completed Future resumes the async caller using Task's send(None) protocol."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return await k(1)

        async def caller():
            assert await _TaskProtocolAwaitable(7) == 7
            return e()

        assert await h(caller) == 1

    @pytest.mark.asyncio
    async def test_async_caller_handles_many_consecutive_awaits(self):
        """Await requests are processed without growing the Python call stack."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return await k(1)

        async def caller():
            for _ in range(1500):
                await asyncio.sleep(0)
            return e()

        assert await h(caller) == 1

    @pytest.mark.asyncio
    async def test_async_caller_handles_many_consecutive_await_exceptions(self):
        """Await exceptions are processed without growing the Python call stack."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return await k(1)

        async def caller():
            loop = asyncio.get_running_loop()
            for _ in range(1500):
                future: asyncio.Future[None] = loop.create_future()
                loop.call_soon(future.set_exception, ValueError("boom"))
                try:
                    await future
                except ValueError:
                    pass
            return e()

        assert await h(caller) == 1

    @pytest.mark.asyncio
    @pytest.mark.parametrize("delay", [0, 3600], ids=["bare-yield", "future"])
    async def test_cancellation_during_async_caller_await_is_propagated_and_finalized(self, delay: float):
        """Cancellation reaches an async caller and permits asynchronous finalization."""
        e: Effect[[], int] = effect("e")
        h: AsyncHandler[int] = create_async_handler(e)
        started = asyncio.Event()
        cancelled = asyncio.Event()
        finalized = asyncio.Event()

        @h.on(e)
        async def _handle(k: ResumeAsync[int, int]):
            return await k(1)

        async def caller():
            try:
                started.set()
                await asyncio.sleep(delay)
                return e()
            except asyncio.CancelledError:
                cancelled.set()
                raise
            finally:
                await asyncio.sleep(0)
                finalized.set()

        task = asyncio.create_task(h(caller))
        await started.wait()
        task.cancel()

        with pytest.raises(asyncio.CancelledError):
            await task

        assert cancelled.is_set()
        assert finalized.is_set()

    @pytest.mark.asyncio
    async def test_effect_with_arguments(self):
        add: Effect[[int, int], int] = effect("add")
        h: AsyncHandler[int] = create_async_handler(add)

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

        h: AsyncHandler[int] = create_async_handler(counter)

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
        h: AsyncHandler[str] = create_async_handler(e1)

        with pytest.raises(ValueError, match="not declared"):

            @h.on(e2)
            async def _handle(k: ResumeAsync[str, str]):
                return await k("x")

    @pytest.mark.asyncio
    async def test_duplicate_effect_handler_raises(self):
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e)

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
        h: AsyncHandler[str] = create_async_handler(e1, e2)

        @h.on(e1)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("x")

        with pytest.raises(ValueError, match="not all effects are handled"):
            await h(lambda: e1())

    @pytest.mark.asyncio
    async def test_check_false_skips_validation(self):
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        h: AsyncHandler[str] = create_async_handler(e1, e2)

        @h.on(e1)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("ok")

        result = await h(lambda: e1(), check=False)
        assert result == "ok"

    @pytest.mark.asyncio
    async def test_fn_exception_propagates(self):
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e)

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
        h: AsyncHandler[str] = create_async_handler(e)

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
            h: Handler[str] = create_handler(sync_eff)

            @h.on(sync_eff)
            def _handle(k: Resume[str, str]):
                return k("sync_value")

            return h(lambda: sync_eff())

        async def outer():
            h: AsyncHandler[int] = create_async_handler(async_eff)

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
        h: AsyncHandler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("val")

        await h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()

    @pytest.mark.asyncio
    async def test_stack_cleaned_after_exception(self):
        e: Effect[[], str] = effect("e")
        h: AsyncHandler[str] = create_async_handler(e)

        @h.on(e)
        async def _handle(k: ResumeAsync[str, str]):
            return await k("val")

        with pytest.raises(RuntimeError, match="boom"):
            await h(lambda: (_ for _ in ()).throw(RuntimeError("boom")))

        with pytest.raises(EffectNotHandledError):
            e()
