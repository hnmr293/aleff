"""CPython compatibility regressions for Issue #55."""

from __future__ import annotations

import functools

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def test_reduce_contract_matches_cpython() -> None:
    assert_cpython_compatible(
        """\
import functools

def report(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

report("normal", lambda: functools.reduce(lambda left, right: left + right, [1, 2, 3]))
report("initial_positional", lambda: functools.reduce(lambda left, right: left + right, [1, 2], 10))
report("initial_keyword", lambda: functools.reduce(lambda left, right: left + right, [1], initial=10))
report("non_iterable", lambda: functools.reduce(lambda left, right: left + right, 1))
report("empty_without_initial", lambda: functools.reduce(lambda left, right: left + right, []))
report("invalid_function", lambda: functools.reduce(None, [1, 2]))

events = []
def values():
    events.append("started")
    yield 4
    events.append("finished")

report("single_item_with_initial", lambda: functools.reduce(lambda left, right: left + right, values(), 10))
print("iterator_events", events)
"""
    )


def test_partial_call_contract_matches_cpython() -> None:
    assert_cpython_compatible(
        """\
import functools

def report(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

def combine(first, second, *, suffix=""):
    return (first, second, suffix)

operation = functools.partial(combine, "left", suffix="initial")
print("normal", operation("right"))
print("attributes", operation.func.__name__, operation.args, operation.keywords)

nested = functools.partial(operation, "middle", suffix="nested")
print("nested", nested())
operation.keywords["suffix"] = "mutated"
print("mutable_keywords", operation("right"))

report("non_callable_constructor", lambda: functools.partial(1))
report("missing_argument", lambda: functools.partial(combine, "left")())
report("too_many_arguments", lambda: operation("right", "extra"))
report("unexpected_keyword", lambda: functools.partial(combine, "left", unknown=1)("right"))

placeholder = getattr(functools, "Placeholder", None)
if placeholder is not None:
    report(
        "placeholder_call",
        lambda: functools.partial(combine, placeholder, "fixed", suffix="p")("filled"),
    )
"""
    )


def test_cmp_to_key_contract_matches_cpython() -> None:
    assert_cpython_compatible(
        """\
import functools
import gc

def report(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

def compare(left, right):
    return (left > right) - (left < right)

factory = functools.cmp_to_key(compare)
wrapped = factory(4)
print(
    "factory_shape",
    type(factory).__name__,
    callable(factory),
    hasattr(factory, "__name__"),
    gc.is_tracked(factory),
)
print("wrapped", type(wrapped).__name__, wrapped.obj)
print("sorted", [item.obj for item in sorted(map(factory, [3, 1, 2]))])
report("factory_without_object", lambda: factory())
report("factory_with_extra_object", lambda: factory(1, 2))
report("factory_keyword_object", lambda: factory(obj=9).obj)

try:
    non_callable_factory = functools.cmp_to_key(1)
except BaseException as exc:
    print("non_callable_factory", "raise", type(exc).__name__, str(exc))
else:
    print("non_callable_factory", "return", type(non_callable_factory).__name__)
    report(
        "non_callable_comparison",
        lambda: non_callable_factory(1) < non_callable_factory(2),
    )

class Other:
    def __gt__(self, value):
        return "reflected"

report("mixed_type_comparison", lambda: factory(1) < Other())

def failing_compare(left, right):
    raise ValueError("comparison failed")

failing_factory = functools.cmp_to_key(failing_compare)
report("comparator_error", lambda: failing_factory(1) < failing_factory(2))
"""
    )


def test_cmp_to_key_uses_public_native_objects() -> None:
    assert_cpython_compatible(
        """\
import copy
import functools
import _functools
import pickle

factory = functools.cmp_to_key(lambda left, right: (left > right) - (left < right))
wrapped = factory(1)
native_factory = _functools.cmp_to_key(lambda left, right: (left > right) - (left < right))
native_wrapped = native_factory(1)
print("public_identity", functools.cmp_to_key is _functools.cmp_to_key)
print("factory", type(factory).__module__, type(factory).__name__)
print("wrapped", type(wrapped).__module__, type(wrapped).__name__)
print("native_types", type(factory) is type(native_factory), type(wrapped) is type(native_wrapped))

def shape(value):
    return (
        type(value).__module__,
        type(value).__name__,
        repr(value).split(" object at ", 1)[0],
        getattr(value, "obj", "missing"),
    )

print("wrapped_shape", shape(wrapped))
for label, operation in (
    ("copy", lambda: copy.copy(wrapped)),
    ("deepcopy", lambda: copy.deepcopy(wrapped)),
    ("pickle", lambda: pickle.loads(pickle.dumps(wrapped, protocol=4))),
):
    try:
        copied = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", shape(copied))

try:
    wrapped.obj = 2
except BaseException as exc:
    print("obj_assignment", type(exc).__name__, str(exc))
else:
    print("obj_assignment", "succeeded")
"""
    )


def test_functools_metadata_matches_cpython() -> None:
    assert_cpython_compatible(
        """\
import functools
import inspect

def signature_shape(function):
    try:
        signature = inspect.signature(function)
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))
    return tuple(
        (parameter.name, parameter.kind.name, parameter.default is inspect.Parameter.empty)
        for parameter in signature.parameters.values()
    )

for name in ("reduce", "cmp_to_key", "lru_cache"):
    function = getattr(functools, name)
    doc = function.__doc__
    first_line = None if doc is None else doc.splitlines()[0]
    module = function.__module__
    module_shape = (type(module).__name__, getattr(module, "__name__", module))
    print(
        name,
        function.__name__,
        module_shape,
        doc is not None,
        first_line,
        repr(doc),
        getattr(function, "__text_signature__", None),
        signature_shape(function),
    )
"""
    )


def test_lru_cache_call_forms_and_errors_match_cpython() -> None:
    assert_cpython_compatible(
        """\
import functools

def report(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

def implementation(value):
    return value * 10

cached = functools.lru_cache(maxsize=2, typed=True)(implementation)
print("normal", cached(2), cached(2), cached(3))
info = cached.cache_info()
print("cache_info", info.hits, info.misses, info.maxsize, info.currsize)
print("parameters", sorted(cached.cache_parameters().items()))
print("wrapper_attributes", cached.__name__, cached.__doc__, cached.__wrapped__ is implementation)
cached.cache_clear()
info = cached.cache_info()
print("cleared", info.hits, info.misses, info.maxsize, info.currsize)

report("direct_positional_typed", lambda: functools.lru_cache(implementation, True)(2))
report("direct_keyword_typed", lambda: functools.lru_cache(implementation, typed=True)(2))
report("user_function_keyword", lambda: functools.lru_cache(2)(user_function=implementation)(2))

decorator = functools.lru_cache(2)
print("decorator_type", type(decorator).__name__)
report("decorator_attribute", lambda: setattr(decorator, "custom_attribute", 7))
report("decorator_attribute_read", lambda: decorator.custom_attribute)

report("invalid_maxsize", lambda: functools.lru_cache(maxsize="invalid"))
report("invalid_decorated_function", lambda: functools.lru_cache(2)(None))
report("unexpected_keyword", lambda: functools.lru_cache(maxsize=2, unsupported=True))
"""
    )


def test_lru_cache_reentrant_same_key_resume_preserves_existing_value() -> None:
    choose = effect("issue55_lru_same_key")
    handler = create_handler(choose)

    @handler.on(choose)
    def resume_all(k):
        outcomes = []
        for value in (1, 10):
            outcomes.append(k(value))
        return outcomes

    state = {"outer": True}

    @functools.lru_cache(maxsize=1)
    def cached(key):
        if state.pop("outer", False):
            cached(key)
            return choose()
        return 99

    assert handler(lambda: (cached("key"), cached("key"))) == [(1, 99), (10, 99)]
