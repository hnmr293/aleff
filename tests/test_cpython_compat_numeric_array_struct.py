"""CPython differential coverage for numeric, array, and struct accelerators."""

from cpython_compat_support import (
    assert_cpython_compatible,
    assert_cpython_compatible_after_prelude,
)


def test_array_audit_hook_installed_before_aleff_matches_cpython() -> None:
    assert_cpython_compatible_after_prelude(
        r"""
import sys

events = []


def audit(event, args):
    if event == "array.__new__":
        events.append((event, args[0], type(args[1]).__name__))


sys.addaudithook(audit)
""".strip(),
        r"""
import array


class Source:
    def __iter__(self):
        return iter((1, 2))


print("after_import", events)
print("result", array.array("i", Source()).tolist())
print("events", events)
""".strip(),
    )


def test_math_functions_and_version_gated_functions_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import math
import sys


def report(name, call):
    try:
        value = call()
    except BaseException as exc:
        print(name, "raise", type(exc).__name__, str(exc))
    else:
        print(name, "return", type(value).__name__, repr(value))


print("runtime", sys.version_info[:2])
calls = (
    ("acos", (0.25,)),
    ("acosh", (2.0,)),
    ("asin", (0.25,)),
    ("asinh", (0.25,)),
    ("atan", (0.25,)),
    ("atan2", (0.25, 1.0)),
    ("atanh", (0.25,)),
    ("cbrt", (2.0,)),
    ("ceil", (1.25,)),
    ("comb", (5, 2)),
    ("copysign", (1.25, -1.0)),
    ("cos", (0.25,)),
    ("cosh", (0.25,)),
    ("degrees", (0.25,)),
    ("dist", ((3.0, 4.0), (0.0, 0.0))),
    ("erf", (0.25,)),
    ("erfc", (0.25,)),
    ("exp", (0.25,)),
    ("exp2", (0.25,)),
    ("expm1", (0.25,)),
    ("fabs", (-1.25,)),
    ("factorial", (5,)),
    ("floor", (1.25,)),
    ("fma", (1.25, 2.0, 3.0)),
    ("fmod", (5.0, 2.0)),
    ("frexp", (5.0,)),
    ("fsum", ((1.25, 2.5, 4.0),)),
    ("gamma", (2.5,)),
    ("gcd", (12, 8)),
    ("hypot", (3.0, 4.0)),
    ("isclose", (1.0, 1.0)),
    ("isfinite", (1.0,)),
    ("isinf", (float("inf"),)),
    ("isnan", (float("nan"),)),
    ("isqrt", (17,)),
    ("lcm", (12, 8)),
    ("ldexp", (1.25, 2)),
    ("lgamma", (2.5,)),
    ("log", (2.5,)),
    ("log10", (2.5,)),
    ("log1p", (0.25,)),
    ("log2", (2.5,)),
    ("modf", (1.25,)),
    ("nextafter", (1.0, 2.0)),
    ("perm", (5, 2)),
    ("pow", (2.0, 3.0)),
    ("prod", ((2, 3, 4),)),
    ("radians", (0.25,)),
    ("remainder", (5.0, 2.0)),
    ("sin", (0.25,)),
    ("sinh", (0.25,)),
    ("sqrt", (2.5,)),
    ("sumprod", ((1, 2), (3, 4))),
    ("tan", (0.25,)),
    ("tanh", (0.25,)),
    ("trunc", (1.25,)),
    ("ulp", (1.25,)),
)
for name, args in calls:
    operation = getattr(math, name, None)
    if operation is not None:
        report(name, lambda operation=operation, args=args: operation(*args))

for name, args in (
    ("acos", ("bad",)),
    ("comb", (5.5, 2)),
    ("sqrt", (-1.0,)),
    ("gcd", ("bad", 2)),
    ("dist", ((1.0, "bad"), (0.0, 0.0))),
    ("fsum", ((1.0, "bad"),)),
    ("isclose", (1.0, "bad")),
    ("nextafter", (1.0, 2.0, -1)),
):
    operation = getattr(math, name, None)
    if operation is not None:
        report(name + "_error", lambda operation=operation, args=args: operation(*args))

if hasattr(math, "sumprod"):
    report("sumprod_long_product_overflow", lambda: math.sumprod((sys.maxsize,), (2,)))
    report("sumprod_long_sum_overflow", lambda: math.sumprod((sys.maxsize, 1), (1, 1)))

events = []


class FloatValue:
    def __getattribute__(self, name):
        events.append(("getattribute", name))
        return object.__getattribute__(self, name)

    def __float__(self):
        events.append("float")
        return 0.25


report("sin_float_protocol", lambda: math.sin(FloatValue()))
print("sin_float_protocol_events", events)


class RoundingValue:
    def __ceil__(self):
        return "ceil-result"

    def __floor__(self):
        return "floor-result"

    def __trunc__(self):
        return "trunc-result"


for name in ("ceil", "floor", "trunc"):
    report(name + "_special_result", lambda name=name: getattr(math, name)(RoundingValue()))
""".strip()
    )


def test_cmath_functions_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import cmath


def report(name, call):
    try:
        value = call()
    except BaseException as exc:
        print(name, "raise", type(exc).__name__, str(exc))
    else:
        print(name, "return", type(value).__name__, repr(value))


calls = (
    ("acos", (0.25 + 0.5j,)),
    ("acosh", (0.25 + 0.5j,)),
    ("asin", (0.25 + 0.5j,)),
    ("asinh", (0.25 + 0.5j,)),
    ("atan", (0.25 + 0.5j,)),
    ("atanh", (0.25 + 0.5j,)),
    ("cos", (0.25 + 0.5j,)),
    ("cosh", (0.25 + 0.5j,)),
    ("exp", (0.25 + 0.5j,)),
    ("isclose", (0.25 + 0.5j, 0.5 + 0.25j)),
    ("isfinite", (0.25 + 0.5j,)),
    ("isinf", (complex(float("inf"), 0.0),)),
    ("isnan", (complex(float("nan"), 0.0),)),
    ("log", (0.25 + 0.5j,)),
    ("log10", (0.25 + 0.5j,)),
    ("phase", (0.25 + 0.5j,)),
    ("polar", (0.25 + 0.5j,)),
    ("rect", (1.25, 0.5)),
    ("sin", (0.25 + 0.5j,)),
    ("sinh", (0.25 + 0.5j,)),
    ("sqrt", (0.25 + 0.5j,)),
    ("tan", (0.25 + 0.5j,)),
    ("tanh", (0.25 + 0.5j,)),
)
for name, args in calls:
    operation = getattr(cmath, name, None)
    if operation is not None:
        report(name, lambda operation=operation, args=args: operation(*args))

for name, args in (
    ("acos", ("bad",)),
    ("isclose", (1.0, "bad")),
    ("rect", ("bad", 0.5)),
):
    operation = getattr(cmath, name, None)
    if operation is not None:
        report(name + "_error", lambda operation=operation, args=args: operation(*args))

for protocol in ("complex", "float", "index"):
    events = []

    def value_method(self, protocol=protocol):
        del self
        events.append(protocol)
        return {"complex": 0.25 + 0.5j, "float": 0.25, "index": 1}[protocol]

    namespace = {f"__{protocol}__": value_method}
    value_type = type(f"{protocol.title()}Value", (), namespace)
    report(f"sin_{protocol}_protocol", lambda value_type=value_type: cmath.sin(value_type()))
    print(f"sin_{protocol}_protocol_events", events)
""".strip()
    )


def test_math_iterator_state_and_partial_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import math


class Values:
    def __init__(self, values, fail_at=None):
        self.values = tuple(values)
        self.fail_at = fail_at
        self.index = 0
        self.events = []

    def __iter__(self):
        self.events.append("iter")
        return self

    def __next__(self):
        self.events.append(("next", self.index))
        if self.fail_at == self.index:
            raise RuntimeError("iterator failed")
        if self.index == len(self.values):
            raise StopIteration
        value = self.values[self.index]
        self.index += 1
        return value


def report(name, call, source):
    try:
        value = call()
    except BaseException as exc:
        value = ("raise", type(exc).__name__, str(exc))
    else:
        value = ("return", value)
    print(name, value, source.events)


for name in ("fsum", "hypot", "prod", "gcd", "lcm"):
    operation = getattr(math, name)
    source = Values((1, 2, 3))
    if name == "hypot":
        report(name, lambda: operation(*source), source)
    elif name in ("gcd", "lcm"):
        report(name, lambda: operation(*source), source)
    else:
        report(name, lambda: operation(source), source)

source = Values((3.0, 4.0))
report("dist", lambda: math.dist(source, (0.0, 0.0)), source)

if hasattr(math, "sumprod"):
    source = Values((1, 2))
    report("sumprod", lambda: math.sumprod(source, (3, 4)), source)

for name in ("fsum", "hypot", "prod", "gcd", "lcm"):
    operation = getattr(math, name)
    source = Values((1, 2), fail_at=1)
    if name == "hypot" or name in ("gcd", "lcm"):
        report(name + "_error", lambda: operation(*source), source)
    else:
        report(name + "_error", lambda: operation(source), source)
source = Values((3.0, 4.0), fail_at=1)
report("dist_error", lambda: math.dist(source, (0.0, 0.0)), source)
if hasattr(math, "sumprod"):
    source = Values((1, 2), fail_at=1)
    report("sumprod_error", lambda: math.sumprod(source, (3, 4)), source)
""".strip()
    )


def test_array_constructor_methods_and_file_io_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
from array import array
import io


def report(name, call):
    try:
        value = call()
    except BaseException as exc:
        print(name, "raise", type(exc).__name__, str(exc))
    else:
        print(name, "return", type(value).__name__, repr(value))


constructed = array("i", (1, 2, 3))
print("constructor", constructed.typecode, constructed.itemsize, constructed.tolist())
empty = array("i")
print("empty", empty.typecode, empty.tolist())

value = array("i", [1, 2, 1])
print("append", value.append(3), value.tolist())
print("insert", value.insert(1, 9), value.tolist())
print("extend", value.extend((7, 8)), value.tolist())
print("count", value.count(1))
print("index", value.index(9))
print("remove", value.remove(9), value.tolist())

payload = array("i", [11, 13])
stream = io.BytesIO(payload.tobytes())
loaded = array("i")
print("fromfile", loaded.fromfile(stream, 2), loaded.tolist(), stream.tell())
output = io.BytesIO()
print("tofile", payload.tofile(output), output.getvalue() == payload.tobytes())

report("bad_typecode", lambda: array("z"))
report("bad_constructor_item", lambda: array("i", [object()]))
report("bad_append", lambda: array("i", [1]).append(object()))
report("bad_insert", lambda: array("i", [1]).insert("0", 2))
report("missing_index", lambda: array("i", [1]).index(2))
report("missing_remove", lambda: array("i", [1]).remove(2))
report("short_fromfile", lambda: array("i").fromfile(io.BytesIO(b"x"), 1))
""".strip()
    )


def test_struct_module_and_struct_type_apis_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import struct


def report(name, call):
    try:
        value = call()
    except BaseException as exc:
        print(name, "raise", type(exc).__name__, str(exc))
    else:
        print(name, "return", type(value).__name__, repr(value))


payload = struct.pack("ii", 11, 13)
buffer = bytearray(8)
print("pack", struct.pack("ii", 11, 13))
print("repeat_pack", struct.pack("2i", 11, 13))
print("zero_repeat", struct.pack("0i"))
print("string_repeat", struct.pack("4s", b"a"))
print("pack_into", struct.pack_into("ii", buffer, 0, 11, 13), bytes(buffer))
print("unpack", struct.unpack("ii", payload))
print("unpack_from", struct.unpack_from("ii", b"x" + payload, 1))
print("iter_unpack", list(struct.iter_unpack("ii", payload * 2)))

descriptor = struct.Struct("ii")
buffer = bytearray(8)
print("Struct", descriptor.format, descriptor.size)
print("Struct.pack", descriptor.pack(11, 13))
print("Struct.pack_into", descriptor.pack_into(buffer, 0, 11, 13), bytes(buffer))
print("Struct.unpack", descriptor.unpack(payload))
print("Struct.unpack_from", descriptor.unpack_from(b"x" + payload, 1))
print("Struct.iter_unpack", list(descriptor.iter_unpack(payload * 2)))

report("bad_format", lambda: struct.pack("z", 1))

protocol_events = []


class IndexValue:
    def __index__(self):
        protocol_events.append("index")
        return 1


report("bad_prefix", lambda: struct.pack("i<i", IndexValue(), IndexValue()))
print("bad_prefix_protocol_events", protocol_events)
report("bad_value", lambda: struct.pack("i", "bad"))
report("short_unpack", lambda: struct.unpack("ii", b"short"))
report("bad_offset", lambda: struct.unpack_from("i", b"x", 2))
report("short_iter_unpack", lambda: list(struct.iter_unpack("ii", b"short")))
report("Struct_short_unpack", lambda: descriptor.unpack(b"short"))
report("Struct_bad_offset", lambda: descriptor.unpack_from(payload, 5))
report("Struct_short_iter_unpack", lambda: list(descriptor.iter_unpack(b"short")))
""".strip()
    )
