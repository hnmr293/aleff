import pytest

from aleff import (
    effect,
    Effect,
    handler,
    create_handler,
    Resume,
    EffectNotHandledError,
)

# ---------------------------------------------------------------------------
# effect() factory
# ---------------------------------------------------------------------------


class TestEffectFactory:
    def test_create_sync_effect(self):
        e: Effect[[], str] = effect("my_effect")
        assert e.name == "my_effect"

    def test_create_sync_effect_is_effect(self):
        e = effect("e")
        assert isinstance(e, Effect)

    def test_annotator_single_effect(self):
        e: Effect[[], str] = effect("e")

        @effect(e)
        def fn() -> str:
            return "hello"

        assert fn() == "hello"

    def test_annotator_multiple_effects(self):
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[int], None] = effect("e2")

        @effect(e1, e2)
        def fn() -> str:
            return "hello"

        assert fn() == "hello"

    def test_no_arguments_raises_type_error(self):
        with pytest.raises(TypeError, match="at least one argument"):
            effect()

    def test_non_effect_arguments_raises_type_error(self):
        with pytest.raises(TypeError, match="all arguments must be callable"):
            effect(42)  # type: ignore


# ---------------------------------------------------------------------------
# Sync handler — normal cases
# ---------------------------------------------------------------------------


class TestSyncHandler:
    def test_single_effect_resume(self):
        get_val: Effect[[], str] = effect("get_val")
        h: handler[str] = create_handler(get_val)

        @h.on(get_val)
        def _get(k: Resume[str, str]):
            return k("hello")

        result = h(lambda: get_val())
        assert result == "hello"

    def test_multiple_effects(self):
        read: Effect[[], str] = effect("read")
        write: Effect[[str], int] = effect("write")
        h: handler[int] = create_handler(read, write)

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

    def test_effect_with_arguments(self):
        add: Effect[[int, int], int] = effect("add")
        h: handler[int] = create_handler(add)

        @h.on(add)
        def _add(k: Resume[int, int], a: int, b: int):
            return k(a + b)

        result = h(lambda: add(3, 7))
        assert result == 10

    def test_handler_result_from_fn(self):
        """handler returns the value from fn when no abort occurs."""
        e: Effect[[], str] = effect("e")
        h: handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("val")

        result = h(lambda: e() + "_suffix")
        assert result == "val_suffix"

    def test_abort_without_resume(self):
        e: Effect[[], int] = effect("e")
        h: handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 42  # no resume → abort

        result = h(lambda: e(), check=False)
        assert result == 42

    def test_nested_handlers(self):
        inner_eff: Effect[[], str] = effect("inner")
        outer_eff: Effect[[], int] = effect("outer")

        def run_inner():
            h: handler[str] = create_handler(inner_eff)

            @h.on(inner_eff)
            def _handle(k: Resume[str, str]):
                return k("from_inner")

            return h(lambda: inner_eff())

        def run_outer():
            h: handler[int] = create_handler(outer_eff)

            @h.on(outer_eff)
            def _handle(k: Resume[int, int]):
                return k(99)

            def body():
                run_inner()
                return outer_eff()

            return h(body)

        assert run_outer() == 99

    def test_same_effect_nested_handlers(self):
        """Inner handler shadows outer handler for the same effect."""
        e: Effect[[], str] = effect("e")

        def run():
            h_inner: handler[str] = create_handler(e)

            @h_inner.on(e)
            def _inner(k: Resume[str, str]):
                return k("inner")

            return h_inner(lambda: e())

        h_outer: handler[str] = create_handler(e)

        @h_outer.on(e)
        def _outer(k: Resume[str, str]):
            return k("outer")

        result = h_outer(run)
        assert result == "inner"

    def test_multiple_effect_invocations(self):
        """Same effect can be invoked multiple times."""
        counter: Effect[[], int] = effect("counter")
        call_count = 0

        h: handler[int] = create_handler(counter)

        @h.on(counter)
        def _counter(k: Resume[int, int]):
            nonlocal call_count
            call_count += 1
            return k(call_count)

        def run():
            a = counter()
            b = counter()
            c = counter()
            return a + b + c

        result = h(run)
        assert result == 6  # 1 + 2 + 3
        assert call_count == 3


# ---------------------------------------------------------------------------
# Sync handler — error cases
# ---------------------------------------------------------------------------


class TestSyncHandlerErrors:
    def test_effect_not_handled_raises(self):
        e: Effect[[], str] = effect("unhandled")
        with pytest.raises(EffectNotHandledError):
            e()

    def test_effect_not_declared_raises(self):
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        h: handler[str] = create_handler(e1)

        with pytest.raises(ValueError, match="not declared"):

            @h.on(e2)
            def _handle(k: Resume[str, str]):
                return k("x")

    def test_duplicate_effect_handler_raises(self):
        e: Effect[[], str] = effect("e")
        h: handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("x")

        with pytest.raises(ValueError, match="already handled"):

            @h.on(e)
            def _handle2(k: Resume[str, str]):
                return k("y")

    def test_unbound_effects_raises_on_call(self):
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        h: handler[str] = create_handler(e1, e2)

        @h.on(e1)
        def _handle(k: Resume[str, str]):
            return k("x")

        with pytest.raises(ValueError, match="not all effects are handled"):
            h(lambda: e1())

    def test_check_false_skips_unbound_validation(self):
        e1: Effect[[], str] = effect("e1")
        e2: Effect[[], str] = effect("e2")
        h: handler[str] = create_handler(e1, e2)

        @h.on(e1)
        def _handle(k: Resume[str, str]):
            return k("ok")

        result = h(lambda: e1(), check=False)
        assert result == "ok"

    def test_fn_exception_propagates(self):
        """Exceptions from fn propagate through handler."""
        e: Effect[[], str] = effect("e")
        h: handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("val")

        def fn():
            e()
            raise ValueError("user error")

        with pytest.raises(ValueError, match="user error"):
            h(fn)

    def test_handler_exception_propagates(self):
        """Exceptions raised in handler function propagate."""
        e: Effect[[], str] = effect("e")
        h: handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            raise ValueError("handler error")

        with pytest.raises(ValueError, match="handler error"):
            h(lambda: e())


# ---------------------------------------------------------------------------
# Sync stack cleanup
# ---------------------------------------------------------------------------


class TestSyncStackCleanup:
    def test_stack_cleaned_after_handler(self):
        e: Effect[[], str] = effect("e")
        h: handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("val")

        h(lambda: e())

        with pytest.raises(EffectNotHandledError):
            e()

    def test_stack_cleaned_after_exception(self):
        """Stack should be cleaned even when fn raises an exception."""
        e: Effect[[], str] = effect("e")
        h: handler[str] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[str, str]):
            return k("val")

        with pytest.raises(RuntimeError, match="boom"):
            h(lambda: (_ for _ in ()).throw(RuntimeError("boom")))

        with pytest.raises(EffectNotHandledError):
            e()

    def test_stack_cleaned_after_abort(self):
        """Stack is cleaned when handler aborts (no resume)."""
        e: Effect[[], int] = effect("e")
        h: handler[int] = create_handler(e)

        @h.on(e)
        def _handle(k: Resume[int, int]):
            return 0  # abort

        h(lambda: e(), check=False)

        with pytest.raises(EffectNotHandledError):
            e()
