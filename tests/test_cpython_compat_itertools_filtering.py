"""Differential compatibility tests for the itertools adapters in Issue #55."""

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible


def test_compress_matches_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        selector_calls = []

        class Selector:
            def __init__(self, name, value):
                self.name = name
                self.value = value

            def __bool__(self):
                selector_calls.append(self.name)
                return self.value

        data = [{"name": "a"}, {"name": "b"}, {"name": "c"}]
        selected = list(
            itertools.compress(
                data,
                (Selector("first", True), Selector("second", False), Selector("third", True)),
            )
        )
        print("selected", [item["name"] for item in selected])
        print("identity", selected[0] is data[0], selected[1] is data[2])
        print("selector-calls", selector_calls)

        events = []

        def source():
            for value in ("x", "y", "z"):
                events.append("source-" + value)
                yield value

        iterator = itertools.compress(source(), (0, 1))
        print("lazy-before", events)
        print("lazy-first", next(iterator), events)
        show("lazy-end", lambda: next(iterator))
        print("lazy-after", events)

        show("empty", lambda: list(itertools.compress((), ())))
        show("keyword", lambda: list(itertools.compress(data=[1, 2], selectors=[1, 0])))
        show("missing", lambda: itertools.compress())
        show("one-argument", lambda: itertools.compress([1]))
        show("bad-data", lambda: itertools.compress(1, [1]))
        show("bad-selectors", lambda: itertools.compress([1], 1))
        show("extra-keyword", lambda: itertools.compress([1], [1], unexpected=True))
            """
        )
    )


def test_count_matches_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools
        from decimal import Decimal
        from fractions import Fraction

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        print("integers", list(itertools.islice(itertools.count(7, -3), 5)))
        print("float", list(itertools.islice(itertools.count(1.5, 0.25), 4)))
        print("fraction", list(itertools.islice(itertools.count(Fraction(1, 3), Fraction(1, 6)), 4)))
        print("decimal", list(itertools.islice(itertools.count(Decimal("1.2"), Decimal("0.4")), 4)))
        print("large", list(itertools.islice(itertools.count(10**100, 10**100), 3)))
        print("zero-step", list(itertools.islice(itertools.count(9, 0), 3)))
        print("keyword", list(itertools.islice(itertools.count(start=2, step=5), 4)))

        counter = itertools.count(0)
        print("repr-before", repr(counter))
        print("next", next(counter))
        print("repr-after", repr(counter))
        print("repr-after-second", repr(counter), next(counter), repr(counter))

        class FailingStep(int):
            def __new__(cls):
                return int.__new__(cls, 2)

            def __radd__(self, other):
                raise ValueError("step failed")

        failing = itertools.count(0, FailingStep())
        show("failing-first", lambda: next(failing))
        show("failing-second", lambda: next(failing))
        show("bad-step", lambda: next(itertools.count(0, None)))
        show("bad-keyword", lambda: itertools.count(start=0, unexpected=1))
        show("too-many", lambda: itertools.count(0, 1, 2))
            """
        )
    )


def test_cycle_matches_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        events = []

        def source():
            events.append("start")
            for value in ("a", "b"):
                events.append("yield-" + value)
                yield value
            events.append("stop")

        iterator = itertools.cycle(source())
        print("before", events)
        print("values", [next(iterator) for _ in range(5)])
        print("after", events)

        mutable = [["left"], ["right"]]
        repeated = itertools.cycle(mutable)
        first = next(repeated)
        mutable[0].append("changed")
        print("mutation", first, next(repeated), next(repeated), first is mutable[0])
        print("empty", list(itertools.islice(itertools.cycle(()), 2)))
        print("single", list(itertools.islice(itertools.cycle(("only",)), 4)))

        show("keyword", lambda: itertools.cycle(iterable=[1]))
        show("missing", lambda: itertools.cycle())
        show("bad-iterable", lambda: itertools.cycle(1))
        show("extra-keyword", lambda: itertools.cycle([1], unexpected=True))
            """
        )
    )


def test_dropwhile_matches_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        calls = []

        def predicate(value):
            calls.append(value)
            return value < 3

        print("transition", list(itertools.dropwhile(predicate, (1, 2, 3, 4))))
        print("calls", calls)
        print("all-dropped", list(itertools.dropwhile(lambda value: True, (1, 2))))
        print("none-dropped", list(itertools.dropwhile(lambda value: False, (1, 2))))
        print("empty", list(itertools.dropwhile(lambda value: True, ())))

        show("missing", lambda: itertools.dropwhile(lambda value: True))
        show("bad-predicate", lambda: next(itertools.dropwhile(None, (1,))))
        show("bad-iterable", lambda: itertools.dropwhile(lambda value: True, 1))
        show("keyword", lambda: itertools.dropwhile(predicate=lambda value: False, iterable=[1]))
            """
        )
    )


def test_dropwhile_consumes_item_after_predicate_error() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        class Once:
            def __init__(self):
                self.calls = 0

            def __call__(self, value):
                self.calls += 1
                if self.calls == 1:
                    raise ValueError("predicate failed")
                return False

        failing_predicate = Once()
        failing_iterator = itertools.dropwhile(failing_predicate, iter([10, 20]))
        show("predicate-error", lambda: next(failing_iterator))
        show("after-error", lambda: next(failing_iterator))
        print("error-calls", failing_predicate.calls)
            """
        )
    )


def test_dropwhile_reentrancy_matches_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        events = []
        entered = False
        nested_iterator = None

        def reentrant(value):
            global entered
            events.append("predicate-" + str(value))
            if value == 1 and not entered:
                entered = True
                show("nested", lambda: next(nested_iterator))
            return False

        nested_iterator = itertools.dropwhile(reentrant, iter([1, 2, 3]))
        show("first", lambda: next(nested_iterator))
        show("second", lambda: next(nested_iterator))
        print("events", events)
            """
        )
    )


def test_filterfalse_matches_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        print("predicate", list(itertools.filterfalse(lambda value: value % 2, range(6))))
        print("empty", list(itertools.filterfalse(bool, ())))

        show("missing", lambda: itertools.filterfalse())
        show("missing-iterable", lambda: itertools.filterfalse(bool))
        show("bad-iterable", lambda: itertools.filterfalse(bool, 1))
        show("keyword", lambda: itertools.filterfalse(predicate=bool, iterable=[0, 1]))
            """
        )
    )


def test_filterfalse_none_predicate_matches_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        print("none-predicate", list(itertools.filterfalse(None, (0, 1, False, 2, "", "x"))))
            """
        )
    )


def test_filterfalse_consumes_item_after_predicate_error() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        class Once:
            def __init__(self):
                self.calls = 0

            def __call__(self, value):
                self.calls += 1
                if self.calls == 1:
                    raise ValueError("predicate failed")
                return False

        failing_predicate = Once()
        failing_iterator = itertools.filterfalse(failing_predicate, iter([10, 20]))
        show("predicate-error", lambda: next(failing_iterator))
        show("after-error", lambda: next(failing_iterator))
        print("error-calls", failing_predicate.calls)
            """
        )
    )


def test_filterfalse_reentrancy_matches_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import itertools

        def show(label, operation):
            try:
                print(label, "ok", operation())
            except Exception as exc:
                print(label, "error", type(exc).__name__)

        events = []
        entered = False
        nested_iterator = None

        def reentrant(value):
            global entered
            events.append("predicate-" + str(value))
            if value == 1 and not entered:
                entered = True
                show("nested", lambda: next(nested_iterator))
            return False

        nested_iterator = itertools.filterfalse(reentrant, iter([1, 2, 3]))
        show("first", lambda: next(nested_iterator))
        show("second", lambda: next(nested_iterator))
        print("events", events)
            """
        )
    )
