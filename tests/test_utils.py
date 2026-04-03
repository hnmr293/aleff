"""Tests for v8 effect metadata, get_effects, and unhandled_effects."""

import pytest

from aleff1 import (
    effect,
    Effect,
    Resume,
    create_handler,
    create_async_handler,
    effects,
    unhandled_effects,
)


# ---------------------------------------------------------------------------
# get_effects: metadata retrieval
# ---------------------------------------------------------------------------


class TestGetEffects:
    def test_single_effect(self):
        e: Effect[[], str] = effect("e")

        @effect(e)
        def fn():
            return e()

        assert effects(fn) == frozenset({e})

    def test_multiple_effects(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")

        @effect(read, write)
        def fn():
            s = read()
            return write(s)

        assert effects(fn) == frozenset({read, write})

    def test_no_decorator_returns_empty(self):
        def fn():
            return 42

        assert effects(fn) == frozenset()

    def test_lambda_returns_empty(self):
        assert effects(lambda: 42) == frozenset()

    def test_decorator_preserves_function_behavior(self):
        """@effect(...) should not change the function's runtime behavior."""
        e: Effect[[], str] = effect("e")

        @effect(e)
        def fn():
            return e()

        h = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("hello")

        result = h(fn)
        assert result == "hello"

    def test_decorator_preserves_function_name(self):
        e: Effect[[], str] = effect("e")

        @effect(e)
        def my_function():
            return e()

        assert my_function.__name__ == "my_function"


# ---------------------------------------------------------------------------
# unhandled_effects: single handler
# ---------------------------------------------------------------------------


class TestUnhandledEffectsSingleHandler:
    def test_all_handled(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")

        @effect(read, write)
        def fn():
            s = read()
            return write(s)

        h = create_handler(read, write)

        @h.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        @h.on(write)
        def _write(k: Resume[int, int], s: str):
            return k(len(s))

        assert unhandled_effects(fn, h) == frozenset()

    def test_some_unhandled(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        log: Effect[[str], None] = effect("log")

        @effect(read, write, log)
        def fn():
            s = read()
            log(s)
            return write(s)

        h = create_handler(read, write)

        @h.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        @h.on(write)
        def _write(k: Resume[int, int], s: str):
            return k(len(s))

        assert unhandled_effects(fn, h) == frozenset({log})

    def test_all_unhandled(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")

        @effect(read, write)
        def fn():
            s = read()
            return write(s)

        log: Effect[[str], None] = effect("log")
        h = create_handler(log)

        @h.on(log)
        def _log(k: Resume[None, None], msg: str):
            return k(None)

        assert unhandled_effects(fn, h) == frozenset({read, write})

    def test_no_decorator_returns_empty(self):
        """A function without @effect has no declared effects, so nothing is unhandled."""

        def fn():
            return 42

        read: Effect[[], str] = effect("read")
        h = create_handler(read)

        @h.on(read)
        def _read(k: Resume[str, str]):
            return k("data")

        assert unhandled_effects(fn, h) == frozenset()

    def test_no_handlers(self):
        """With no handlers provided, all declared effects are unhandled."""
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")

        @effect(read, write)
        def fn():
            s = read()
            return write(s)

        assert unhandled_effects(fn) == frozenset({read, write})


# ---------------------------------------------------------------------------
# unhandled_effects: multiple handlers (nested handler scenario)
# ---------------------------------------------------------------------------


class TestUnhandledEffectsMultipleHandlers:
    def test_two_handlers_cover_all(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        log: Effect[[str], None] = effect("log")

        @effect(read, write, log)
        def fn():
            s = read()
            log(s)
            return write(s)

        h_inner = create_handler(read, write)

        @h_inner.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        @h_inner.on(write)
        def _write(k: Resume[int, int], s: str):
            return k(len(s))

        h_outer = create_handler(log)

        @h_outer.on(log)
        def _log(k: Resume[None, int], msg: str):
            return k(None)

        assert unhandled_effects(fn, h_inner, h_outer) == frozenset()

    def test_two_handlers_partial_coverage(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        log: Effect[[str], None] = effect("log")
        audit: Effect[[str], None] = effect("audit")

        @effect(read, write, log, audit)
        def fn(): ...

        h1 = create_handler(read)

        @h1.on(read)
        def _read(k: Resume[str, None]):
            return k("data")

        h2 = create_handler(write)

        @h2.on(write)
        def _write(k: Resume[int, None], s: str):
            return k(len(s))

        assert unhandled_effects(fn, h1, h2) == frozenset({log, audit})

    def test_overlapping_handlers(self):
        """When multiple handlers declare the same effect, it's still covered."""
        read: Effect[[], str] = effect("read")

        @effect(read)
        def fn():
            return read()

        h1 = create_handler(read)

        @h1.on(read)
        def _read1(k: Resume[str, str]):
            return k("from h1")

        h2 = create_handler(read)

        @h2.on(read)
        def _read2(k: Resume[str, str]):
            return k("from h2")

        assert unhandled_effects(fn, h1, h2) == frozenset()


# ---------------------------------------------------------------------------
# unhandled_effects: with async_handler
# ---------------------------------------------------------------------------


class TestUnhandledEffectsAsync:
    def test_async_handler(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")

        @effect(read, write)
        def fn():
            s = read()
            return write(s)

        h = create_async_handler(read)

        # Only read is handled
        assert unhandled_effects(fn, h) == frozenset({write})

    def test_mixed_sync_async_handlers(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        log: Effect[[str], None] = effect("log")

        @effect(read, write, log)
        def fn(): ...

        h_sync = create_handler(read, write)

        @h_sync.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        @h_sync.on(write)
        def _write(k: Resume[int, int], s: str):
            return k(len(s))

        h_async = create_async_handler(log)

        assert unhandled_effects(fn, h_sync, h_async) == frozenset()
