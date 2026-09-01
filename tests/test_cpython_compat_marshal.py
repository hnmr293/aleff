"""CPython differential tests for marshal serialization APIs."""

from __future__ import annotations

from cpython_compat_support import assert_cpython_compatible


def test_marshal_api_shape_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import inspect
import marshal


def signature(value):
    try:
        return ("return", str(inspect.signature(value)))
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


for name in ("dump", "dumps", "load", "loads"):
    value = getattr(marshal, name)
    print(
        name,
        type(value).__module__,
        type(value).__name__,
        value.__module__,
        value.__name__,
        signature(value),
        repr(getattr(value, "__text_signature__", None)),
    )
print("version", marshal.version)
""".strip()
    )


def test_marshal_dump_write_protocol_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import marshal


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class Writer:
    def __init__(self, result=None, error=None):
        self.result = result
        self.error = error
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        if self.error is not None:
            raise self.error
        return self.result


value = (1, "two", [3], {"four": 4})
expected = marshal.dumps(value)
for result in (None, 0, -1, "sentinel", object()):
    writer = Writer(result=result)
    actual = marshal.dump(value, writer)
    print("result", type(result).__name__, actual is result, len(writer.writes), writer.writes == [expected])

writer = Writer(error=RuntimeError("write failed"))
print("error", outcome(lambda: marshal.dump(value, writer)), len(writer.writes), writer.writes == [expected])

class Missing:
    pass

print("missing", outcome(lambda: marshal.dump(value, Missing())))
print("noncallable", outcome(lambda: marshal.dump(value, type("Bad", (), {"write": 42})())))
""".strip()
    )


def test_marshal_values_errors_and_version_conversion_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import marshal
import sys


def outcome(call):
    try:
        value = call()
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))
    if isinstance(value, bytes):
        return ("return", value.hex())
    return ("return", value)


class Version:
    def __init__(self, value):
        self.value = value

    def __index__(self):
        return self.value


for version in (0, 3, 4, marshal.version, Version(4), Version(10), Version(2**100), Version("4")):
    print("version", type(version).__name__, repr(getattr(version, "value", version)), outcome(lambda version=version: marshal.dumps((1, 2), version)))

for value in (None, True, 1, 1.5, 1+2j, "text", b"bytes", [1], (1,), {"a": 1}, {1, 2}, frozenset({1, 2}), slice(1, 2, 3)):
    print("value", type(value).__name__, outcome(lambda value=value: marshal.loads(marshal.dumps(value))))

print("unsupported", outcome(lambda: marshal.dumps(object())))
print("missing", outcome(lambda: marshal.dumps()))
print("too_many", outcome(lambda: marshal.loads(b"N", 1)))
print("keyword_version", outcome(lambda: marshal.dumps(None, version=4)))
print("runtime", sys.version_info[:2])
""".strip()
    )


def test_marshal_allow_code_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import marshal
import sys


if sys.version_info >= (3, 13):
    def outcome(call):
        try:
            value = call()
        except BaseException as exc:
            return ("raise", type(exc).__name__, str(exc))
        return ("return", type(value).__name__)

    code = compile("value = 1", "<marshal-test>", "exec")
    encoded = marshal.dumps(code)

    class Allow:
        def __init__(self, value):
            self.value = value

        def __bool__(self):
            print("bool", self.value)
            return self.value

    for allowed in (True, False):
        print("dumps", allowed, outcome(lambda allowed=allowed: marshal.dumps(code, allow_code=Allow(allowed))))
        print("loads", allowed, outcome(lambda allowed=allowed: marshal.loads(encoded, allow_code=Allow(allowed))))
else:
    print("allow_code unavailable")
""".strip()
    )


def test_marshal_load_file_read_protocol_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import marshal


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class Reader:
    def __init__(self, data):
        self.data = data
        self.position = 0
        self.calls = []

    def read(self, size):
        self.calls.append(("read", size))
        if size != 0:
            raise AssertionError("read() received a non-zero size")
        return b""

    def readinto(self, buffer):
        self.calls.append(("readinto", len(buffer)))
        amount = min(len(buffer), len(self.data) - self.position)
        buffer[:amount] = self.data[self.position:self.position + amount]
        self.position += amount
        return amount


for value in (None, 1, (1, "two"), [1, 2, 3], "x" * 100):
    data = marshal.dumps(value)
    reader = Reader(data)
    result = outcome(lambda: marshal.load(reader))
    print(
        type(value).__name__,
        result,
        "calls",
        reader.calls,
        "position",
        reader.position,
        "length",
        len(data),
    )


class ReadZero:
    def __init__(self, result):
        self.result = result
        self.calls = []

    def read(self, size):
        self.calls.append(("read", size))
        return self.result

    def readinto(self, buffer):
        self.calls.append(("readinto", len(buffer)))
        buffer[0] = marshal.dumps(None)[0]
        return 1


for result in (b"", b"ignored", None, "not bytes"):
    reader = ReadZero(result)
    print("read_zero", type(result).__name__, outcome(lambda: marshal.load(reader)), reader.calls)
""".strip()
    )


def test_marshal_load_file_readinto_return_contract_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import marshal


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


data = marshal.dumps(1)


class Reader:
    def __init__(self, returns):
        self.returns = list(returns)
        self.position = 0
        self.calls = []

    def read(self, size):
        self.calls.append(("read", size))
        return b""

    def readinto(self, buffer):
        self.calls.append(("readinto", len(buffer)))
        returned = self.returns.pop(0)
        if isinstance(returned, int) and 0 <= returned <= len(buffer):
            amount = min(returned, len(data) - self.position)
        else:
            amount = min(len(buffer), len(data) - self.position)
        buffer[:amount] = data[self.position:self.position + amount]
        self.position += amount
        return returned


cases = (
    ("short", (1, 3)),
    ("long", (2,)),
    ("none", (None,)),
    ("float", (1.5,)),
    ("text", ("one",)),
    ("negative", (-1,)),
)
for label, returns in cases:
    reader = Reader(returns)
    print(label, outcome(lambda: marshal.load(reader)), reader.calls, reader.position)
""".strip()
    )


def test_marshal_load_file_callback_exceptions_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import marshal


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class ReadError:
    def __init__(self):
        self.calls = []

    def read(self, size):
        self.calls.append(("read", size))
        raise LookupError("read failed")


class ReadIntoError:
    def __init__(self):
        self.calls = []

    def read(self, size):
        self.calls.append(("read", size))
        return b""

    def readinto(self, buffer):
        self.calls.append(("readinto", len(buffer)))
        raise RuntimeError("readinto failed")


class MissingReadInto:
    def __init__(self):
        self.calls = []

    def read(self, size):
        self.calls.append(("read", size))
        return b""


for reader in (ReadError(), ReadIntoError(), MissingReadInto()):
    print(type(reader).__name__, outcome(lambda reader=reader: marshal.load(reader)), reader.calls)
""".strip()
    )


def test_marshal_load_does_not_overread_concatenated_objects() -> None:
    assert_cpython_compatible(
        r"""
import marshal


class Reader:
    def __init__(self, data):
        self.data = data
        self.position = 0
        self.calls = []

    def read(self, size):
        self.calls.append(("read", size))
        return b""

    def readinto(self, buffer):
        self.calls.append(("readinto", len(buffer)))
        amount = min(len(buffer), len(self.data) - self.position)
        buffer[:amount] = self.data[self.position:self.position + amount]
        self.position += amount
        return amount


first_data = marshal.dumps(("first", [1, 2]))
second_data = marshal.dumps({"second": 3})
tail = b"unread tail"
reader = Reader(first_data + second_data + tail)

first = marshal.load(reader)
first_position = reader.position
first_calls = len(reader.calls)
second = marshal.load(reader)
second_position = reader.position
print("first", first, first_position, first_position == len(first_data))
print("second", second, second_position, second_position == len(first_data) + len(second_data))
print("tail", reader.data[reader.position:], "calls", reader.calls[first_calls:])
""".strip()
    )


def test_marshal_load_file_allow_code_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import marshal
import sys


def outcome(call):
    try:
        value = call()
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))
    return ("return", type(value).__name__, getattr(value, "co_filename", None))


if sys.version_info >= (3, 13):
    class Reader:
        def __init__(self, data):
            self.data = data
            self.position = 0
            self.calls = []

        def read(self, size):
            self.calls.append(("read", size))
            return b""

        def readinto(self, buffer):
            self.calls.append(("readinto", len(buffer)))
            amount = min(len(buffer), len(self.data) - self.position)
            buffer[:amount] = self.data[self.position:self.position + amount]
            self.position += amount
            return amount

    class Allow:
        def __init__(self, value):
            self.value = value

        def __bool__(self):
            print("bool", self.value)
            return self.value

    code = compile("value = 1", "<marshal-load-test>", "exec")
    data = marshal.dumps(code)
    for allowed in (True, False, Allow(True), Allow(False)):
        reader = Reader(data)
        print(
            "allow",
            type(allowed).__name__,
            getattr(allowed, "value", allowed),
            outcome(lambda allowed=allowed: marshal.load(reader, allow_code=allowed)),
            "position",
            reader.position,
            "calls",
            reader.calls,
        )
else:
    print("allow_code unavailable")
""".strip()
    )


def test_marshal_load_slice_version_difference_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import marshal
import sys


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


if sys.version_info >= (3, 14):
    class Reader:
        def __init__(self, data):
            self.data = data
            self.position = 0
            self.calls = []

        def read(self, size):
            self.calls.append(("read", size))
            return b""

        def readinto(self, buffer):
            self.calls.append(("readinto", len(buffer)))
            amount = min(len(buffer), len(self.data) - self.position)
            buffer[:amount] = self.data[self.position:self.position + amount]
            self.position += amount
            return amount

    data = marshal.dumps(slice(1, 2, 3))
    reader = Reader(data)
    print("slice", outcome(lambda: marshal.load(reader)), reader.calls, reader.position, len(data))
else:
    print("slice", outcome(lambda: marshal.dumps(slice(1, 2, 3))))
""".strip()
    )
