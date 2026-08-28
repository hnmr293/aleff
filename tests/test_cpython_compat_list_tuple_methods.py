"""CPython differential regression tests for Issue #55 container adapters."""

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible


def test_list_extend_normal_and_argument_errors() -> None:
    assert_cpython_compatible(
        dedent(
            """
        def report(label, operation):
            try:
                print(label, operation())
            except Exception as exc:
                print(label, type(exc).__name__, str(exc))

        values = [1]
        result = values.extend((2, 3))
        print("normal", result, values)
        report("non_iterable", lambda: [].extend(42))
        report("missing", lambda: [].extend())
        report("too_many", lambda: [].extend((1,), (2,)))
            """
        )
    )


def test_list_extend_length_hint_and_partial_failure() -> None:
    assert_cpython_compatible(
        dedent(
            """
        class Hint:
            def __iter__(self):
                return self

            def __next__(self):
                raise StopIteration

            def __length_hint__(self):
                raise RuntimeError("hint")

        values = [0]
        try:
            values.extend(Hint())
        except Exception as exc:
            print("hint", type(exc).__name__, str(exc), values)

        class FailingIterator:
            def __init__(self):
                self.calls = 0

            def __iter__(self):
                return self

            def __next__(self):
                self.calls += 1
                if self.calls == 1:
                    return 1
                raise RuntimeError("boom")

            def __length_hint__(self):
                return 1

        values = [0]
        try:
            values.extend(FailingIterator())
        except Exception as exc:
            print("partial", type(exc).__name__, str(exc), values)
            """
        )
    )


def test_list_and_tuple_search_normal_corner_and_errors() -> None:
    assert_cpython_compatible(
        dedent(
            """
        values = [0, 1, 2, 1]
        print("list", values.count(1), values.index(1), values.index(1, -3, -1), 2 in values)
        remaining = values.copy()
        print("remove", remaining.remove(1), remaining)

        tuple_values = (0, 1, 2, 1)
        print("tuple", tuple_values.count(1), tuple_values.index(1), 2 in tuple_values)

        def report(label, operation):
            try:
                print(label, operation())
            except Exception as exc:
                print(label, type(exc).__name__, str(exc))

        report("list_index_missing", lambda: [1].index(2))
        report("tuple_index_missing", lambda: (1,).index(2))
        report("list_remove_missing", lambda: [1].remove(2))
        report("index_outside", lambda: [1, 2].index(1, 3, 4))
            """
        )
    )


def test_search_operators_normalize_results_and_short_circuit_identity() -> None:
    assert_cpython_compatible(
        dedent(
            """
        class Match:
            def __eq__(self, other):
                return 2 if other == "needle" else 0

        item = Match()
        values = [item]
        contains = "needle" in values
        print("contains", contains, type(contains).__name__)
        print("count", values.count("needle"), values.index("needle"))

        class Exploding:
            def __eq__(self, other):
                raise AssertionError("comparison was not short-circuited")

        needle = object()
        print("identity", needle in [needle, Exploding()])
        print("tuple_identity", needle in (needle, Exploding()))
            """
        )
    )


def test_list_remove_does_not_dispatch_list_subclass_delitem() -> None:
    assert_cpython_compatible(
        dedent(
            """
        events = []

        class Subclass(list):
            def __delitem__(self, index):
                events.append(index)
                super().__delitem__(index)

        values = Subclass([1, 2])
        print("return", values.remove(1))
        print("events", events)
        print("values", values)
            """
        )
    )


def test_list_sort_normal_stable_and_argument_errors() -> None:
    assert_cpython_compatible(
        dedent(
            """
        values = [(2, "first"), (1, "only"), (2, "second")]
        result = values.sort(key=lambda item: item[0])
        print("stable", result, values)

        def report(label, operation):
            try:
                print(label, operation())
            except Exception as exc:
                print(label, type(exc).__name__, str(exc))

        report("positional", lambda: [1].sort(1))
        report("unknown_keyword", lambda: [1].sort(unknown=True))
            """
        )
    )


def test_list_sort_reverse_comparison_and_truthiness_timing() -> None:
    assert_cpython_compatible(
        dedent(
            """
        class Key:
            def __init__(self, value):
                self.value = value

            def __lt__(self, other):
                return self.value < other.value

            def __gt__(self, other):
                return False

        values = [1, 2]
        values.sort(key=Key, reverse=True)
        print("comparison", values)

        values = [2, 1]

        class Reverse:
            def __bool__(self):
                values.append(3)
                return False

        values.sort(reverse=Reverse())
        print("truthiness", values)
            """
        )
    )


def test_list_sort_detects_callback_mutation() -> None:
    assert_cpython_compatible(
        dedent(
            """
        values = [2, 1]

        def key(value):
            values.append(99)
            return value

        try:
            result = values.sort(key=key)
        except Exception as exc:
            print("error", type(exc).__name__, str(exc), values)
        else:
            print("success", result, values)
            """
        )
    )


def test_container_method_signatures_match_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import inspect

        methods = (
            (list, "extend"),
            (list, "sort"),
            (list, "count"),
            (list, "index"),
            (list, "remove"),
            (tuple, "count"),
            (tuple, "index"),
        )
        for owner, name in methods:
            method = getattr(owner, name)
            print(owner.__name__, name, repr(method.__text_signature__))
            try:
                print("signature", inspect.signature(method))
            except Exception as exc:
                print("signature_error", type(exc).__name__, str(exc))
            """
        )
    )


def test_list_and_tuple_comparisons_match_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        def comparisons(left, right):
            return (
                left == right,
                left != right,
                left < right,
                left <= right,
                left > right,
                left >= right,
            )

        print("list", comparisons([1, 2], [1, 3]))
        print("tuple", comparisons((1, 2), (1, 3)))
        print("prefix", comparisons([1], [1, 0]))

        for label, operation in (
            ("list_tuple_eq", lambda: [1] == (1,)),
            ("list_tuple_lt", lambda: [1] < (1,)),
            ("tuple_list_eq", lambda: (1,) == [1]),
            ("tuple_list_lt", lambda: (1,) < [1]),
        ):
            try:
                print(label, operation())
            except Exception as exc:
                print(label, type(exc).__name__, str(exc))
            """
        )
    )
