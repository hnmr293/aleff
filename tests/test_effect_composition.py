"""Tests for effect composition via @effect decorator."""

import pytest

from aleff1 import (
    effect,
    Effect,
    Resume,
    create_handler,
    effects,
    unhandled_effects,
)


# ---------------------------------------------------------------------------
# Composing effects from decorated functions
# ---------------------------------------------------------------------------


class TestEffectComposition:
    def test_compose_from_decorated_functions(self):
        """@effect can accept decorated functions and collect their effects transitively."""
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")

        @effect(read)
        def step1():
            return read()

        @effect(write)
        def step2(s: str):
            return write(s)

        @effect(step1, step2)
        def pipeline():
            s = step1()
            return step2(s)

        assert effects(pipeline) == frozenset({read, write})

    def test_compose_mixed_effects_and_functions(self):
        """@effect can mix Effect instances and decorated functions."""
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        log: Effect[[str], None] = effect("log")

        @effect(read)
        def step1():
            return read()

        @effect(step1, write, log)
        def pipeline():
            s = step1()
            log(s)
            return write(s)

        assert effects(pipeline) == frozenset({read, write, log})

    def test_compose_nested(self):
        """Composition works across multiple levels of nesting."""
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        e3: Effect[[], str] = effect("e3")

        @effect(e1)
        def a():
            return e1()

        @effect(e2, a)
        def b():
            a()
            return e2()

        @effect(e3, b)
        def c():
            b()
            return e3()

        assert effects(c) == frozenset({e1, e2, e3})

    def test_compose_duplicate_effects(self):
        """Duplicate effects from multiple functions are deduplicated."""
        read: Effect[[], str] = effect("read")

        @effect(read)
        def step1():
            return read()

        @effect(read)
        def step2():
            return read()

        @effect(step1, step2)
        def pipeline():
            step1()
            return step2()

        assert effects(pipeline) == frozenset({read})

    def test_compose_preserves_function_behavior(self):
        """Composed @effect should not change the function's runtime behavior."""
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")

        @effect(read)
        def step1():
            return read()

        @effect(step1, write)
        def pipeline():
            s = step1()
            return write(s)

        h = create_handler(read, write)

        @h.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        @h.on(write)
        def _write(k: Resume[int, int], s: str):
            return k(len(s))

        result = h(pipeline)
        assert result == 4

    def test_compose_preserves_function_name(self):
        read: Effect[[], str] = effect("read")

        @effect(read)
        def step1():
            return read()

        @effect(step1)
        def my_pipeline():
            return step1()

        assert my_pipeline.__name__ == "my_pipeline"


# ---------------------------------------------------------------------------
# Error cases
# ---------------------------------------------------------------------------


class TestEffectCompositionEdgeCases:
    def test_undecorated_callable_contributes_no_effects(self):
        """Passing a callable without __effects__ contributes an empty set."""
        read: Effect[[], str] = effect("read")

        def plain_fn():
            return 42

        @effect(plain_fn, read)
        def pipeline():
            plain_fn()
            return read()

        assert effects(pipeline) == frozenset({read})

    def test_non_callable_non_effect_raises(self):
        """Passing a non-callable, non-Effect value raises TypeError."""
        with pytest.raises(TypeError):
            effect(42)  # type: ignore

    def test_no_arguments_raises(self):
        """effect() with no arguments raises TypeError."""
        with pytest.raises(TypeError):
            effect()


# ---------------------------------------------------------------------------
# Integration with unhandled_effects
# ---------------------------------------------------------------------------


class TestEffectCompositionWithUnhandled:
    def test_unhandled_with_composed_effects(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        log: Effect[[str], None] = effect("log")

        @effect(read)
        def step1():
            return read()

        @effect(step1, write, log)
        def pipeline():
            s = step1()
            log(s)
            return write(s)

        h = create_handler(read, write)

        @h.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        @h.on(write)
        def _write(k: Resume[int, int], s: str):
            return k(len(s))

        assert unhandled_effects(pipeline, h) == frozenset({log})

    def test_all_handled_with_composed_effects(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")

        @effect(read)
        def step1():
            return read()

        @effect(step1, write)
        def pipeline():
            s = step1()
            return write(s)

        h = create_handler(read, write)

        @h.on(read)
        def _read(k: Resume[str, int]):
            return k("data")

        @h.on(write)
        def _write(k: Resume[int, int], s: str):
            return k(len(s))

        assert unhandled_effects(pipeline, h) == frozenset()
