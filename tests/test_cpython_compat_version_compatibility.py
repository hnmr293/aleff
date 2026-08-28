"""Regression tests for version-dependent CPython compatibility."""

from __future__ import annotations

from collections.abc import Callable
from textwrap import dedent
from typing import Any

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def _check(source: str, *, timeout: float = 10) -> None:
    assert_cpython_compatible(dedent(source), timeout=timeout)


def test_normal_builtin_and_container_behavior() -> None:
    _check(
        """
        values = [1, 2, 3]
        mapping = {"a": 1}
        result = (
            len(values),
            sum(values),
            list(reversed(values)),
            mapping.get("missing", 9),
            {2, 1} == {1, 2},
            b"-".join((b"a", b"b")),
        )
        print(repr(result))
        """
    )


def test_builtin_errors_are_unchanged() -> None:
    _check(
        """
        import operator

        def describe(call):
            try:
                call()
            except BaseException as exc:
                return (type(exc).__name__, str(exc))
            return ("returned", "")

        calls = (
            lambda: [][1],
            lambda: {}["missing"],
            lambda: int("not-an-int"),
            lambda: operator.index("not-an-index"),
            lambda: b",".join((b"a", 1)),
        )
        print(repr([describe(call) for call in calls]))
        """
    )


def test_builtin_signature_metadata_is_preserved() -> None:
    _check(
        """
        import inspect

        def describe(obj):
            try:
                return str(inspect.signature(obj))
            except BaseException as exc:
                return (type(exc).__name__, str(exc))

        signatures = (
            describe(len),
            describe(list.extend),
            describe(dict.get),
            describe(str.join),
        )
        print(repr(signatures))
        """
    )


def test_codec_error_handlers_resume_with_the_remaining_input() -> None:
    import codecs

    def run_with_effectful_error(
        error_name: str,
        operation: Callable[[], Any],
        replacements: tuple[str, ...],
    ) -> list[Any]:
        choose = effect(f"{error_name}_choose")
        handler = create_handler(choose)

        def codec_error(exc: UnicodeError) -> tuple[Any, int]:
            return choose(), exc.end  # pyright: ignore[reportAttributeAccessIssue, reportUnknownMemberType, reportUnknownVariableType]

        codecs.register_error(error_name, codec_error)

        @handler.on(choose)
        def resume(k: Any) -> list[Any]:
            return [k(replacement) for replacement in replacements]

        return handler(operation)

    actual = (
        run_with_effectful_error(
            "issue55_encode_effect",
            lambda: "\N{LATIN SMALL LETTER E WITH ACUTE}X".encode("ascii", "issue55_encode_effect"),
            ("?", "!"),
        ),
        run_with_effectful_error(
            "issue55_decode_effect",
            lambda: b"\xffX".decode("ascii", "issue55_decode_effect"),
            ("?", "!"),
        ),
        run_with_effectful_error(
            "issue55_decode_nul_effect",
            lambda: b"\xffX".decode("ascii", "issue55_decode_nul_effect"),
            ("\x00", "?"),
        ),
    )
    assert actual == ([b"?X", b"!X"], ["?X", "!X"], ["\x00X", "?X"])


def test_dict_subclass_fromkeys_constructs_the_subclass_once() -> None:
    _check(
        """
        class D(dict):
            pass

        try:
            result = D.fromkeys(("a", "b"), 7)
        except BaseException as exc:
            result = (type(exc).__name__,)
        else:
            result = (type(result).__name__, result)
        print(repr(result))
        """
    )


def test_dict_lookup_uses_the_stored_hash() -> None:
    _check(
        """
        class Key:
            def __init__(self, hashes):
                self.hashes = iter(hashes)

            def __hash__(self):
                return next(self.hashes)

            def __eq__(self, other):
                return isinstance(other, Key)

        stored_for_get = Key((1, 2))
        query_for_get = Key((1,))
        get_result = {stored_for_get: "get-value"}.get(query_for_get)

        stored_for_pop = Key((1, 2))
        query_for_pop = Key((1,))
        pop_mapping = {stored_for_pop: "pop-value"}
        pop_result = pop_mapping.pop(query_for_pop, "missing")
        print(repr((get_result, pop_result, len(pop_mapping))))
        """
    )


def test_bulk_mutators_keep_partial_updates_before_an_error() -> None:
    _check(
        """
        class FailingValues:
            def __iter__(self):
                yield 1
                raise RuntimeError("iterator failed")

        class FailingPairs:
            def __iter__(self):
                yield ("first", 1)
                raise RuntimeError("iterator failed")

        values = []
        mapping = {}
        result = {}
        try:
            values.extend(FailingValues())
        except RuntimeError as exc:
            result["list.extend"] = (type(exc).__name__, str(exc), values)
        try:
            mapping.update(FailingPairs())
        except RuntimeError as exc:
            result["dict.update"] = (type(exc).__name__, str(exc), mapping)

        items = set()
        try:
            items.update(FailingValues())
        except RuntimeError as exc:
            result["set.update"] = (type(exc).__name__, str(exc), items)
        print(repr(result))
        """
    )


def test_sort_reports_mutation_during_sort() -> None:
    _check(
        """
        values = [2, 1]

        def key(value):
            values.append(99)
            return value

        try:
            values.sort(key=key)
        except BaseException as exc:
            result = (type(exc).__name__, str(exc), values)
        else:
            result = ("returned", "", values)
        print(repr(result))
        """
    )


def test_range_count_and_index_use_arithmetic_for_huge_ranges() -> None:
    _check(
        """
        values = range(0, 10**100, 3)
        print(repr((values.count(10**99), values.index(10**99 - 1))))
        """,
        timeout=2,
    )
