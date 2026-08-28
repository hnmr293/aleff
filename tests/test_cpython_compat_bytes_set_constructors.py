"""CPython compatibility tests for the set and byte-sequence constructors."""

from __future__ import annotations

import textwrap
from typing import Any, Callable, cast

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def _compatible(source: str) -> None:
    assert_cpython_compatible(textwrap.dedent(source))


def _resume_outcomes(
    run: Callable[[Callable[[], Any]], Any],
    values: tuple[Any, ...] = (1, 10),
) -> list[tuple[str, Any]]:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in values:
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:  # The exception is part of the result.
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    return cast(list[tuple[str, Any]], handler(lambda: run(choose)))


def test_set_and_frozenset_normal_construction() -> None:
    _compatible(
        """
        def outcome(constructor):
            try:
                value = constructor((3, 1, 3, True))
            except Exception as exc:
                return ("raise", type(exc).__name__, str(exc))
            return (type(value).__name__, sorted(value))

        print(outcome(set))
        print(outcome(frozenset))
        print(type(set()).__name__, type(frozenset()).__name__)
        """
    )


def test_bytes_and_bytearray_normal_construction() -> None:
    _compatible(
        """
        def outcome(constructor, source):
            try:
                value = constructor(source)
            except Exception as exc:
                return ("raise", type(exc).__name__, str(exc))
            if isinstance(value, (bytes, bytearray)):
                return (type(value).__name__, bytes(value))
            return (type(value).__name__, value)

        class BytesObject:
            def __bytes__(self):
                return b"custom"

        class IndexObject:
            def __index__(self):
                return 3

        print(outcome(bytes, (0, 1, 255)))
        print(outcome(bytearray, (0, 1, 255)))
        print(outcome(bytes, memoryview(b"view")))
        print(outcome(bytearray, memoryview(b"view")))
        print(outcome(bytes, BytesObject()))
        print(outcome(bytes, IndexObject()))
        print(outcome(bytearray, IndexObject()))
        """
    )


def test_set_and_frozenset_invalid_items_and_arguments() -> None:
    _compatible(
        """
        def outcome(call):
            try:
                value = call()
            except Exception as exc:
                return ("raise", type(exc).__name__, str(exc))
            return (type(value).__name__, sorted(value))

        print(outcome(lambda: set(([1],))))
        print(outcome(lambda: frozenset(([1],))))
        print(outcome(lambda: set(42)))
        print(outcome(lambda: frozenset(42)))
        print(outcome(lambda: set()))
        print(outcome(lambda: frozenset()))
        """
    )


def test_bytes_and_bytearray_invalid_items_and_arguments() -> None:
    _compatible(
        """
        def outcome(call):
            try:
                value = call()
            except Exception as exc:
                return ("raise", type(exc).__name__, str(exc))
            return (type(value).__name__, bytes(value))

        class InvalidIndex:
            def __index__(self):
                return 256

        print(outcome(lambda: bytes((256,))))
        print(outcome(lambda: bytearray((-1,))))
        print(outcome(lambda: bytes((InvalidIndex(),))))
        print(outcome(lambda: bytearray((InvalidIndex(),))))
        print(outcome(lambda: bytes("text")))
        print(outcome(lambda: bytearray("text")))
        """
    )


def test_set_subclass_storage_is_used_without_overridden_iteration() -> None:
    _compatible(
        """
        class SetSubclass(set):
            def __iter__(self):
                return iter((99,))

        source = SetSubclass((1,))
        print(sorted(set(source)))
        print(sorted(frozenset(source)))
        """
    )


def test_constructor_validation_happens_during_iteration() -> None:
    _compatible(
        """
        def probe(constructor, kind):
            events = []

            class Bad:
                def __hash__(self):
                    events.append("validate")
                    raise ValueError("bad hash")

                def __index__(self):
                    events.append("validate")
                    raise ValueError("bad index")

            class Iterator:
                def __iter__(self):
                    return self

                def __next__(self):
                    count = getattr(self, "count", 0)
                    if count == 0:
                        self.count = 1
                        return Bad()
                    if count == 1:
                        self.count = 2
                        events.append("second")
                        return 0
                    raise StopIteration

            try:
                constructor(Iterator())
            except Exception as exc:
                return (kind, events, type(exc).__name__, str(exc))
            return (kind, events, "return")

        print(probe(set, "hash"))
        print(probe(frozenset, "hash"))
        print(probe(bytes, "index"))
        print(probe(bytearray, "index"))
        """
    )


def test_bytes_uses_iterable_length_hint() -> None:
    _compatible(
        """
        class Iterable:
            def __iter__(self):
                return iter((1,))

            def __len__(self):
                raise ValueError("length requested")

        try:
            print(bytes(Iterable()))
        except Exception as exc:
            print(type(exc).__name__, str(exc))
        """
    )


def test_bytes_and_bytearray_use_special_buffer_lookup() -> None:
    _compatible(
        """
        def outcome(constructor):
            class Buffer:
                def __buffer__(self, flags):
                    return memoryview(b"class")

            source = Buffer()
            source.__buffer__ = lambda flags: memoryview(b"instance")
            return bytes(constructor(source))

        print(outcome(bytes))
        print(outcome(bytearray))
        """
    )


def test_bytes_and_bytearray_use_special_release_buffer_lookup() -> None:
    _compatible(
        """
        def outcome(constructor):
            events = []

            class Buffer:
                def __buffer__(self, flags):
                    return memoryview(b"value")

                def __release_buffer__(self, view):
                    events.append("class")

            source = Buffer()
            source.__release_buffer__ = lambda view: events.append("instance")
            result = constructor(source)
            return (bytes(result), events)

        print(outcome(bytes))
        print(outcome(bytearray))
        """
    )


def test_bytes_and_bytearray_non_iterable_error_messages() -> None:
    _compatible(
        """
        class NotIterable:
            pass

        def outcome(constructor):
            try:
                constructor(NotIterable())
            except Exception as exc:
                return (type(exc).__name__, str(exc))
            return "return"

        print(outcome(bytes))
        print(outcome(bytearray))
        """
    )


def test_set_init_direct_descriptor_clears_before_consuming_source() -> None:
    _compatible(
        """
        target = {1}
        set.__init__(target, target)
        print(sorted(target))

        target = {1}

        class FailingIterable:
            def __iter__(self):
                target.add(2)
                raise ValueError("iteration failed")

        try:
            set.__init__(target, FailingIterable())
        except ValueError as exc:
            print(type(exc).__name__, str(exc), sorted(target))
        """
    )


def test_bytearray_init_direct_descriptor_clears_before_consuming_source() -> None:
    _compatible(
        """
        target = bytearray(b"old")

        class FailingIterable:
            def __iter__(self):
                target.extend(b"during")
                raise ValueError("iteration failed")

        try:
            bytearray.__init__(target, FailingIterable())
        except ValueError as exc:
            print(type(exc).__name__, str(exc), bytes(target))
        """
    )


def test_set_constructor_resumes_hash_conversion() -> None:
    def run(choose: Callable[[], Any]) -> tuple[int, list[str], bool, bool]:
        class Hashable:
            def __init__(self) -> None:
                self.calls = 0

            def __hash__(self) -> int:
                if self.calls == 0:
                    self.calls = 1
                    return cast(int, choose())
                return 1

        result = set((Hashable(),))
        return (
            len(result),
            sorted(type(item).__name__ for item in result),
            1 in result,  # pyright: ignore[reportUnnecessaryContains]
            10 in result,  # pyright: ignore[reportUnnecessaryContains]
        )

    assert _resume_outcomes(run) == [
        ("return", (1, ["Hashable"], False, False)),
        ("return", (1, ["Hashable"], False, False)),
    ]


def test_frozenset_constructor_resumes_hash_conversion() -> None:
    def run(choose: Callable[[], Any]) -> tuple[int, list[str], bool, bool]:
        class Hashable:
            def __init__(self) -> None:
                self.calls = 0

            def __hash__(self) -> int:
                if self.calls == 0:
                    self.calls = 1
                    return cast(int, choose())
                return 1

        result = frozenset((Hashable(),))
        return (
            len(result),
            sorted(type(item).__name__ for item in result),
            1 in result,  # pyright: ignore[reportUnnecessaryContains]
            10 in result,  # pyright: ignore[reportUnnecessaryContains]
        )

    assert _resume_outcomes(run) == [
        ("return", (1, ["Hashable"], False, False)),
        ("return", (1, ["Hashable"], False, False)),
    ]


def test_bytes_constructor_resumes_index_conversion() -> None:
    def run(choose: Callable[[], Any]) -> bytes:
        class Indexable:
            def __init__(self) -> None:
                self.calls = 0

            def __index__(self) -> int:
                if self.calls == 0:
                    self.calls = 1
                    return cast(int, choose())
                return 1

        return bytes((Indexable(),))

    assert _resume_outcomes(run) == [("return", b"\x01"), ("return", b"\x0a")]


def test_bytearray_constructor_resumes_index_conversion() -> None:
    def run(choose: Callable[[], Any]) -> bytearray:
        class Indexable:
            def __init__(self) -> None:
                self.calls = 0

            def __index__(self) -> int:
                if self.calls == 0:
                    self.calls = 1
                    return cast(int, choose())
                return 1

        return bytearray((Indexable(),))

    assert _resume_outcomes(run) == [
        ("return", bytearray(b"\x01")),
        ("return", bytearray(b"\x0a")),
    ]
