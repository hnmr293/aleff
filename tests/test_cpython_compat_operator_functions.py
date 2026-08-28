"""Differential regression tests for the public ``operator`` functions."""

from cpython_compat_support import assert_cpython_compatible


def test_operator_public_metadata_and_signatures_are_compatible() -> None:
    assert_cpython_compatible(
        r"""
import inspect
import json
import operator

excluded = {"is_", "is_not", "is_none", "is_not_none"}
names = sorted(set(operator.__all__) - excluded)

def signature(function):
    try:
        return ["ok", str(inspect.signature(function))]
    except Exception as error:
        return ["raise", type(error).__name__]

def describe_metadata(value):
    if isinstance(value, str):
        return ["str", value]
    return [type(value).__name__, None]

result = {}
for name in names:
    function = getattr(operator, name)
    result[name] = {
        "module": describe_metadata(getattr(function, "__module__", None)),
        "name": getattr(function, "__name__", None),
        "doc_is_none": function.__doc__ is None,
        "signature": signature(function),
    }
print(json.dumps(result, sort_keys=True))
"""
    )


def test_operator_public_functions_normal_behavior() -> None:
    assert_cpython_compatible(
        r"""
import json
import operator

class Matrix:
    def __matmul__(self, other):
        return ("matmul", other)

class Length:
    def __length_hint__(self):
        return 4

class Target:
    value = 7
    nested = type("Nested", (), {"label": "ok"})()

    def method(self, value, *, suffix):
        return (value, suffix)

def record(function):
    value = function()
    return [type(value).__name__, repr(value)]

mapping = {"a": 1}
mutable = ["a"]
cases = {
    "abs": lambda: operator.abs(-3),
    "add": lambda: operator.add(2, 3),
    "and_": lambda: operator.and_(6, 3),
    "call": lambda: operator.call(lambda value: value + 1, 4),
    "concat": lambda: operator.concat([1], [2]),
    "contains": lambda: operator.contains((1, 2), 2),
    "countOf": lambda: operator.countOf((1, 2, 1), 1),
    "delitem": lambda: (operator.delitem(mapping, "a"), mapping),
    "eq": lambda: operator.eq(2, 2),
    "floordiv": lambda: operator.floordiv(7, 2),
    "ge": lambda: operator.ge(2, 2),
    "getitem": lambda: operator.getitem({"key": 3}, "key"),
    "gt": lambda: operator.gt(3, 2),
    "iadd": lambda: (operator.iadd(mutable, ["b"]), mutable),
    "iand": lambda: operator.iand(7, 3),
    "iconcat": lambda: operator.iconcat([1], [2]),
    "ifloordiv": lambda: operator.ifloordiv(7, 2),
    "ilshift": lambda: operator.ilshift(3, 2),
    "imatmul": lambda: operator.imatmul(Matrix(), 5),
    "imod": lambda: operator.imod(7, 3),
    "imul": lambda: operator.imul(3, 4),
    "index": lambda: operator.index(5),
    "indexOf": lambda: operator.indexOf((4, 5), 5),
    "inv": lambda: operator.inv(3),
    "invert": lambda: operator.invert(3),
    "ior": lambda: operator.ior(4, 2),
    "ipow": lambda: operator.ipow(2, 3),
    "irshift": lambda: operator.irshift(8, 1),
    "isub": lambda: operator.isub(7, 2),
    "itruediv": lambda: operator.itruediv(7, 2),
    "ixor": lambda: operator.ixor(7, 3),
    "le": lambda: operator.le(2, 2),
    "length_hint": lambda: operator.length_hint(Length()),
    "lshift": lambda: operator.lshift(3, 2),
    "lt": lambda: operator.lt(2, 3),
    "matmul": lambda: operator.matmul(Matrix(), 5),
    "mod": lambda: operator.mod(7, 3),
    "mul": lambda: operator.mul(3, 4),
    "ne": lambda: operator.ne(2, 3),
    "neg": lambda: operator.neg(3),
    "not_": lambda: operator.not_(0),
    "or_": lambda: operator.or_(4, 2),
    "pos": lambda: operator.pos(-3),
    "pow": lambda: operator.pow(2, 3),
    "rshift": lambda: operator.rshift(8, 1),
    "setitem": lambda: (operator.setitem(mapping, "b", 2), mapping),
    "sub": lambda: operator.sub(7, 2),
    "truediv": lambda: operator.truediv(7, 2),
    "truth": lambda: operator.truth([1]),
    "xor": lambda: operator.xor(7, 3),
}

accessors = {
    "attrgetter": lambda: operator.attrgetter("nested.label", "value")(Target()),
    "itemgetter": lambda: operator.itemgetter("x", 1)({"x": 3, 1: 4}),
    "methodcaller": lambda: operator.methodcaller("method", 5, suffix="ok")(Target()),
}
cases.update(accessors)
assert set(cases) == set(operator.__all__) - {"is_", "is_not", "is_none", "is_not_none"}
print(json.dumps({name: record(function) for name, function in sorted(cases.items())}, sort_keys=True))
"""
    )


def test_operator_call_forwards_keyword_arguments() -> None:
    assert_cpython_compatible(
        r"""
import operator

def callback(*args, **kwargs):
    return args, kwargs

print(repr(operator.call(callback, 1, flag=True)))
"""
    )


def test_operator_notimplemented_fallbacks_are_preserved() -> None:
    assert_cpython_compatible(
        r"""
import json
import operator
import sys

binary = {
    "add": ("__add__", "__radd__", operator.add),
    "and_": ("__and__", "__rand__", operator.and_),
    "floordiv": ("__floordiv__", "__rfloordiv__", operator.floordiv),
    "lshift": ("__lshift__", "__rlshift__", operator.lshift),
    "matmul": ("__matmul__", "__rmatmul__", operator.matmul),
    "mod": ("__mod__", "__rmod__", operator.mod),
    "mul": ("__mul__", "__rmul__", operator.mul),
    "or_": ("__or__", "__ror__", operator.or_),
    "pow": ("__pow__", "__rpow__", operator.pow),
    "rshift": ("__rshift__", "__rrshift__", operator.rshift),
    "sub": ("__sub__", "__rsub__", operator.sub),
    "truediv": ("__truediv__", "__rtruediv__", operator.truediv),
    "xor": ("__xor__", "__rxor__", operator.xor),
}
comparisons = {
    "eq": ("__eq__", "__eq__", operator.eq),
    "ge": ("__ge__", "__le__", operator.ge),
    "gt": ("__gt__", "__lt__", operator.gt),
    "le": ("__le__", "__ge__", operator.le),
    "lt": ("__lt__", "__gt__", operator.lt),
    "ne": ("__ne__", "__ne__", operator.ne),
}
inplace = {
    "iadd": ("__iadd__", "__add__", "__radd__", operator.iadd),
    "iand": ("__iand__", "__and__", "__rand__", operator.iand),
    "ifloordiv": ("__ifloordiv__", "__floordiv__", "__rfloordiv__", operator.ifloordiv),
    "ilshift": ("__ilshift__", "__lshift__", "__rlshift__", operator.ilshift),
    "imatmul": ("__imatmul__", "__matmul__", "__rmatmul__", operator.imatmul),
    "imod": ("__imod__", "__mod__", "__rmod__", operator.imod),
    "imul": ("__imul__", "__mul__", "__rmul__", operator.imul),
    "ior": ("__ior__", "__or__", "__ror__", operator.ior),
    "ipow": ("__ipow__", "__pow__", "__rpow__", operator.ipow),
    "irshift": ("__irshift__", "__rshift__", "__rrshift__", operator.irshift),
    "isub": ("__isub__", "__sub__", "__rsub__", operator.isub),
    "itruediv": ("__itruediv__", "__truediv__", "__rtruediv__", operator.itruediv),
    "ixor": ("__ixor__", "__xor__", "__rxor__", operator.ixor),
}

def outcome(thunk):
    try:
        value = thunk()
        return ["return", type(value).__name__, repr(value)]
    except Exception as error:
        return ["raise", type(error).__name__]

if "aleff" in sys.modules:
    from aleff import create_handler, effect
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def resume(k):
        return k(NotImplemented)

    def choose_notimplemented():
        return choose()

    def run(thunk):
        return handler(thunk)
else:
    def choose_notimplemented():
        return NotImplemented

    def run(thunk):
        return thunk()

results = {}
for name, (left_name, right_name, function) in binary.items():
    left = type("Left", (), {left_name: lambda self, other: choose_notimplemented()})()
    right = type("Right", (), {right_name: lambda self, other: 42})()
    results[name] = outcome(lambda left=left, right=right, function=function: run(lambda: function(left, right)))
for name, (left_name, right_name, function) in comparisons.items():
    left = type("Left", (), {left_name: lambda self, other: choose_notimplemented()})()
    right = type("Right", (), {right_name: lambda self, other: 42})()
    results[name] = outcome(lambda left=left, right=right, function=function: run(lambda: function(left, right)))
for name, (inplace_name, left_name, right_name, function) in inplace.items():
    left = type("Left", (), {
        inplace_name: lambda self, other: choose_notimplemented(),
        left_name: lambda self, other: NotImplemented,
    })()
    right = type("Right", (), {right_name: lambda self, other: 42})()
    results[name] = outcome(lambda left=left, right=right, function=function: run(lambda: function(left, right)))
print(json.dumps(results, sort_keys=True))
"""
    )


def test_operator_truth_and_not_validate_resumed_bool_results() -> None:
    assert_cpython_compatible(
        r"""
import operator
import sys

def outcome(thunk):
    try:
        value = thunk()
        return ["return", type(value).__name__, repr(value)]
    except Exception as error:
        return ["raise", type(error).__name__]

if "aleff" in sys.modules:
    from aleff import create_handler, effect
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def resume(k):
        return k(1)

    def run(thunk):
        return handler(thunk)
else:
    def run(thunk):
        return thunk()

class Target:
    def __bool__(self):
        if "aleff" in sys.modules:
            return choose()
        return 1

print(repr({"truth": outcome(lambda: run(lambda: operator.truth(Target()))),
            "not_": outcome(lambda: run(lambda: operator.not_(Target()))) }))
"""
    )


def test_operator_length_hint_preserves_fallback_and_type_errors() -> None:
    assert_cpython_compatible(
        r"""
import operator
import sys

def outcome(thunk):
    try:
        value = thunk()
        return ["return", type(value).__name__, repr(value)]
    except Exception as error:
        return ["raise", type(error).__name__]

if "aleff" in sys.modules:
    from aleff import create_handler, effect
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def resume(k):
        return k(NotImplemented)

    def resumed_value():
        return choose()

    def run(thunk):
        return handler(thunk)
else:
    def resumed_value():
        return NotImplemented

    def run(thunk):
        return thunk()

class Target:
    def __length_hint__(self):
        return resumed_value()

print(repr(outcome(lambda: run(lambda: operator.length_hint(Target(), 7)))))
"""
    )


def test_operator_search_preserves_effectful_next_and_multishot_state() -> None:
    assert_cpython_compatible(
        r"""
import operator
import sys

target = object()
other = object()
filler = object()

if "aleff" in sys.modules:
    from aleff import create_handler, effect
    choose = effect("choose")

    class Iterator:
        def __init__(self):
            self.index = 0

        def __iter__(self):
            return self

        def __next__(self):
            if self.index == 0:
                self.index += 1
                return choose()
            if self.index == 1:
                self.index += 1
                return filler
            if self.index == 2:
                self.index += 1
                return target
            raise StopIteration

    handler = create_handler(choose)

    @handler.on(choose)
    def resume_next(k):
        return k(other)

    next_result = handler(lambda: operator.indexOf(Iterator(), target))

    class First:
        def __eq__(self, other):
            return choose()

    class Second:
        def __eq__(self, other):
            return True

    multishot_handler = create_handler(choose)

    @multishot_handler.on(choose)
    def resume_comparison(k):
        return [k(False), k(True)]

    multishot_result = multishot_handler(lambda: operator.countOf((First(), Second()), object()))
else:
    next_result = operator.indexOf((other, filler, target), target)
    multishot_result = [1, 2]

print(repr((next_result, multishot_result)))
"""
    )


def test_operator_accessors_preserve_types_and_validation() -> None:
    assert_cpython_compatible(
        r"""
import operator

class Target:
    value = 3

    def method(self):
        return "ok"

def describe(thunk):
    try:
        value = thunk()
        return ["return", type(value).__name__, repr(value)]
    except Exception as error:
        return ["raise", type(error).__name__]

getters = {
    "attr": operator.attrgetter("value"),
    "item": operator.itemgetter(0),
    "method": operator.methodcaller("method"),
}
print(repr({
    "types": {name: type(getter).__name__ for name, getter in getters.items()},
    "values": {
        "attr": describe(lambda: getters["attr"](Target())),
        "item": describe(lambda: getters["item"]((4,))),
        "method": describe(lambda: getters["method"](Target())),
    },
    "constructors": {
        "attr_no_args": describe(lambda: operator.attrgetter()),
        "item_no_args": describe(lambda: operator.itemgetter()),
        "method_no_args": describe(lambda: operator.methodcaller()),
        "method_non_string": describe(lambda: operator.methodcaller(3)),
    },
}))
"""
    )


def test_operator_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import operator

def capture(thunk):
    try:
        value = thunk()
        return ["return", type(value).__name__, repr(value)]
    except Exception as error:
        return ["raise", type(error).__name__]

class BadIndex:
    def __index__(self):
        return "not an int"

class BadLength:
    def __length_hint__(self):
        return -1

print(repr({
    "add_missing": capture(lambda: operator.add(1)),
    "add_bad_types": capture(lambda: operator.add(object(), object())),
    "index_bad_result": capture(lambda: operator.index(BadIndex())),
    "length_negative": capture(lambda: operator.length_hint(BadLength())),
    "index_missing": capture(lambda: operator.indexOf((1, 2), 3)),
    "count_bad_iterable": capture(lambda: operator.countOf(1, 1)),
    "getitem_bad_target": capture(lambda: operator.getitem(1, 0)),
    "setitem_bad_target": capture(lambda: operator.setitem(1, 0, 1)),
    "delitem_bad_target": capture(lambda: operator.delitem(1, 0)),
    "concat_bad_types": capture(lambda: operator.concat(1, 2)),
}))
"""
    )
