"""Tests for multi-shot continuations.

Multi-shot means the handler can call k(value) multiple times,
each time resuming from the same suspension point with independent state.
"""

import asyncio
from collections.abc import Generator
import dis
import subprocess
import sys
import textwrap
import types
from typing import Any

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
    EffectNotHandledError,
)


# ---------------------------------------------------------------------------
# Multi-shot: basic resume multiple times
# ---------------------------------------------------------------------------


class TestMultiShotBasic:
    def test_sync_caller_preserves_handled_exception_per_shot(self):
        """Restored synchronous callers retain their handled exception."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, str]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, str]]]):
            return k(1) + k(2)

        def run() -> list[tuple[int, str]]:
            try:
                raise ValueError("active")
            except ValueError:
                value = choose()
                exc = sys.exception()
                assert exc is not None
                return [(value, str(exc))]

        assert h(run) == [(1, "active"), (2, "active")]

    def test_nested_sync_frames_preserve_distinct_handled_exceptions(self):
        """Each restored synchronous frame retains its own handled exception."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[tuple[int, str, str]]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[tuple[int, str, str]]]):
            return k(1) + k(2)

        def inner() -> tuple[int, str]:
            try:
                raise KeyError("inner")
            except KeyError:
                value = choose()
                exc = sys.exception()
                assert exc is not None
                return value, type(exc).__name__

        def run() -> list[tuple[int, str, str]]:
            try:
                raise ValueError("outer")
            except ValueError:
                value, inner_exception = inner()
                exc = sys.exception()
                assert exc is not None
                return [(value, inner_exception, type(exc).__name__)]

        assert h(run) == [(1, "KeyError", "ValueError"), (2, "KeyError", "ValueError")]

    def test_sync_generator_frame_is_rejected_without_crashing(self):
        code = textwrap.dedent(
            """
            from aleff import Handler, Resume, create_handler, effect

            choose = effect("choose")
            handler = create_handler(choose)

            @handler.on(choose)
            def handle_choose(k):
                return k(1) + k(2)

            def values():
                value = choose()
                yield value
                yield value + 10

            def run():
                iterator = values()
                return [next(iterator), next(iterator)]

            try:
                handler(run)
            except RuntimeError as exc:
                assert str(exc) == "synchronous generator frames are not supported by multi-shot restoration"
            else:
                raise AssertionError("expected an explicit unsupported-frame error")
            """
        )

        result = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)

        assert result.returncode == 0, result.stderr

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

        def inner() -> list[int]:
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
# Multi-shot: call shapes in the captured chain
# ---------------------------------------------------------------------------


class TestMultiShotCallShapes:
    """Multi-shot across calls that do not compile to a plain CALL.

    A frame suspended mid-call is resumed just past the call opcode, and not
    every call spells that opcode the same way: ``f(*args)`` and ``f(**kwargs)``
    compile to CALL_FUNCTION_EX, and on 3.13+ a keyword call compiles to
    CALL_KW.
    """

    def test_multishot_through_star_args_call(self):
        """The continuation crosses a f(*args) call."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]) -> list[int]:
            return [*k(1), *k(2)]

        def f(a: int) -> list[int]:
            return [choose() + a]

        args = (10,)
        assert h(lambda: f(*args)) == [11, 12]

    def test_multishot_through_star_args_call_three_shots(self):
        """The same frame is re-entered once per shot."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]) -> list[int]:
            return [*k(1), *k(2), *k(3)]

        def f(a: int) -> list[int]:
            return [choose() + a]

        args = (10,)
        assert h(lambda: f(*args)) == [11, 12, 13]

    def test_multishot_through_double_star_kwargs_call(self):
        """The continuation crosses a f(**kwargs) call."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]) -> list[int]:
            return [*k(1), *k(2)]

        def f(a: int) -> list[int]:
            return [choose() + a]

        kwargs = {"a": 10}
        assert h(lambda: f(**kwargs)) == [11, 12]

    def test_multishot_through_keyword_call(self):
        """The continuation crosses a keyword call (CALL_KW on 3.13+)."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]) -> list[int]:
            return [*k(1), *k(2)]

        def f(a: int) -> list[int]:
            return [choose() + a]

        assert h(lambda: f(a=10)) == [11, 12]

    def test_multishot_through_mixed_star_and_keyword_call(self):
        """The continuation crosses a f(*args, b=...) call."""
        choose: Effect[[], int] = effect("choose")
        h: Handler[list[int]] = create_handler(choose)

        @h.on(choose)
        def _choose(k: Resume[int, list[int]]) -> list[int]:
            return [*k(1), *k(2)]

        def g(a: int, b: int) -> list[int]:
            return [choose() + a + b]

        args = (10,)
        assert h(lambda: g(*args, b=100)) == [111, 112]

    def test_multishot_through_nested_handlers(self):
        """The inner continuation contains the library's own *args dispatch.

        `_drive` calls the handler fn as `d.fn(resume, *d.args, **d.kwargs)`, so
        a nested handler puts a CALL_FUNCTION_EX frame in the captured chain
        without any starred call in user code.
        """
        inner_e: Effect[[], int] = effect("inner")
        outer_e: Effect[[int], int] = effect("outer")

        h_outer: Handler[Any] = create_handler(outer_e)
        h_inner: Handler[Any] = create_handler(inner_e)

        @h_outer.on(outer_e)
        def _handle_outer(k: Resume[int, Any], v: int) -> list[Any]:
            return [k(v), k(v + 100)]

        @h_inner.on(inner_e)
        def _handle_inner(k: Resume[int, Any]) -> Any:
            return k(outer_e(7))

        assert h_outer(lambda: h_inner(lambda: inner_e())) == [7, 107]

    @pytest.mark.asyncio
    async def test_async_multishot_through_star_args_call(self):
        """The async re-entry path resumes the same call shapes."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]) -> list[int]:
            return [*await k(1), *await k(2)]

        def f(a: int) -> list[int]:
            return [choose() + a]

        args = (10,)
        assert await h(lambda: f(*args)) == [11, 12]


# ---------------------------------------------------------------------------
# Multi-shot: post-resume code (non-stack-cutting)
# ---------------------------------------------------------------------------


class TestMultiShotPostResume:
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
    async def test_async_caller_preserves_handled_exception_per_shot(self):
        """Each restored shot retains the caller's handled exception."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[tuple[int, str]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[tuple[int, str]]]):
            return await k(1) + await k(2) + await k(3)

        async def run() -> list[tuple[int, str]]:
            try:
                raise ValueError("active")
            except ValueError:
                value = choose()
                exc = sys.exception()
                assert exc is not None
                return [(value, str(exc))]

        assert await h(run) == [(1, "active"), (2, "active"), (3, "active")]

    @pytest.mark.asyncio
    async def test_async_caller_bare_raise_uses_handled_exception_per_shot(self):
        """A bare raise after resume re-raises the caller's active exception."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[str]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[str]]):
            messages: list[str] = []
            for value in (1, 2):
                try:
                    await k(value)
                except ValueError as exc:
                    messages.append(str(exc))
            return messages

        async def run() -> list[str]:
            try:
                raise ValueError("active")
            except ValueError:
                choose()
                raise

        assert await h(run) == ["active", "active"]

    @pytest.mark.asyncio
    async def test_handler_exception_context_does_not_leak_into_caller(self):
        """A handler's active exception is not inherited by restored callers."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[BaseException | None]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[BaseException | None]]):
            try:
                raise ValueError("handler")
            except ValueError:
                return await k(1) + await k(2)

        async def run() -> list[BaseException | None]:
            choose()
            return [sys.exception()]

        assert await h(run) == [None, None]

    @pytest.mark.asyncio
    async def test_nested_coroutines_preserve_distinct_handled_exceptions(self):
        """Each restored coroutine frame retains its own handled exception."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[tuple[int, str, str]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[tuple[int, str, str]]]):
            return await k(1) + await k(2)

        async def inner() -> tuple[int, str]:
            try:
                raise KeyError("inner")
            except KeyError:
                value = choose()
                exc = sys.exception()
                assert exc is not None
                return value, type(exc).__name__

        async def run() -> list[tuple[int, str, str]]:
            try:
                raise ValueError("outer")
            except ValueError:
                value, inner_exception = await inner()
                exc = sys.exception()
                assert exc is not None
                return [(value, inner_exception, type(exc).__name__)]

        assert await h(run) == [(1, "KeyError", "ValueError"), (2, "KeyError", "ValueError")]

    @pytest.mark.asyncio
    async def test_inner_handled_exception_does_not_leak_to_outer_coroutine(self):
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[tuple[int, BaseException | None]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[tuple[int, BaseException | None]]]):
            return await k(1) + await k(2)

        async def inner() -> int:
            try:
                raise KeyError("inner")
            except KeyError:
                return choose()

        async def run() -> list[tuple[int, BaseException | None]]:
            value = await inner()
            return [(value, sys.exception())]

        assert await h(run) == [(1, None), (2, None)]

    @pytest.mark.asyncio
    async def test_nested_coroutine_inherits_outer_handled_exception(self):
        """A restored inner coroutine sees an exception handled by its awaiter."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[tuple[int, str]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[tuple[int, str]]]):
            return await k(1) + await k(2)

        async def inner() -> tuple[int, str]:
            value = choose()
            exc = sys.exception()
            assert exc is not None
            return value, type(exc).__name__

        async def run() -> list[tuple[int, str]]:
            try:
                raise ValueError("outer")
            except ValueError:
                return [await inner()]

        assert await h(run) == [(1, "ValueError"), (2, "ValueError")]

    @pytest.mark.asyncio
    async def test_async_caller_with_sync_helper_preserves_distinct_handled_exceptions(self):
        """A sync helper and its async caller restore their own exception states."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[tuple[int, str, str]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[tuple[int, str, str]]]):
            return await k(1) + await k(2)

        def inner() -> tuple[int, str]:
            try:
                raise KeyError("inner")
            except KeyError:
                value = choose()
                exc = sys.exception()
                assert exc is not None
                return value, type(exc).__name__

        async def run() -> list[tuple[int, str, str]]:
            try:
                raise ValueError("outer")
            except ValueError:
                value, inner_exception = inner()
                exc = sys.exception()
                assert exc is not None
                return [(value, inner_exception, type(exc).__name__)]

        assert await h(run) == [(1, "KeyError", "ValueError"), (2, "KeyError", "ValueError")]

    @pytest.mark.asyncio
    async def test_generator_based_coroutine_returns_after_effect_per_shot(self):
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        @types.coroutine
        def inner() -> Generator[Any, Any, int]:
            value = choose()
            if False:
                yield None
            return value

        async def run() -> list[int]:
            return [await inner()]

        assert await h(run) == [1, 2]

    @pytest.mark.asyncio
    async def test_generator_based_coroutine_bare_yield_after_effect_per_shot(self):
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        @types.coroutine
        def inner() -> Generator[Any, Any, int]:
            value = choose()
            yield None
            return value

        async def run() -> list[int]:
            return [await inner()]

        assert await h(run) == [1, 2]

    @pytest.mark.asyncio
    async def test_generator_based_coroutine_await_and_error_after_effect_per_shot(self):
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int | str]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int | str]]):
            return await k(1) + await k(2)

        @types.coroutine
        def inner() -> Generator[Any, Any, int]:
            value = choose()
            yield from asyncio.sleep(0).__await__()
            if value == 2:
                raise ValueError("bad shot: 2")
            return value

        async def run() -> list[int | str]:
            try:
                return [await inner()]
            except ValueError as exc:
                return [str(exc)]

        assert await h(run) == [1, "bad shot: 2"]

    def test_async_generator_frame_is_restored_with_async_generator_ownership(self):
        code = textwrap.dedent(
            """
            import asyncio
            from aleff import create_async_handler, effect

            choose = effect("choose")
            handler = create_async_handler(choose)

            @handler.on(choose)
            async def handle_choose(k):
                return await k(1) + await k(2)

            async def values():
                value = choose()
                yield value
                yield value + 10

            async def run():
                iterator = values()
                return [await anext(iterator), await anext(iterator)]

            async def main():
                assert await handler(run) == [1, 11, 2, 12]

            asyncio.run(main())
            """
        )

        result = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)

        assert result.returncode == 0, result.stderr

    @pytest.mark.asyncio
    async def test_async_generator_effect_is_restored_through_async_for(self):
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        async def values():
            yield choose()

        async def run() -> list[int]:
            result: tuple[int, ...] = ()
            async for value in values():
                result += (value,)
            return list(result)

        assert await h(run) == [1, 2]

    @pytest.mark.asyncio
    async def test_async_generator_effect_with_extended_jump_arguments(self):
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        async def values():
            yield choose()

        padding = "\n".join(["        total += 0"] * 14_000)
        source = f"""
async def run():
    total = 0
    if padding_enabled:
{padding}
    while total < 1:
{padding}
        total += 1
    try:
{padding}
        raise ValueError("enter exception handler")
    except ValueError:
        iterator = values()
        return [await anext(iterator)]
"""
        namespace: dict[str, Any] = {
            "anext": anext,
            "padding_enabled": False,
            "values": values,
        }
        exec(source, namespace)
        run = namespace["run"]
        instructions = list(dis.get_instructions(run))
        maximum_extended_arg_run = 0
        extended_arg_run = 0
        for instruction in instructions:
            if instruction.opname == "EXTENDED_ARG":
                extended_arg_run += 1
                maximum_extended_arg_run = max(maximum_extended_arg_run, extended_arg_run)
            else:
                extended_arg_run = 0

        assert maximum_extended_arg_run >= 2
        assert any(
            instruction.opcode in dis.hasjrel
            and isinstance(instruction.argval, int)
            and instruction.argval > instruction.offset
            and instruction.arg is not None
            and instruction.arg > 65_535
            for instruction in instructions
        )
        assert any(
            instruction.opname.startswith("JUMP_BACKWARD") and instruction.arg is not None and instruction.arg > 65_535
            for instruction in instructions
        )
        assert await h(run) == [1, 2]

    def test_async_generator_asend_and_athrow_are_restored_per_shot(self):
        code = textwrap.dedent(
            """
            import asyncio
            from aleff import create_async_handler, effect

            choose = effect("choose")
            handler = create_async_handler(choose)

            @handler.on(choose)
            async def handle_choose(k):
                return await k(1) + await k(2)

            async def asend_values():
                sent = yield 0
                value = choose()
                yield sent + value

            async def run_asend():
                iterator = asend_values()
                first = await anext(iterator)
                second = await iterator.asend(10)
                await iterator.aclose()
                return [(first, second)]

            async def athrow_values():
                try:
                    yield 0
                except ValueError:
                    yield choose()

            async def run_athrow():
                iterator = athrow_values()
                first = await anext(iterator)
                second = await iterator.athrow(ValueError("injected"))
                await iterator.aclose()
                return [(first, second)]

            async def main():
                assert await handler(run_asend) == [(0, 11), (0, 12)]
                assert await handler(run_athrow) == [(0, 1), (0, 2)]

            asyncio.run(main())
            """
        )

        result = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)

        assert result.returncode == 0, result.stderr

    @pytest.mark.asyncio
    async def test_async_caller_preserves_implicit_exception_chaining(self):
        """New errors raised after resume retain the caller's handled exception as context."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[tuple[str, str]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[tuple[str, str]]]):
            results: list[tuple[str, str]] = []
            for value in (1, 2):
                try:
                    await k(value)
                except RuntimeError as exc:
                    assert exc.__context__ is not None
                    results.append((str(exc), type(exc.__context__).__name__))
            return results

        async def run() -> list[tuple[str, str]]:
            try:
                raise ValueError("active")
            except ValueError:
                value = choose()
                raise RuntimeError(f"shot: {value}")

        assert await h(run) == [("shot: 1", "ValueError"), ("shot: 2", "ValueError")]

    @pytest.mark.asyncio
    @pytest.mark.parametrize(
        ("shots", "expected"),
        [(0, []), (1, [0]), (2, [0, 1]), (3, [0, 1, 2])],
    )
    async def test_async_caller_resume_many_times(self, shots: int, expected: list[int]):
        """An async caller can be resumed zero, one, or many times."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            results: list[int] = []
            for value in range(shots):
                results.extend(await k(value))
            return results

        async def run() -> list[int]:
            return [choose()]

        assert await h(run) == expected

    @pytest.mark.asyncio
    @pytest.mark.parametrize("delay", [0, 0.001], ids=["bare-yield", "future"])
    async def test_async_caller_awaits_after_effect_per_shot(self, delay: float):
        """Each restored async caller independently completes a later await."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)
        completed: list[int] = []

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        async def run():
            value = choose()
            await asyncio.sleep(delay)
            completed.append(value)
            return [value]

        assert await h(run) == [1, 2]
        assert completed == [1, 2]

    @pytest.mark.asyncio
    async def test_async_caller_exception_after_effect_isolated_per_shot(self):
        """An exception from one restored caller does not corrupt another shot."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int | str]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int | str]]):
            results: list[int | str] = []
            for value in (1, 2, 3):
                try:
                    results.extend(await k(value))
                except ValueError as exc:
                    results.append(str(exc))
            return results

        async def run() -> list[int | str]:
            value = choose()
            await asyncio.sleep(0)
            if value == 2:
                raise ValueError("bad shot: 2")
            return [value]

        assert await h(run) == [1, "bad shot: 2", 3]

    @pytest.mark.asyncio
    async def test_async_caller_restores_nested_coroutine_frames(self):
        """Nested coroutine frames are restored from inner to outer per shot."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        async def inner():
            value = choose()
            await asyncio.sleep(0)
            return value

        async def run():
            return [10 + await inner()]

        assert await h(run) == [11, 12]

    @pytest.mark.asyncio
    async def test_async_caller_restores_sync_helper_before_coroutine_frame(self):
        """Synchronous frames below an async caller are restored first."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        def inner():
            return choose()

        async def run():
            value = inner()
            await asyncio.sleep(0)
            return [value]

        assert await h(run) == [1, 2]

    @pytest.mark.asyncio
    async def test_async_caller_throws_nested_coroutine_error_into_outer_frame(self):
        """An inner restored coroutine error reaches the outer async frame."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int | str]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int | str]]):
            return await k(1) + await k(2)

        async def inner():
            value = choose()
            await asyncio.sleep(0)
            if value == 2:
                raise ValueError("nested: 2")
            return value

        async def run() -> list[int | str]:
            try:
                return [await inner()]
            except ValueError as exc:
                return [str(exc)]

        assert await h(run) == [1, "nested: 2"]

    @pytest.mark.asyncio
    async def test_async_caller_restores_three_nested_coroutine_frames(self):
        """Coroutine completion is relayed through every restored await frame."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        async def leaf():
            value = choose()
            await asyncio.sleep(0)
            return value

        async def middle():
            return 10 + await leaf()

        async def run():
            return [100 + await middle()]

        assert await h(run) == [111, 112]

    @pytest.mark.asyncio
    async def test_async_caller_composes_two_multishot_effects(self):
        """Two effects in an async caller produce their cartesian product."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[tuple[int, int]]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[tuple[int, int]]]):
            return await k(0) + await k(1)

        async def run():
            first = choose()
            await asyncio.sleep(0)
            second = choose()
            await asyncio.sleep(0)
            return [(first, second)]

        assert await h(run) == [(0, 0), (0, 1), (1, 0), (1, 1)]

    @pytest.mark.asyncio
    async def test_async_caller_catches_awaitable_error_per_shot(self):
        """Each restored caller receives errors from its own awaitable."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[str]] = create_async_handler(choose)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[str]]):
            return await k(1) + await k(2)

        async def run() -> list[str]:
            value = choose()
            future: asyncio.Future[None] = asyncio.get_running_loop().create_future()
            asyncio.get_running_loop().call_soon(future.set_exception, ValueError(f"future: {value}"))
            try:
                await future
            except ValueError as exc:
                return [str(exc)]
            pytest.fail("the Future error was not thrown into the restored caller")

        assert await h(run) == ["future: 1", "future: 2"]

    @pytest.mark.asyncio
    async def test_async_caller_resume_many_times_with_shallow_handler(self):
        """A shallow async handler can resume one captured occurrence repeatedly."""
        choose: Effect[[], int] = effect("choose")
        h: AsyncHandler[list[int]] = create_async_handler(choose, shallow=True)

        @h.on(choose)
        async def _choose(k: ResumeAsync[int, list[int]]):
            return await k(1) + await k(2)

        async def run() -> list[int]:
            return [choose()]

        assert await h(run) == [1, 2]

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
