"""Differential regression tests for attribute-related built-ins."""

from __future__ import annotations

import subprocess
import sys

import pytest

from cpython_compat_support import assert_cpython_compatible


def test_delattr_normal_errors_and_slots_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def outcome(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

class Plain:
    present = "class-value"

target = Plain()
target.instance = "instance-value"
outcome("delete-instance", lambda: delattr(target, "instance"))
print("instance-after", hasattr(target, "instance"))
outcome("delete-class", lambda: delattr(Plain, "present"))
outcome("delete-missing", lambda: delattr(target, "missing"))
outcome("non-string-name", lambda: delattr(Plain(), 1))
outcome("non-object", lambda: delattr(1, "value"))
outcome("keyword", lambda: delattr(object=target, name="value"))

class Slotted:
    __slots__ = ("value",)

slotted = Slotted()
slotted.value = 3
outcome("delete-slot", lambda: delattr(slotted, "value"))

class ReadOnly:
    @property
    def value(self):
        return 1

outcome("delete-read-only", lambda: delattr(ReadOnly(), "value"))
"""
    )


def test_dir_normal_sorting_errors_and_iterables_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def outcome(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

class Custom:
    def __dir__(self):
        return ["zeta", "alpha", "middle"]

outcome("custom", lambda: dir(Custom()))
outcome("ordinary", lambda: [name for name in dir(3) if name in ("real", "imag")])

class TupleNames:
    def __dir__(self):
        return ("b", "a")

outcome("tuple-result", lambda: dir(TupleNames()))

class BadResult:
    def __dir__(self):
        return 3

outcome("bad-result", lambda: dir(BadResult()))

class BadName:
    def __dir__(self):
        return ["ok", 1]

outcome("bad-name", lambda: dir(BadName()))
outcome("too-many", lambda: dir(1, 2))
outcome("keyword", lambda: dir(object=1))
"""
    )


def test_getattr_normal_defaults_descriptors_and_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def outcome(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

class Descriptor:
    value = 42

    @property
    def property_value(self):
        return "property"

target = Descriptor()
outcome("ordinary", lambda: getattr(target, "value"))
outcome("property", lambda: getattr(target, "property_value"))
outcome("default", lambda: getattr(target, "missing", "fallback"))
outcome("missing", lambda: getattr(target, "missing"))
outcome("bad-name", lambda: getattr(target, 1))
outcome("bad-object", lambda: getattr(1, "missing"))
outcome("keyword", lambda: getattr(object=target, name="value"))

class Raising:
    def __getattribute__(self, name):
        if name == "missing":
            raise RuntimeError("lookup failed")
        return object.__getattribute__(self, name)

outcome("non-attribute-error", lambda: getattr(Raising(), "missing", "fallback"))
"""
    )


def test_hasattr_boolean_attribute_error_and_other_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def outcome(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

class Target:
    present = None

    def __getattribute__(self, name):
        if name == "missing":
            raise AttributeError(name)
        if name == "broken":
            raise RuntimeError("lookup failed")
        return object.__getattribute__(self, name)

target = Target()
outcome("present", lambda: hasattr(target, "present"))
outcome("missing", lambda: hasattr(target, "missing"))
outcome("broken", lambda: hasattr(target, "broken"))
outcome("bad-name", lambda: hasattr(target, 1))
outcome("bad-object", lambda: hasattr(1, "missing"))
outcome("keyword", lambda: hasattr(object=target, name="present"))
"""
    )


def test_hash_normalization_unhashable_and_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def outcome(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

outcome("integer", lambda: hash(123456789))
outcome("negative-zero", lambda: hash(-0.0))
outcome("unhashable", lambda: hash([]))
outcome("keyword", lambda: hash(object=1))

class MinusOne:
    def __hash__(self):
        return -1

outcome("normalize-minus-one", lambda: hash(MinusOne()))

class BadHash:
    __hash__ = None

outcome("explicitly-unhashable", lambda: hash(BadHash()))
"""
    )


@pytest.mark.parametrize("operation", ["isinstance", "issubclass"])
def test_abstract_checks_normal_tuples_unions_and_errors_match_cpython(
    operation: str,
) -> None:
    assert_cpython_compatible(
        f"""
def outcome(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

operation = {operation}
candidate = 3 if operation is isinstance else int
outcome("ordinary", lambda: operation(candidate, int))
outcome("tuple", lambda: operation(candidate, (str, int)))
outcome("nested-tuple", lambda: operation(candidate, (str, (bytes, int))))
outcome("short-circuit", lambda: operation(candidate, (int, (1,))))
outcome("union", lambda: operation(candidate, int | str))
outcome("invalid", lambda: operation(candidate, (str, 1)))
outcome("bad-classinfo", lambda: operation(candidate, 1))
outcome("bad-candidate", lambda: operation(1, int))
outcome("keyword", lambda: operation(object=candidate, class_or_tuple=int))

class Meta(type):
    def __instancecheck__(cls, value):
        return 2

    def __subclasscheck__(cls, value):
        return 0

class Checked(metaclass=Meta):
    pass

outcome("hook", lambda: operation(candidate, Checked))

deep = int
for _ in range(1000):
    deep = (deep,)
outcome("deep-tuple", lambda: operation(candidate, deep))
"""
    )


def test_setattr_normal_descriptors_slots_and_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def outcome(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

class Plain:
    pass

target = Plain()
outcome("instance", lambda: setattr(target, "value", 7))
print("instance-value", target.value)
outcome("replace", lambda: setattr(target, "value", 8))
print("replaced-value", target.value)
outcome("bad-name", lambda: setattr(target, 1, 2))
outcome("bad-object", lambda: setattr(1, "value", 2))
outcome("keyword", lambda: setattr(object=target, name="value", value=9))

class Slotted:
    __slots__ = ("value",)

slotted = Slotted()
outcome("slot", lambda: setattr(slotted, "value", 4))
outcome("unknown-slot", lambda: setattr(slotted, "other", 5))

class ReadOnly:
    @property
    def value(self):
        return 1

outcome("read-only", lambda: setattr(ReadOnly(), "value", 2))
"""
    )


def test_vars_objects_mappings_and_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
def outcome(label, operation):
    try:
        value = operation()
    except BaseException as exc:
        print(label, "raise", type(exc).__name__, str(exc))
    else:
        print(label, "return", type(value).__name__, repr(value))

class Plain:
    class_value = "class"

target = Plain()
target.instance_value = "instance"
mapping = vars(target)
print("object-dict", type(mapping).__name__, sorted(mapping.items()))
print("same-object", mapping is vars(target))
print("class-value", vars(Plain)["class_value"])
outcome("integer", lambda: vars(1))

class Slotted:
    __slots__ = ()

outcome("slots", lambda: vars(Slotted()))
outcome("keyword", lambda: vars(object=target))

class CustomDict:
    def __getattribute__(self, name):
        if name == "__dict__":
            return {"provided": 3}
        return object.__getattribute__(self, name)

outcome("custom-dict", lambda: vars(CustomDict()))
"""
    )


def test_replaced_builtin_metadata_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import builtins
import inspect

for name in (
    "delattr", "dir", "getattr", "hasattr", "hash",
    "isinstance", "issubclass", "setattr",
):
    function = getattr(builtins, name)
    print(
        name,
        type(function).__name__,
        getattr(function, "__self__", None) is builtins,
        getattr(function, "__module__", None),
        repr(getattr(function, "__text_signature__", None)),
    )
    try:
        print("signature", str(inspect.signature(function)))
    except Exception as exc:
        print("signature", type(exc).__name__)
"""
    )


def test_abstract_check_nested_protocol_error_does_not_crash() -> None:
    source = r"""
from aleff import create_handler, effect

choose = effect("choose")

class Directory:
    def __dir__(self):
        return choose()

class Meta(type):
    def __instancecheck__(cls, value):
        return dir(Directory())[0]

    def __subclasscheck__(cls, value):
        return dir(Directory())[0]

class Checked(metaclass=Meta):
    pass

def run(operation):
    handler = create_handler(choose)

    def handle(k):
        results = []
        for value in (1, ["name"]):
            try:
                results.append(("return", k(value)))
            except BaseException as exc:
                results.append(("raise", type(exc).__name__))
        return results

    @handler.on(choose)
    def choose_value(k):
        return handle(k)

    return handler(
        lambda: operation(object, Checked)
        if operation is issubclass
        else operation(object(), Checked)
    )

print(run(isinstance))
print(run(issubclass))
"""
    result = subprocess.run(
        [sys.executable, "-X", "faulthandler", "-c", source],
        text=True,
        capture_output=True,
        timeout=10,
    )
    assert result.returncode == 0, (
        f"subprocess exited with {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    assert result.stdout.splitlines() == [
        "[('raise', 'TypeError'), ('return', True)]",
        "[('raise', 'TypeError'), ('return', True)]",
    ]
