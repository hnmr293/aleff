"""CPython differential tests for regular-expression substitution APIs."""

from __future__ import annotations

from cpython_compat_support import assert_cpython_compatible


def test_re_substitution_api_shape_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import inspect
import re


def signature(value):
    try:
        return ("return", str(inspect.signature(value)))
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


pattern = re.compile("a")
for name, value in (
    ("sub", re.sub),
    ("subn", re.subn),
    ("pattern_sub", pattern.sub),
    ("pattern_subn", pattern.subn),
):
    print(
        name,
        type(value).__module__,
        type(value).__name__,
        value.__module__,
        value.__name__,
        signature(value),
        repr(getattr(value, "__text_signature__", None)),
    )
print("aliases", pattern.sub.__self__ is pattern, pattern.subn.__self__ is pattern)
""".strip()
    )


def test_re_callable_substitution_results_and_order_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import re


def exercise(operation, pattern, source, count=0):
    events = []
    replacement = b"X" if isinstance(source, bytes) else "X"

    def replace(match):
        events.append((match.span(), match.group(0), match.lastindex, match.lastgroup))
        return replacement

    result = operation(replace, source, count=count)
    return result, events


for pattern_text, source in (("a", "a-a-a"), (b"a", b"a-a-a"), ("", "ab"), (b"", b"ab")):
    pattern = re.compile(pattern_text)
    for count in (0, 1, 2, -1):
        print("sub", repr(pattern_text), count, repr(exercise(pattern.sub, pattern, source, count)))
        print("subn", repr(pattern_text), count, repr(exercise(pattern.subn, pattern, source, count)))

events = []
def module_replace(match):
    events.append(match.span())
    return "R"

print("module_sub", re.sub("a", module_replace, "a-a"), events)
events.clear()
print("module_subn", re.subn("a", module_replace, "a-a"), events)
""".strip()
    )


def test_re_callable_return_values_and_exceptions_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import re


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class CallbackError(Exception):
    pass


for source, pattern, values in (
    ("a", re.compile("a"), ("", "X", None, 1, b"X")),
    (b"a", re.compile(b"a"), (b"", b"X", None, 1, "X")),
):
    for value in values:
        print(type(source).__name__, repr(value), outcome(lambda value=value: pattern.sub(lambda _m: value, source)))

    def fail(_match):
        raise CallbackError("replacement failed")

    print(type(source).__name__, "exception", outcome(lambda: pattern.sub(fail, source)))

print("missing", outcome(lambda: re.sub()))
print("unknown", outcome(lambda: re.sub("a", lambda _m: "x", "a", unknown=True)))
print("bad_count", outcome(lambda: re.sub("a", lambda _m: "x", "a", count="1")))
print("bad_callable", outcome(lambda: re.sub("a", 42, "a")))
print("mixed", outcome(lambda: re.sub(b"a", lambda _m: b"x", "a")))
""".strip()
    )


def test_re_callable_substitution_reentrancy_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import re


events = []


def outer(match):
    events.append(("outer", match.span()))
    nested = re.sub("b", lambda inner: f"<{inner.group(0)}>", "b-b")
    events.append(("nested", nested))
    return nested


print("result", re.sub("a", outer, "a-a"))
print("events", events)
""".strip()
    )


def test_re_match_metadata_late_validation_and_mutable_input_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import re


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


pattern = re.compile(r"(?P<letter>a)?b")
source = "ab-b"
events = []


def metadata(match):
    events.append(
        (
            match.span(),
            match.groups(),
            match.groupdict(),
            match.lastindex,
            match.lastgroup,
            match.re is pattern,
            match.string is source,
            match.pos,
            match.endpos,
        )
    )
    return match.group("letter")


print("metadata", pattern.subn(metadata, source), events)

events.clear()
print(
    "late_str_error",
    outcome(lambda: re.sub("a", lambda match: events.append(match.span()) or 42, "a-a-a")),
    events,
)
events.clear()
print(
    "late_bytes_error",
    outcome(lambda: re.sub(b"a", lambda match: events.append(match.span()) or "x", b"a-a-a")),
    events,
)

for replacement in (bytearray(b"X"), memoryview(b"Y")):
    print("bytes_like_replacement", re.sub(b"a", lambda _match, replacement=replacement: replacement, b"aba"))

mutable = bytearray(b"a-a")
mutable_events = []


def mutate(match):
    mutable_events.append(match.span())
    mutable[2] = ord("b")
    return b"X"


print("mutable", re.sub(b"a", mutate, mutable), bytes(mutable), mutable_events)

for pattern_text, text in ((r"x*", "abxd"), (r"|x", "x")):
    spans = []
    result = re.sub(pattern_text, lambda match: spans.append(match.span()) or "-", text)
    print("empty_order", pattern_text, result, spans)
""".strip()
    )


def test_pattern_sub_argument_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import re


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


pattern = re.compile("a")
callback = lambda _match: "x"
for label, call in (
    ("missing", lambda: pattern.sub()),
    ("one", lambda: pattern.sub(callback)),
    ("too_many", lambda: pattern.sub(callback, "a", 1, 2)),
    ("unknown", lambda: pattern.sub(callback, "a", unknown=True)),
    ("duplicate", lambda: pattern.sub(callback, "a", repl=callback)),
    ("bad_count", lambda: pattern.sub(callback, "a", count="1")),
    ("keywords", lambda: pattern.sub(repl=callback, string="a", count=1)),
):
    print(label, outcome(call))
""".strip()
    )
