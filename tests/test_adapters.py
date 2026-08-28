"""Tests for explicit, state-preserving standard-library adapters."""

import itertools

import pytest

from aleff import (
    AdapterRegistry,
    Effect,
    Handler,
    Resume,
    X,
    adapt,
    adapter_registry,
    create_handler,
    effect,
    wind_count,
    wind_range,
    wind_repeat,
)


class TestAdapterRegistry:
    def test_lookup_uses_exact_identity_instead_of_equality(self):
        class Equal:
            def __hash__(self) -> int:
                return 1

            def __eq__(self, other: object) -> bool:
                return isinstance(other, Equal)

        first = Equal()
        second = Equal()

        def first_adapter(*args: object) -> None:
            pass

        def second_adapter(*args: object) -> None:
            pass

        registry = AdapterRegistry()
        registry.register(first, first_adapter)

        assert registry.get(first) is first_adapter
        assert registry.get(second) is None
        assert first in registry
        assert second not in registry

        registry.register(second, second_adapter)
        assert registry.get(first) is first_adapter
        assert registry.get(second) is second_adapter

    def test_register_replaces_only_the_same_identity(self):
        operation = object()
        other_operation = object()

        def old(*args: object) -> None:
            pass

        def new(*args: object) -> None:
            pass

        def other(*args: object) -> None:
            pass

        registry = AdapterRegistry()
        registry.register(operation, old)
        registry.register(operation, new)
        registry.register(other_operation, other)

        assert registry[operation] is new
        assert registry[other_operation] is other

    def test_decorator_registration_returns_the_adapter(self):
        def operation(value: int) -> int:
            return value

        registry = AdapterRegistry()

        @registry.register(operation)
        def adapter(value: int) -> int:
            return value + 1

        assert registry.get(operation) is adapter
        assert registry.adapt(operation, 4) == 5

    def test_unregister_rejects_an_unregistered_identity(self):
        operation = object()
        registry = AdapterRegistry()

        with pytest.raises(KeyError):
            registry.unregister(operation)

    def test_adapt_falls_back_to_the_original_callable(self):
        registry = AdapterRegistry()

        def operation(value: int) -> int:
            return value * 2

        assert registry.adapt(operation, 3) == 6


class TestStandardLibraryAdapters:
    def test_default_registry_contains_only_explicit_adapters(self):
        assert adapter_registry.get(range) is wind_range
        assert adapter_registry.get(itertools.count) is wind_count
        assert adapter_registry.get(itertools.repeat) is wind_repeat
        assert adapter_registry.get(iter) is None

    def test_x_dispatches_by_exact_callable_identity(self):
        assert isinstance(X(range)(3), wind_range)
        assert isinstance(X.count(2, 3), wind_count)
        assert isinstance(X.repeat("x", 2), wind_repeat)

    def test_x_wraps_unregistered_callable_and_limits_unsafe_extent(self):
        from aleff._multishot.v1.adapters import native_continuation_active

        observed: list[bool] = []

        def operation(value: int) -> int:
            observed.append(native_continuation_active())
            return value + 1

        wrapped = X(operation)

        assert wrapped(4) == 5
        assert observed == [True]
        assert native_continuation_active() is False
        assert wrapped.__name__ == operation.__name__

    def test_x_resets_unsafe_extent_when_wrapped_callable_raises(self):
        from aleff._multishot.v1.adapters import native_continuation_active

        def operation() -> None:
            assert native_continuation_active()
            raise RuntimeError("failed")

        with pytest.raises(RuntimeError, match="failed"):
            X(operation)()

        assert native_continuation_active() is False

    def test_range_matches_the_builtin(self):
        with X.range(2, 8, 2) as values:
            assert list(values) == list(range(2, 8, 2))

    def test_count_restores_position_for_each_shot(self):
        choose: Effect[[], int] = effect("choose")
        handler: Handler[list[tuple[int, int, int]]] = create_handler(choose)

        @handler.on(choose)
        def _choose(k: Resume[int, list[tuple[int, int, int]]]):
            return k(10) + k(20)

        def run() -> list[tuple[int, int, int]]:
            with X.count(0) as values:
                first = next(values)
                chosen = choose()
                return [(first, chosen, next(values))]
            raise AssertionError("unreachable")

        assert handler(run) == [(0, 10, 1), (0, 20, 1)]

    def test_repeat_restores_remaining_count_for_each_shot(self):
        choose: Effect[[], str] = effect("choose")
        handler: Handler[list[tuple[str, str, str]]] = create_handler(choose)

        @handler.on(choose)
        def _choose(k: Resume[str, list[tuple[str, str, str]]]):
            return k("a") + k("b")

        def run() -> list[tuple[str, str, str]]:
            with X.repeat("x", 2) as values:
                first = next(values)
                chosen = choose()
                return [(first, chosen, next(values))]
            raise AssertionError("unreachable")

        assert handler(run) == [("x", "a", "x"), ("x", "b", "x")]

    def test_direct_adapt_uses_the_same_registry(self):
        with adapt(range, 3) as values:
            assert list(values) == [0, 1, 2]

    def test_repeat_treats_negative_times_as_zero(self):
        with X.repeat("x", -1) as values:
            assert list(values) == []
