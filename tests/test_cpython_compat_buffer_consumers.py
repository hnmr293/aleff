"""CPython differential tests for buffer-consumer continuation adapters."""

from cpython_compat_support import assert_cpython_compatible


def test_buffer_consumer_api_shape_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import binascii
import bz2
import hashlib
import hmac
import inspect
import lzma
import zlib


def describe(value):
    try:
        signature = ("return", str(inspect.signature(value)))
    except BaseException as exc:
        signature = ("raise", type(exc).__name__, str(exc))
    doc = getattr(value, "__doc__", None)
    return (
        type(value).__module__,
        type(value).__name__,
        getattr(value, "__module__", None),
        getattr(value, "__name__", None),
        getattr(value, "__qualname__", None),
        None if doc is None else doc.splitlines()[0],
        signature,
        repr(getattr(value, "__text_signature__", None)),
    )


functions = [hashlib.new]
functions.extend(
    getattr(hashlib, name)
    for name in (
        "md5", "sha1", "sha224", "sha256", "sha384", "sha512",
        "sha3_224", "sha3_256", "sha3_384", "sha3_512", "shake_128",
        "shake_256", "blake2b", "blake2s",
    )
)
functions.extend((hmac.new, hmac.digest))
functions.extend(
    getattr(binascii, name)
    for name in (
        "a2b_base64", "a2b_hex", "a2b_qp", "a2b_uu", "b2a_base64",
        "b2a_hex", "b2a_qp", "b2a_uu", "crc32", "crc_hqx", "hexlify",
        "unhexlify",
    )
)
functions.extend((zlib.adler32, zlib.crc32, zlib.compress, zlib.decompress))
functions.extend((bz2.compress, bz2.decompress, lzma.compress, lzma.decompress))
for value in functions:
    print(describe(value))

methods = (
    hashlib.sha256().update,
    bz2.BZ2Compressor.compress,
    bz2.BZ2Decompressor.decompress,
    lzma.LZMACompressor.compress,
    lzma.LZMADecompressor.decompress,
)
for value in methods:
    print(describe(value))
""".strip()
    )


def test_buffer_consumer_normal_results_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import binascii
import bz2
import hashlib
import hmac
import lzma
import zlib


payload = b"alpha-beta-gamma" * 20
print("hash", hashlib.new("sha256", payload).hexdigest(), hashlib.sha256(payload).hexdigest())
print("hmac", hmac.new(b"key", payload, "sha256").hexdigest(), hmac.digest(b"key", payload, "sha256").hex())
for name, operation in (
    ("hexlify", lambda: binascii.hexlify(payload, b":", 2)),
    ("unhexlify", lambda: binascii.unhexlify(b"616c656666")),
    ("base64", lambda: binascii.a2b_base64(binascii.b2a_base64(payload, newline=False))),
    ("crc", lambda: (binascii.crc32(payload, 123), binascii.crc_hqx(payload, 123))),
    ("zlib", lambda: zlib.decompress(zlib.compress(payload, level=1))),
    ("bz2", lambda: bz2.decompress(bz2.compress(payload, compresslevel=1))),
    ("lzma", lambda: lzma.decompress(lzma.compress(payload, preset=0))),
):
    print(name, operation())
""".strip()
    )


def test_buffer_consumer_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import binascii
import bz2
import hashlib
import hmac
import lzma
import zlib


def outcome(operation):
    try:
        value = operation()
    except BaseException as exc:
        return "raise", type(exc).__name__, str(exc)
    return "return", type(value).__name__, repr(value)


operations = (
    lambda: hashlib.new(),
    lambda: hashlib.sha256(unknown=True),
    lambda: hmac.new(b"key"),
    lambda: hmac.digest(b"key", b"data", object()),
    lambda: binascii.hexlify("text"),
    lambda: binascii.crc_hqx(b"data"),
    lambda: zlib.compress("text"),
    lambda: zlib.decompress(b"invalid"),
    lambda: bz2.compress(b"data", compresslevel=0),
    lambda: bz2.decompress(b"invalid"),
    lambda: lzma.compress(b"data", unknown=True),
    lambda: lzma.decompress(b"invalid"),
)
for operation in operations:
    print(outcome(operation))
""".strip()
    )


def test_buffer_consumer_empty_and_writable_buffers_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import binascii
import bz2
import hashlib
import hmac
import lzma
import zlib


for data in (b"", bytearray(b"aleff"), memoryview(bytearray(b"aleff"))):
    print(
        type(data).__name__,
        hashlib.sha256(data).hexdigest(),
        hmac.digest(bytearray(b"key"), data, "sha256").hex(),
        binascii.hexlify(data),
        zlib.decompress(zlib.compress(data)),
        bz2.decompress(bz2.compress(data)),
        lzma.decompress(lzma.compress(data)),
    )
""".strip()
    )


def test_python_buffer_protocol_calls_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import binascii
import hashlib
import zlib


class Buffer:
    def __init__(self, data):
        self.data = data
        self.events = []

    def __buffer__(self, flags):
        self.events.append(("acquire", flags))
        return memoryview(self.data)

    def __release_buffer__(self, view):
        self.events.append(("release", view.tobytes()))


for operation in (hashlib.sha256, binascii.hexlify, zlib.compress):
    value = Buffer(b"payload")
    result = operation(value)
    print(operation.__module__, operation.__name__, type(result).__name__, value.events)
""".strip()
    )


def test_invalid_python_buffer_result_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import hashlib


class Buffer:
    def __buffer__(self, flags):
        return b"not-a-memoryview"


try:
    hashlib.sha256(Buffer())
except BaseException as exc:
    print(type(exc).__name__, str(exc))
else:
    print("return")
""".strip()
    )


def test_acquired_buffers_are_released_when_later_acquisition_fails() -> None:
    assert_cpython_compatible(
        r"""
import hashlib


events = []


class FirstBuffer:
    def __buffer__(self, flags):
        events.append(("first_acquire", flags))
        return memoryview(b"key")

    def __release_buffer__(self, view):
        events.append(("first_release", view.tobytes()))


class FailingBuffer:
    def __buffer__(self, flags):
        events.append(("second_acquire", flags))
        raise RuntimeError("acquisition failed")


try:
    hashlib.blake2b(b"", key=FirstBuffer(), salt=FailingBuffer())
except BaseException as exc:
    print(type(exc).__name__, str(exc), events)
else:
    print("return", events)
""".strip()
    )


def test_hash_data_and_string_conflict_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import hashlib
import sys


if sys.version_info >= (3, 13):
    events = []

    class Buffer:
        def __init__(self, name):
            self.name = name

        def __buffer__(self, flags):
            events.append((self.name, "acquire", flags))
            return memoryview(self.name.encode())

        def __release_buffer__(self, view):
            events.append((self.name, "release", view.tobytes()))

    operations = (
        lambda: hashlib.new("sha256", data=Buffer("data"), string=Buffer("string")),
        lambda: hashlib.sha256(data=Buffer("data"), string=Buffer("string")),
        lambda: hashlib.blake2b(data=Buffer("data"), string=Buffer("string")),
    )
    for operation in operations:
        events.clear()
        try:
            operation()
        except BaseException as exc:
            print(type(exc).__name__, str(exc), events)
        else:
            print("return", events)
""".strip()
    )
