"""CPython compatibility tests for operator accessors (Issue #55)."""

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible

try:
    from aleff._multishot.v1 import _aleff as _aleff
except ImportError as exc:
    raise RuntimeError("CPython compatibility tests require a built aleff extension") from exc

if not _aleff.HAS_RESTORE:
    raise RuntimeError("CPython compatibility tests require aleff continuation support")


def test_attrgetter_returns_values_in_cpython_order() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        class Child:
            value = 7
            label = "child"

        class Target:
            nested = Child()
            label = "target"

        target = Target()
        single = operator.attrgetter("nested.value")
        multiple = operator.attrgetter("nested.value", "label", "nested.label")
        print("single", single(target), type(single(target)).__name__)
        result = multiple(target)
        print("multiple", result, type(result).__name__)
        """
        )
    )


def test_itemgetter_returns_single_and_multiple_items() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        values = ["zero", "one", "two", "three"]
        mapping = {"name": "aleff", "number": 55}
        print("single", operator.itemgetter(2)(values), type(operator.itemgetter(2)(values)).__name__)
        print("multiple", operator.itemgetter(0, 3)(values))
        print("slice", operator.itemgetter(slice(1, 3))(values))
        print("mapping", operator.itemgetter("name", "number")(mapping))
        """
        )
    )


def test_methodcaller_forwards_positional_and_keyword_arguments() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        class Target:
            def render(self, prefix, suffix="", *, upper=False):
                text = prefix + "aleff" + suffix
                return text.upper() if upper else text

        target = Target()
        print(operator.methodcaller("render", "<", ">", upper=True)(target))
        print(operator.methodcaller("render", "[", suffix="]")(target))
        """
        )
    )


def test_methodcaller_performs_method_lookup_for_each_call() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        class Target:
            def first(self):
                return "first"

        target = Target()
        caller = operator.methodcaller("first")
        print(caller(target))
        target.first = lambda: "second"
        print(caller(target))
        """
        )
    )


def test_accessor_constructors_preserve_cpython_object_metadata() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator
        import pickle

        values = [
            (operator.attrgetter, ("name",)),
            (operator.itemgetter, (0,)),
            (operator.methodcaller, ("name",)),
        ]
        for factory, arguments in values:
            value = factory(*arguments)
            value_repr = repr(value)
            print(
                factory.__name__,
                type(value).__module__,
                type(value).__name__,
                getattr(value, "__name__", None),
                value_repr.startswith("operator."),
                "operator_accessor" in value_repr,
                getattr(value, "__doc__", None) is None,
            )
            try:
                reduced = value.__reduce__()
                print(
                    "reduce",
                    type(reduced).__name__,
                    len(reduced),
                    getattr(reduced[0], "__module__", None),
                    getattr(reduced[0], "__name__", None),
                    type(reduced[1]).__name__,
                    len(reduced[1]) if isinstance(reduced[1], tuple) else None,
                )
            except Exception as exc:
                print("reduce-error", type(exc).__name__)
            try:
                serialized = pickle.dumps(value)
                restored = pickle.loads(serialized)
                print(
                    "pickle",
                    type(restored).__module__,
                    type(restored).__name__,
                    repr(restored).startswith("operator."),
                )
            except Exception as exc:
                print("pickle-error", type(exc).__name__)
        """
        )
    )


def test_accessor_instances_have_cpython_types() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        for factory, argument in (
            (operator.attrgetter, "name"),
            (operator.itemgetter, 0),
            (operator.methodcaller, "name"),
        ):
            value = factory(argument)
            try:
                result = isinstance(value, factory)
                print(factory.__name__, result)
            except Exception as exc:
                print(factory.__name__, "raise", type(exc).__name__)
        """
        )
    )


def test_methodcaller_does_not_expose_mutable_internal_kwargs() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        class Target:
            def method(self, **kwargs):
                return kwargs

        caller = operator.methodcaller("method", flag=False)
        try:
            caller.__self__[3]["flag"] = True
            print("mutated", caller(Target()))
        except Exception as exc:
            print("unavailable", type(exc).__name__)
        """
        )
    )


def test_attrgetter_interns_dotted_attribute_names() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator
        import sys

        expected = sys.intern("child")

        class Nested:
            def __getattribute__(self, name):
                if name == "child":
                    return name is expected
                return object.__getattribute__(self, name)

        class Target:
            nested = Nested()

        path = ".".join(("nested", "child"))
        print(operator.attrgetter(path)(Target()))
        """
        )
    )


def test_methodcaller_interns_method_names() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator
        import sys

        expected = sys.intern("run")

        class Target:
            def __getattribute__(self, name):
                if name == "run":
                    return lambda: name is expected
                return object.__getattribute__(self, name)

        method_name = "".join(("r", "un"))
        print(operator.methodcaller(method_name)(Target()))
        """
        )
    )


def test_accessor_constructor_and_call_errors_match_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        def outcome(label, operation):
            try:
                print(label, "return", operation())
            except Exception as exc:
                print(label, "raise", type(exc).__name__)

        outcome("attr-no-args", lambda: operator.attrgetter())
        outcome("attr-non-string", lambda: operator.attrgetter(1))
        outcome("attr-keyword-constructor", lambda: operator.attrgetter(attr="value"))
        outcome("item-no-args", lambda: operator.itemgetter())
        outcome("item-keyword-constructor", lambda: operator.itemgetter(item=0))
        outcome("method-no-args", lambda: operator.methodcaller())
        outcome("method-non-string", lambda: operator.methodcaller(1))
        outcome("method-keyword-constructor", lambda: operator.methodcaller(name="method"))

        attr = operator.attrgetter("value")
        item = operator.itemgetter(0)
        caller = operator.methodcaller("method")
        for label, operation in (
            ("attr-no-target", lambda: attr()),
            ("attr-too-many", lambda: attr(1, 2)),
            ("attr-keyword", lambda: attr(target=1)),
            ("item-no-target", lambda: item()),
            ("item-too-many", lambda: item(1, 2)),
            ("item-keyword", lambda: item(target=1)),
            ("method-no-target", lambda: caller()),
            ("method-too-many", lambda: caller(1, 2)),
            ("method-keyword", lambda: caller(target=1)),
        ):
            outcome(label, operation)
        """
        )
    )


def test_accessor_lookup_and_call_errors_are_propagated() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        def outcome(label, operation):
            try:
                print(label, "return", operation())
            except Exception as exc:
                print(label, "raise", type(exc).__name__, str(exc))

        class AttrTarget:
            def __getattribute__(self, name):
                if name == "missing":
                    raise LookupError("attribute failure")
                return object.__getattribute__(self, name)

        class ItemTarget:
            def __getitem__(self, key):
                if key == "missing":
                    raise LookupError("item failure")
                return "value"

        class MethodTarget:
            def method(self):
                raise RuntimeError("method failure")

        outcome("attr", lambda: operator.attrgetter("missing")(AttrTarget()))
        outcome("item", lambda: operator.itemgetter("missing")(ItemTarget()))
        outcome("method", lambda: operator.methodcaller("method")(MethodTarget()))
        outcome("method-missing", lambda: operator.methodcaller("missing")(object()))
        """
        )
    )


def test_multiple_accessor_errors_stop_at_the_failing_operation() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        def outcome(label, operation):
            try:
                print(label, "return", operation())
            except Exception as exc:
                print(label, "raise", type(exc).__name__)

        class AttrTarget:
            def __getattribute__(self, name):
                print("attr", name)
                if name == "bad":
                    raise ValueError("bad attribute")
                return object.__getattribute__(self, name)
            first = "ok"

        class ItemTarget:
            def __getitem__(self, key):
                print("item", key)
                if key == "bad":
                    raise ValueError("bad item")
                return key

        outcome("attr", lambda: operator.attrgetter("first", "bad", "last")(AttrTarget()))
        outcome("item", lambda: operator.itemgetter("first", "bad", "last")(ItemTarget()))
        """
        )
    )


def test_empty_and_dotted_attribute_names_follow_cpython() -> None:
    assert_cpython_compatible(
        dedent(
            """
        import operator

        class Target:
            def __getattribute__(self, name):
                if name in ("", "part"):
                    return name or "empty"
                return object.__getattribute__(self, name)

        for name in ("", "part", "part..part"):
            try:
                print(name, operator.attrgetter(name)(Target()))
            except Exception as exc:
                print(name, "raise", type(exc).__name__)
        """
        )
    )
