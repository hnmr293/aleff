"""Differential tests for item access and iteration protocols.

The compatibility helper runs each source once on pristine CPython and once
after importing aleff.  Keeping the observations as plain, deterministic
text makes failures useful without depending on implementation addresses.
"""

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible


def test_custom_item_access_protocols_and_errors() -> None:
    assert_cpython_compatible(
        dedent("""
        import operator

        def observe(label, operation):
            try:
                print(label, "ok", repr(operation()))
            except BaseException as exc:
                print(label, "error", type(exc).__name__)

        class Items:
            def __init__(self):
                self.values = {"answer": 41}

            def __getitem__(self, key):
                return self.values[key] + 1

            def __setitem__(self, key, value):
                self.values[key] = value

            def __delitem__(self, key):
                del self.values[key]

        target = Items()
        observe("getitem", lambda: target["answer"])
        observe("operator_getitem", lambda: operator.getitem(target, "answer"))
        observe("setitem", lambda: (operator.setitem(target, "new", 7), target.values))
        observe("delitem", lambda: (operator.delitem(target, "new"), target.values))
        observe("missing_getitem", lambda: target["missing"])
        observe("missing_delitem", lambda: operator.delitem(target, "missing"))

        class NoItems:
            pass

        observe("unsupported_getitem", lambda: NoItems()[0])
        observe("unsupported_setitem", lambda: operator.setitem(NoItems(), 0, 1))
        observe("unsupported_delitem", lambda: operator.delitem(NoItems(), 0))

        class Generic:
            @classmethod
            def __class_getitem__(cls, key):
                return (cls.__name__, key)

        observe("class_getitem", lambda: Generic["item"])
        """),
    )


def test_builtin_item_access_boundaries_and_dict_hashing() -> None:
    assert_cpython_compatible(
        dedent("""
        def observe(label, operation):
            try:
                print(label, "ok", repr(operation()))
            except BaseException as exc:
                print(label, "error", type(exc).__name__)

        values = ["zero", "one", "two"]
        observe("list_negative", lambda: values[-1])
        observe("list_slice", lambda: values[1:3])
        observe("list_out_of_range", lambda: values[3])
        observe("list_bad_index", lambda: values["one"])
        observe("tuple_negative", lambda: (10, 20)[-2])
        observe("bytes_slice", lambda: b"abc"[1:])

        class EqualKeys:
            def __init__(self, hash_value):
                self.hash_value = hash_value

            def __hash__(self):
                return self.hash_value

            def __eq__(self, other):
                return isinstance(other, EqualKeys)

        mapping = {EqualKeys(1): "value"}
        observe("dict_syntax_different_hash", lambda: mapping[EqualKeys(2)])
        observe("dict_dunder_different_hash", lambda: dict.__getitem__(mapping, EqualKeys(2)))

        huge = 2**100

        class HugeHash:
            def __hash__(self):
                return huge

        huge_mapping = {huge: "huge value"}
        observe("dict_dunder_huge_hash", lambda: dict.__getitem__(huge_mapping, HugeHash()))

        class Stored:
            def __hash__(self):
                return 1

            def __eq__(self, other):
                return isinstance(other, Lookup)

        class Lookup:
            def __hash__(self):
                return 1

        def mapping_snapshot(mapping):
            return sorted((type(key).__name__, value) for key, value in mapping.items())

        equality_mapping = {Stored(): "equal value"}
        observe("dict_contains_equal", lambda: dict.__contains__(equality_mapping, Lookup()))
        observe("dict_get_equal", lambda: dict.__getitem__(equality_mapping, Lookup()))
        observe(
            "dict_set_equal",
            lambda: (dict.__setitem__(equality_mapping, Lookup(), "updated"), mapping_snapshot(equality_mapping)),
        )
        observe(
            "dict_del_equal",
            lambda: (dict.__delitem__(equality_mapping, Lookup()), mapping_snapshot(equality_mapping)),
        )
        """),
    )


def test_iteration_protocols_normal_errors_and_corner_cases() -> None:
    assert_cpython_compatible(
        dedent("""
        def observe(label, operation):
            try:
                print(label, "ok", repr(operation()))
            except BaseException as exc:
                print(label, "error", type(exc).__name__)

        class Iterator:
            def __init__(self):
                self.index = 0

            def __iter__(self):
                return self

            def __next__(self):
                if self.index == 2:
                    raise StopIteration
                value = self.index
                self.index += 1
                return value

        iterator = Iterator()
        observe("iter_identity", lambda: iter(iterator) is iterator)
        observe("list_iteration", lambda: list(iterator))
        observe("next_default", lambda: next(iterator, "done"))
        observe("next_non_iterator", lambda: next(object()))

        class BadIterable:
            def __iter__(self):
                return 3

        observe("bad_iter_result", lambda: iter(BadIterable()))

        class Reversible:
            def __reversed__(self):
                return iter((3, 2, 1))

        class SequenceFallback:
            def __len__(self):
                return 3

            def __getitem__(self, index):
                if index < 0:
                    raise IndexError
                return index + 10

        observe("custom_reversed", lambda: list(reversed(Reversible())))
        observe("sequence_reversed", lambda: list(reversed(SequenceFallback())))
        observe("reversed_non_reversible", lambda: reversed(object()))

        calls = iter(("first", "stop", "after"))
        observe("callable_sentinel_iter", lambda: list(iter(calls.__next__, "stop")))
        observe("callable_sentinel_bad_callable", lambda: iter(1, 2))
        """),
    )


def test_iterator_result_types_and_item_producing_factories() -> None:
    assert_cpython_compatible(
        dedent("""
        import itertools
        import operator

        def observe(label, operation):
            try:
                print(label, "ok", repr(operation()))
            except BaseException as exc:
                print(label, "error", type(exc).__name__)

        class Target:
            def __getitem__(self, key):
                return {0: "zero", 1: "one"}[key]

        getter = operator.itemgetter(1)
        observe("itemgetter_value", lambda: getter(Target()))
        observe("itemgetter_type", lambda: (type(getter).__module__, type(getter).__name__))
        observe("itemgetter_bad_constructor", lambda: operator.itemgetter())

        grouped = itertools.groupby((1, 1, 2))

        def consume_groups():
            result = []
            for key, group in grouped:
                result.append((key, type(group).__module__, type(group).__name__, list(group)))
            return result

        observe("groupby_groups", consume_groups)
        observe("groupby_empty", lambda: list(itertools.groupby(())))

        def consume_tee():
            first, second = itertools.tee((1, 2))
            return (
                (type(first).__module__, type(first).__name__),
                (type(second).__module__, type(second).__name__),
                list(first),
                list(second),
            )

        observe("tee_values_and_types", consume_tee)
        observe("tee_zero", lambda: itertools.tee((1,), 0))
        observe("tee_bad_count", lambda: itertools.tee((1,), -1))
        """),
    )


def test_islice_arguments_and_iteration_state() -> None:
    assert_cpython_compatible(
        dedent("""
        import itertools

        def observe(label, operation):
            try:
                print(label, "ok", repr(operation()))
            except BaseException as exc:
                print(label, "error", type(exc).__name__)

        observe("islice_stop", lambda: list(itertools.islice(range(10), 5)))
        observe("islice_start_stop", lambda: list(itertools.islice(range(10), 2, 7)))
        observe("islice_start_stop_step", lambda: list(itertools.islice(range(10), 2, 9, 3)))
        observe("islice_empty", lambda: list(itertools.islice(range(10), 4, 4)))
        observe("islice_negative_start", lambda: itertools.islice(range(3), -1))
        observe("islice_zero_step", lambda: itertools.islice(range(3), 0, 3, 0))
        observe("islice_bad_argument", lambda: itertools.islice(range(3), "stop"))
        observe("islice_too_many_arguments", lambda: itertools.islice(range(3), 0, 1, 1, 1))
        """),
    )


def test_async_iteration_protocols_and_anext_return_type() -> None:
    assert_cpython_compatible(
        dedent("""
        import asyncio
        import warnings

        warnings.filterwarnings("ignore", category=RuntimeWarning)

        def observe(label, operation):
            try:
                print(label, "ok", repr(operation()))
            except BaseException as exc:
                print(label, "error", type(exc).__name__)

        class AsyncIterator:
            def __aiter__(self):
                return self

            async def __anext__(self):
                raise StopAsyncIteration

        class BadAsyncIterable:
            def __aiter__(self):
                return object()

        async_iterator = AsyncIterator()
        observe("aiter_identity", lambda: aiter(async_iterator) is async_iterator)
        observe("aiter_bad_result", lambda: aiter(BadAsyncIterable()))

        def awaitable_type():
            awaitable = anext(AsyncIterator())
            result = (type(awaitable).__module__, type(awaitable).__name__)
            if hasattr(awaitable, "close"):
                awaitable.close()
            return result

        observe("anext_awaitable_type", awaitable_type)

        class Values:
            def __init__(self):
                self.done = False

            def __aiter__(self):
                return self

            async def __anext__(self):
                if self.done:
                    raise StopAsyncIteration
                self.done = True
                return 42

        async def consume():
            iterator = Values()
            return await anext(iterator), await anext(iterator, "default")

        observe("anext_value_and_default", lambda: asyncio.run(consume()))
        """),
    )
