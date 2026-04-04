"""Tests for multi-shot continuations.

Multi-shot means the handler can call k(value) multiple times,
each time resuming from the same suspension point with independent state.
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
# Multi-shot: basic resume multiple times
# ---------------------------------------------------------------------------


class TestMultiShotBasic:
    def test_resume_twice(self):
        """k can be called twice, each resuming from the same point."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[int] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, int]):
            r1 = k(1)
            r2 = k(2)
            return r1 + r2

        def run():
            x = choose()
            return x * 10

        result = h(run)
        assert result == 30  # 10 + 20

    def test_resume_three_times(self):
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            results: list[int] = []
            for v in [1, 2, 3]:
                results.extend(k(v))
            return results

        def run():
            return [choose() * 10]

        result = h(run)
        assert result == [10, 20, 30]

    def test_resume_zero_times(self):
        """Not calling k at all (abort) should still work."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[int] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, int]):
            return -1  # abort: return without calling k

        result = h(lambda: choose())
        assert result == -1


# ---------------------------------------------------------------------------
# Multi-shot: state independence between shots
# ---------------------------------------------------------------------------


class TestMultiShotStateIndependence:
    def test_local_variables_are_independent(self):
        """Each shot has independent local variable state."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        def run():
            x = choose()
            y = x + 100  # local variable derived from x
            return [y]

        result = h(run)
        assert result == [101, 102]

    def test_mutable_locals_are_shared(self):
        """Mutable objects in locals are shared across shots (Scheme semantics).

        The frame copy shares the same list object because continuations
        share the heap, matching Scheme's call/cc behavior.
        """
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[list[int]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[list[int]]]):
            return k(1) + k(2) + k(3)

        def run():
            items: list[int] = []
            v = choose()
            items.append(v)
            return [items]

        result = h(run)
        # Shared list accumulates across shots
        assert result == [[1, 2, 3], [1, 2, 3], [1, 2, 3]]

    def test_mutable_locals_snapshot_via_copy(self):
        """Shallow-copying a local mutable captures per-shot state."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[list[int]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[list[int]]]):
            return k(1) + k(2) + k(3)

        def run():
            items: list[int] = []
            v = choose()
            items.append(v)
            return [list(items)]  # shallow copy captures current state

        result = h(run)
        assert result == [[1], [1, 2], [1, 2, 3]]

    def test_heap_state_is_shared(self):
        """Mutable objects from outside the continuation are shared (Scheme semantics)."""
        choose: Effect[[], int] = effect("choose")
        shared: list[int] = []

        h: Handler[list[list[int]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[list[int]]]):
            return k(1) + k(2)

        def run():
            v = choose()
            shared.append(v)
            return [list(shared)]  # snapshot of shared state

        result = h(run)
        assert result == [[1], [1, 2]]
        # shared is outside the continuation, so mutations accumulate
        assert shared == [1, 2]
        assert result[1] is not shared


# ---------------------------------------------------------------------------
# Multi-shot: multiple effects
# ---------------------------------------------------------------------------


class TestMultiShotMultipleEffects:
    def test_two_multishot_effects(self):
        """Two multi-shot effects compose (cartesian product)."""
        choose_x: Effect[[], int] = effect("choose_x")
        choose_y: Effect[[], int] = effect("choose_y")
        h: Handler[list[tuple[int, int]]] = create_handler(choose_x, choose_y)

        @h.on(choose_x)
        def _choose_x(k: Resume[int, list[tuple[int, int]]]):
            results: list[tuple[int, int]] = []
            for v in [1, 2]:
                results.extend(k(v))
            return results

        @h.on(choose_y)
        def _choose_y(k: Resume[int, list[tuple[int, int]]]):
            results: list[tuple[int, int]] = []
            for v in [10, 20]:
                results.extend(k(v))
            return results

        def run():
            x = choose_x()
            y = choose_y()
            return [(x, y)]

        result = h(run)
        assert result == [(1, 10), (1, 20), (2, 10), (2, 20)]

    def test_multishot_with_oneshot_effect(self):
        """Multi-shot effect coexists with a one-shot effect."""
        choose: Effect[[], int] = effect("choose")
        log: Effect[[str], None] = effect("log")
        logged: list[str] = []

        h: Handler[list[int]] = create_handler(choose, log)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        @h.on(log)
        def _log(k: Resume[None, list[int]], msg: str):
            logged.append(msg)
            return k(None)

        def run():
            x = choose()
            log(f"chose {x}")
            return [x]

        result = h(run)
        assert result == [1, 2]
        assert logged == ["chose 1", "chose 2"]


# ---------------------------------------------------------------------------
# Multi-shot: nested handlers
# ---------------------------------------------------------------------------


class TestMultiShotNested:
    def test_multishot_with_nested_handler(self):
        """Multi-shot in inner handler, one-shot in outer."""
        choose: Effect[[], int] = effect("choose")
        get_base: Effect[[], int] = effect("get_base")

        h_outer: Handler[list[int]] = create_handler(get_base)

        @h_outer.on(get_base)
        def _get_base(k: Resume[int, list[int]]):
            return k(100)

        def inner():
            h_inner: Handler[list[int]] = create_handler(choose)

            @h_inner.on(choose)
            def _choose(k: Resume[int, list[int]]):
                return [*k(1), *k(2), *k(3)]

            def body():
                base = get_base()
                x = choose()
                return [base + x]

            return h_inner(body)

        result = h_outer(inner)
        assert result == [101, 102, 103]


# ---------------------------------------------------------------------------
# Multi-shot: deep call stacks
# ---------------------------------------------------------------------------


class TestMultiShotDeepCalls:
    def test_multishot_through_nested_function_calls(self):
        """Multi-shot works through multiple levels of function calls."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return [*k(1), *k(2)]

        def level3():
            return choose()

        def level2():
            return level3() + 10

        def level1():
            return [level2() + 100]

        result = h(level1)
        assert result == [111, 112]

    def test_multishot_with_recursion(self):
        """Multi-shot works in recursive computations."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return [*k(0), *k(1)]

        def binary_string(n: int) -> list[int]:
            if n == 0:
                return [0]
            bit = choose()
            rest = binary_string(n - 1)
            return [bit * (2 ** (n - 1)) + r for r in rest]

        # This generates all 2-bit binary numbers
        result = h(lambda: binary_string(2))
        assert result == [0, 1, 2, 3]


# ---------------------------------------------------------------------------
# Multi-shot: stateful handler (post-resume code)
# ---------------------------------------------------------------------------


class TestMultiShotStatefulHandler:
    def test_post_resume_code_runs_per_shot(self):
        """Code after k() in the handler runs for each shot independently."""
        choose: Effect[[], int] = effect("choose")
        post_resume_log: list[str] = []

        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            results: list[int] = []
            for v in [1, 2]:
                r = k(v)
                post_resume_log.append(f"shot {v} returned {r}")
                results.extend(r)
            return results

        def run():
            x = choose()
            return [x * 10]

        result = h(run)
        assert result == [10, 20]
        assert post_resume_log == ["shot 1 returned [10]", "shot 2 returned [20]"]


# ---------------------------------------------------------------------------
# Multi-shot: one-shot backward compatibility
# ---------------------------------------------------------------------------


class TestMultiShotBackwardCompat:
    def test_single_resume_still_works(self):
        """Calling k exactly once (one-shot) continues to work."""
        get_val: Effect[[], str] = effect("get_val")
        h: Handler[str] = create_handler(get_val)

        @h.on(get_val)
        def _get(k: Resume[str, str]):
            return k("hello")

        result = h(lambda: get_val())
        assert result == "hello"

    def test_abort_still_works(self):
        """Not calling k (abort) continues to work."""
        e: Effect[[], int] = effect("e")
        h: Handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 42

        result = h(lambda: e())
        assert result == 42

    def test_multiple_effects_oneshot(self):
        """Multiple one-shot effects still work correctly."""
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        h: Handler[int] = create_handler(read, write)

        @h.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        @h.on(write)
        def _write(k: Resume[int, int], s: str):
            return k(len(s))

        def run():
            s = read()
            return write(s)

        result = h(run)
        assert result == 4

    def test_stack_cleanup_after_multishot(self):
        """Handler stack is cleaned up after multi-shot handler completes."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        h(lambda: [choose()])

        with pytest.raises(EffectNotHandledError):
            choose()


# ---------------------------------------------------------------------------
# Multi-shot: edge cases
# ---------------------------------------------------------------------------


class TestMultiShotEdgeCases:
    def test_resume_with_different_types(self):
        """Each shot can resume with a different value."""
        choose: Effect[[], str] = effect("choose")
        h: Handler[list[str]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[str, list[str]]):
            return k("hello") + k("world")

        def run():
            s = choose()
            return [s.upper()]

        result = h(run)
        assert result == ["HELLO", "WORLD"]

    def test_multishot_effect_invoked_multiple_times(self):
        """A multi-shot effect is invoked multiple times in the same computation."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return [*k(0), *k(1)]

        def run():
            a = choose()
            b = choose()
            return [a + b]

        # Two choose() calls, each forking into 2 branches = 4 total
        result = h(run)
        assert result == [0, 1, 1, 2]

    def test_exception_in_continuation_propagates(self):
        """An exception raised in the continuation propagates to the handler."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[int] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, int]):
            try:
                return k(1)
            except ValueError:
                return -1

        def run():
            x = choose()
            if x == 1:
                raise ValueError("bad value")
            return x

        result = h(run)
        assert result == -1

    def test_large_number_of_shots(self):
        """Many shots don't cause stack overflow or corruption."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            results: list[int] = []
            for i in range(100):
                results.extend(k(i))
            return results

        def run():
            return [choose()]

        result = h(run)
        assert result == list(range(100))


# ---------------------------------------------------------------------------
# Multi-shot: async handler
# ---------------------------------------------------------------------------


class TestMultiShotAsync:
    @pytest.mark.asyncio
    async def test_async_resume_twice(self):
        """Async handler can call k(value) multiple times."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[int] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, int]):
            r1 = await k(1)
            r2 = await k(2)
            return r1 + r2

        def run():
            x = choose()
            return x * 10

        result = await h(run)
        assert result == 30  # 10 + 20

    @pytest.mark.asyncio
    async def test_async_resume_with_await_between_shots(self):
        """Async handler can await between multi-shot resumes."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            results: list[int] = []
            for v in [1, 2, 3]:
                await asyncio.sleep(0.001)
                results.extend(await k(v))
            return results

        def run():
            return [choose() * 10]

        result = await h(run)
        assert result == [10, 20, 30]

    @pytest.mark.asyncio
    async def test_async_abort(self):
        """Async handler abort (no resume) still works with multi-shot."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[int] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, int]):
            return -1  # abort: return without calling k

        result = await h(lambda: choose())
        assert result == -1

    @pytest.mark.asyncio
    async def test_async_mutable_locals_shared(self):
        """Async multi-shot shares mutable locals (Scheme semantics)."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[list[int]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[list[int]]]):
            return await k(1) + await k(2) + await k(3)

        def run():
            items: list[int] = []
            v = choose()
            items.append(v)
            return [items]

        result = await h(run)
        # Shared list accumulates across shots
        assert result == [[1, 2, 3], [1, 2, 3], [1, 2, 3]]

    @pytest.mark.asyncio
    async def test_async_mutable_locals_snapshot_via_copy(self):
        """Async: shallow-copying a local mutable captures per-shot state."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[list[int]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[list[int]]]):
            return await k(1) + await k(2) + await k(3)

        def run():
            items: list[int] = []
            v = choose()
            items.append(v)
            return [list(items)]

        result = await h(run)
        assert result == [[1], [1, 2], [1, 2, 3]]

    @pytest.mark.asyncio
    async def test_async_multishot_with_oneshot(self):
        """Async multi-shot coexists with one-shot effect."""
        choose: Effect[[], int] = effect("choose")
        log: Effect[[str], None] = effect("log")
        logged: list[str] = []

        h: AsyncHandler[list[int]] = create_async_handler(choose, log)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        @h.on(log)
        async def _log(k: ResumeAsync[None, list[int]], msg: str):
            logged.append(msg)
            return await k(None)

        def run():
            x = choose()
            log(f"chose {x}")
            return [x]

        result = await h(run)
        assert result == [1, 2]
        assert logged == ["chose 1", "chose 2"]

    @pytest.mark.asyncio
    async def test_async_exception_in_continuation(self):
        """Exception in async multi-shot continuation propagates to handler."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[int] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, int]):
            try:
                return await k(1)
            except ValueError:
                return -1

        def run():
            x = choose()
            if x == 1:
                raise ValueError("bad value")
            return x

        result = await h(run)
        assert result == -1

    @pytest.mark.asyncio
    async def test_async_stack_cleanup(self):
        """Handler stack is cleaned up after async multi-shot handler completes."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        await h(lambda: [choose()])

        with pytest.raises(EffectNotHandledError):
            choose()


# ---------------------------------------------------------------------------
# Multi-shot: mixed sync/async nesting
# ---------------------------------------------------------------------------


class TestMultiShotMixed:
    @pytest.mark.asyncio
    async def test_sync_multishot_inside_async_handler(self):
        """Sync multi-shot handler nested inside async one-shot handler."""
        choose: Effect[[], int] = effect("choose")
        get_base: Effect[[], int] = effect("get_base")

        h_outer: AsyncHandler[list[int]] = create_async_handler(get_base)

        @h_outer.on(get_base)
        async def _get_base(k: ResumeAsync[int, list[int]]):
            return await k(100)

        def inner():
            h_inner: Handler[list[int]] = create_handler(choose)

            @h_inner.on(choose)
            def _choose(k: Resume[int, list[int]]):
                return k(1) + k(2)

            def body():
                base = get_base()
                x = choose()
                return [base + x]

            return h_inner(body)

        result = await h_outer(inner)
        assert result == [101, 102]

    @pytest.mark.asyncio
    async def test_async_oneshot_inside_sync_multishot(self):
        """Async one-shot handler wraps sync multi-shot handler.

        The sync handler's effect is performed inside the async handler's
        caller.  _drive_async must detect the sync handler and create a
        sync Resume instead of ResumeAsync.
        """
        choose: Effect[[], int] = effect("choose")
        get_base: Effect[[], int] = effect("get_base")

        h_sync: Handler[list[int]] = create_handler(choose)

        @h_sync.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        h_async: AsyncHandler[list[int]] = create_async_handler(get_base)

        @h_async.on(get_base)
        async def _get_base(k: ResumeAsync[int, list[int]]):
            return await k(100)

        def body():
            base = get_base()
            x = choose()
            return [base + x]

        async def outer():
            return h_sync(body)

        result = await h_async(outer)
        assert result == [101, 102]


# ---------------------------------------------------------------------------
# Multi-shot: effect inside loop (value stack preservation)
# ---------------------------------------------------------------------------


class TestMultiShotLoop:
    def test_effect_in_for_loop_immutable(self):
        """Effect called multiple times inside a for loop.

        ``(*choices, choose())`` creates a temporary list that is shared
        across shots (Scheme semantics), so each k(1) restore sees the
        list mutated by the preceding k(0).  The range iterator is also
        shared and exhausted after the one-shot path completes.
        """
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, ...]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, ...]]]):
            return k(0) + k(1)

        def body():
            choices: tuple[int, ...] = ()
            for _ in range(3):
                choices = (*choices, choose())
            return [choices]

        result = h(body)
        assert result == [(0, 0, 0), (0, 0, 0, 1), (0, 0, 1), (0, 1)]

    def test_effect_in_for_loop_new_list(self):
        """Effect in a for loop with ``[*items, choose()]``.

        The temporary list from ``[*items, ...]`` is shared across shots
        (Scheme semantics), and the range iterator is exhausted after
        the one-shot path completes.
        """
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, ...]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, ...]]]):
            return k(10) + k(20)

        def body():
            items: list[int] = []
            for _ in range(2):
                items = [*items, choose()]
            return [tuple(items)]

        result = h(body)
        assert result == [(10, 10), (10, 10, 20), (10, 20)]

    def test_effect_in_while_loop(self):
        """Effect called inside a while loop with immutable accumulation."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, int]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, int]]]):
            return k(0) + k(1)

        def body():
            a = choose()
            i = 0
            total = 0
            while i < 2:
                total += choose()
                i += 1
            return [(a, total)]

        result = h(body)
        # a: 2 choices, loop body: 2 choices x 2 iterations = 4 combos
        # total possible: 2 * 4 = 8
        assert len(result) == 8

    def test_effect_in_nested_expression_in_loop(self):
        """Effect inside ``total = total + choose()`` in a for loop.

        ``total`` is an int (immutable) so each shot gets an independent
        value.  However, the range iterator is shared and exhausted after
        the one-shot path, so k(2) from iter 0 cannot enter iter 1.
        """
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]):
            return k(1) + k(2)

        def body():
            total = 0
            for _ in range(2):
                total = total + choose()
            return [total]

        result = h(body)
        # k(1)→k(1)→2, k(1)→k(2)→3, k(2)→iterator exhausted→2
        assert result == [2, 3, 2]
