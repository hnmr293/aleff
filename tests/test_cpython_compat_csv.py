"""CPython differential tests for the csv reader and writer accelerators."""

from __future__ import annotations

from cpython_compat_support import assert_cpython_compatible


def test_csv_writer_reentrant_record_state_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import csv


writes = []


class Sink:
    def write(self, value):
        writes.append(value)
        return len(value)


writer = csv.writer(Sink(), lineterminator="\n")


class Field:
    def __str__(self):
        writer.writerow(["inner"])
        return "b"


result = writer.writerow(["a", Field()])
print("result", result)
print("writes", writes)
""".strip()
    )


def test_csv_reader_writer_api_shape_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import csv
import inspect
import io
import sys


def signature(value):
    try:
        return ("return", str(inspect.signature(value)))
    except BaseException as exc:
        message = str(exc)
        if " at 0x" in message:
            message = message.split(" at 0x", 1)[0] + ">"
        return ("raise", type(exc).__name__, message)


def describe(value):
    return (
        type(value).__module__,
        type(value).__name__,
        getattr(value, "__module__", None),
        getattr(value, "__name__", None),
        signature(value),
        repr(getattr(value, "__text_signature__", None)),
    )


print("version", sys.version_info[:2])
for name in ("reader", "writer"):
    function = getattr(csv, name)
    print("function", name, describe(function))

reader = csv.reader([])
writer = csv.writer(io.StringIO())
for name, value in (("reader", reader), ("writer", writer)):
    public = tuple(item for item in dir(value) if not item.startswith("__"))
    print(
        "object",
        name,
        type(value).__module__,
        type(value).__name__,
        value.__module__,
        repr(value.__doc__.splitlines()[0]),
        public,
    )
    print("dialect", name, tuple(getattr(value.dialect, item) for item in (
        "delimiter", "doublequote", "escapechar", "lineterminator",
        "quotechar", "quoting", "skipinitialspace", "strict",
    )))

print("reader_iter", iter(reader) is reader, reader.line_num)
for name in ("writerow", "writerows"):
    method = getattr(writer, name)
    print("method", name, describe(method), method.__self__ is writer)


def outcome(operation):
    try:
        return ("return", operation())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


for label, operation in (
    ("reader_missing", lambda: csv.reader()),
    ("reader_too_many", lambda: csv.reader([], "excel", {}, 1)),
    ("reader_keyword_iterable", lambda: csv.reader(iterable=[])),
    ("reader_unknown", lambda: csv.reader([], unknown=True)),
    ("writer_missing", lambda: csv.writer()),
    ("writer_too_many", lambda: csv.writer(io.StringIO(), "excel", {}, 1)),
    ("writer_keyword_fileobj", lambda: csv.writer(fileobj=io.StringIO())),
    ("writer_unknown", lambda: csv.writer(io.StringIO(), unknown=True)),
):
    print(label, outcome(operation))

if sys.version_info >= (3, 13):
    expected_reader = ("return", "(iterable, /, dialect='excel', **fmtparams)")
    expected_writer = ("return", "(fileobj, /, dialect='excel', **fmtparams)")
    expected_row = ("return", "(row, /)")
    expected_rows = ("return", "(rows, /)")
else:
    expected_reader = (
        "raise",
        "ValueError",
        "no signature found for builtin <built-in function reader>",
    )
    expected_writer = (
        "raise",
        "ValueError",
        "no signature found for builtin <built-in function writer>",
    )
    expected_row = (
        "raise",
        "ValueError",
        "no signature found for builtin <built-in method writerow of _csv.writer object>",
    )
    expected_rows = (
        "raise",
        "ValueError",
        "no signature found for builtin <built-in method writerows of _csv.writer object>",
    )

assert signature(csv.reader) == expected_reader
assert signature(csv.writer) == expected_writer
assert signature(writer.writerow) == expected_row
assert signature(writer.writerows) == expected_rows
print("constants", csv.QUOTE_NOTNULL, csv.QUOTE_STRINGS)
""".strip()
    )


def test_csv_reader_consumes_input_iterator_and_preserves_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import csv


def outcome(operation):
    try:
        return ("return", operation())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class Input:
    def __init__(self, values, failure=None):
        self.values = list(values)
        self.failure = failure
        self.index = 0
        self.events = []

    def __iter__(self):
        self.events.append("iter")
        return self

    def __next__(self):
        self.events.append(("next", self.index))
        if self.index < len(self.values):
            value = self.values[self.index]
            self.index += 1
            return value
        if self.failure is not None:
            raise self.failure
        raise StopIteration


source = Input(["first,1\n", '"multi\n', 'line",2\n', "last,3\n"])
reader = csv.reader(source)
print("constructed", source.events, reader.line_num, iter(reader) is reader)
print("first", next(reader), source.events, reader.line_num)
print("second", next(reader), source.events, reader.line_num)
print("third", next(reader), source.events, reader.line_num)
print("exhausted", outcome(lambda: next(reader)), source.events, reader.line_num)

failing = Input(["ok,1\n"], RuntimeError("input failed"))
reader = csv.reader(failing)
print("partial_error", outcome(lambda: list(reader)), failing.events, reader.line_num)

class BadIterable:
    def __iter__(self):
        raise LookupError("iteration failed")


for label, value in (
    ("none", None),
    ("bad_iter", BadIterable()),
    ("bytes", [b"a,b\n"]),
    ("non_string", [42]),
):
    print(label, outcome(lambda value=value: list(csv.reader(value))))


if "aleff" in __import__("sys").modules:
    from aleff import create_handler, effect

    choose = effect("csv_reader_input")
    handler = create_handler(choose)

    class EffectInput:
        def __init__(self):
            self.events = []
            self.used = False

        def __iter__(self):
            self.events.append("iter")
            return self

        def __next__(self):
            self.events.append("next")
            if self.used:
                raise StopIteration
            self.used = True
            return choose()

    effect_input = EffectInput()

    @handler.on(choose)
    def resume(k):
        return k("suspended,1\n")

    result = handler(lambda: list(csv.reader(effect_input)))
else:
    class EffectInput:
        def __init__(self):
            self.events = []
            self.used = False

        def __iter__(self):
            self.events.append("iter")
            return self

        def __next__(self):
            self.events.append("next")
            if self.used:
                raise StopIteration
            self.used = True
            return "suspended,1\n"

    effect_input = EffectInput()
    result = list(csv.reader(effect_input))

print("effectful_input", result, effect_input.events)
""".strip()
    )


def test_csv_reader_multiline_quoting_and_conversion_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import csv
import sys


def outcome(operation):
    try:
        return ("return", operation())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


lines = [
    'name;comment\n',
    '"Ada;A";"line one\nline two"\n',
    '"quoted ""word""' '";tail\n',
]
reader = csv.reader(
    lines,
    delimiter=";",
    quotechar='"',
    doublequote=True,
    strict=True,
)
print("quoted", list(reader), reader.line_num)

numbers = list(csv.reader(["1,2.5,\"3\",,\n"], quoting=csv.QUOTE_NONNUMERIC))
print("nonnumeric", numbers)
print("nonnumeric_error", outcome(lambda: list(csv.reader(["text,2\n"], quoting=csv.QUOTE_NONNUMERIC))))

notnull = list(csv.reader(["1,2.5,\"3\",,\n"], quoting=csv.QUOTE_NOTNULL))
strings = outcome(lambda: list(csv.reader(["1,2.5,\"3\",,\n"], quoting=csv.QUOTE_STRINGS)))
if sys.version_info >= (3, 13):
    assert notnull == [["1", "2.5", "3", None, None]]
    assert strings == ("return", [[1.0, 2.5, "3", None, None]])
else:
    assert notnull == [["1", "2.5", "3", "", ""]]
    assert strings == ("return", [["1", "2.5", "3", "", ""]])
print("notnull", notnull)
print("strings", strings)

for label, text, options in (
    ("bad_quote", ['"unterminated\n'], {}),
    ("bad_escape", ["a,b\\"], {"escapechar": "\\"}),
):
    print(
        label,
        outcome(
            lambda text=text, options=options: list(
                csv.reader(text, strict=True, **options)
            )
        ),
    )
""".strip()
    )


def test_csv_writer_write_returns_and_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import csv


def outcome(operation):
    try:
        return ("return", operation())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class Sink:
    def __init__(self, result=None, failure=None):
        self.result = result
        self.failure = failure
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        if self.failure is not None:
            raise self.failure
        return self.result


for result in (None, 0, 7, "write-result"):
    sink = Sink(result=result)
    writer = csv.writer(sink)
    print("write_result", repr(result), outcome(lambda: writer.writerow(["a,b", None, 3])), sink.writes)

sink = Sink(failure=RuntimeError("write failed"))
writer = csv.writer(sink)
print("write_error", outcome(lambda: writer.writerow(["partial", 1])), sink.writes)

class BadString:
    def __str__(self):
        raise ValueError("conversion failed")


sink = Sink(result=None)
writer = csv.writer(sink)
print("conversion_error", outcome(lambda: writer.writerow(["before", BadString(), "after"])), sink.writes)

for label, value in (
    ("bad_file", None),
    ("bad_file_type", 1),
):
    print(label, outcome(lambda value=value: csv.writer(value)))

sink = Sink(result=3)
writer = csv.writer(sink)
for label, operation in (
    ("missing", lambda: writer.writerow()),
    ("too_many", lambda: writer.writerow([1], [2])),
    ("keyword", lambda: writer.writerow(row=[1])),
    ("writers_missing", lambda: writer.writerows()),
    ("writers_keyword", lambda: writer.writerows(rows=[[1]])),
):
    print(label, outcome(operation))


if "aleff" in __import__("sys").modules:
    from aleff import create_handler, effect

    choose = effect("csv_writer_write")
    handler = create_handler(choose)

    class EffectSink:
        def __init__(self):
            self.writes = []

        def write(self, data):
            self.writes.append(data)
            return choose()

    effect_sink = EffectSink()
    effect_writer = csv.writer(effect_sink)

    @handler.on(choose)
    def resume(k):
        return k(11)

    effect_result = handler(lambda: effect_writer.writerow(["effect", 1]))
else:
    class EffectSink:
        def __init__(self):
            self.writes = []

        def write(self, data):
            self.writes.append(data)
            return 11

    effect_sink = EffectSink()
    effect_writer = csv.writer(effect_sink)
    effect_result = effect_writer.writerow(["effect", 1])

print("effectful_write", effect_result, effect_sink.writes)
""".strip()
    )


def test_csv_writerows_consumption_and_partial_failures_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import csv


def outcome(operation):
    try:
        return ("return", operation())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class Rows:
    def __init__(self, values, failure=None):
        self.values = list(values)
        self.failure = failure
        self.index = 0
        self.events = []

    def __iter__(self):
        self.events.append("iter")
        return self

    def __next__(self):
        self.events.append(("next", self.index))
        if self.index < len(self.values):
            value = self.values[self.index]
            self.index += 1
            return value
        if self.failure is not None:
            raise self.failure
        raise StopIteration


class Sink:
    def __init__(self):
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        return len(data)


sink = Sink()
writer = csv.writer(sink)
rows = Rows([[1, "one"], [2, "two"]])
print("normal", outcome(lambda: writer.writerows(rows)), rows.events, sink.writes)

sink = Sink()
writer = csv.writer(sink)
rows = Rows([[1], [2]], RuntimeError("rows input failed"))
print("input_error", outcome(lambda: writer.writerows(rows)), rows.events, sink.writes)

sink = Sink()
writer = csv.writer(sink)
bad_rows = Rows([["first"], 42, ["never"]])
print("row_error", outcome(lambda: writer.writerows(bad_rows)), bad_rows.events, sink.writes)

for value in (None, 42, object()):
    sink = Sink()
    writer = csv.writer(sink)
    print("bad_rows", type(value).__name__, outcome(lambda value=value: writer.writerows(value)), sink.writes)


if "aleff" in __import__("sys").modules:
    from aleff import create_handler, effect

    choose = effect("csv_writerows_input")
    handler = create_handler(choose)

    class EffectRows:
        def __init__(self):
            self.events = []
            self.used = False

        def __iter__(self):
            self.events.append("iter")
            return self

        def __next__(self):
            self.events.append("next")
            if self.used:
                raise StopIteration
            self.used = True
            return choose()

    effect_rows = EffectRows()
    effect_sink = Sink()
    effect_writer = csv.writer(effect_sink)

    @handler.on(choose)
    def resume(k):
        return k(["resumed", 1])

    effect_result = handler(lambda: effect_writer.writerows(effect_rows))
else:
    class EffectRows:
        def __init__(self):
            self.events = []
            self.used = False

        def __iter__(self):
            self.events.append("iter")
            return self

        def __next__(self):
            self.events.append("next")
            if self.used:
                raise StopIteration
            self.used = True
            return ["resumed", 1]

    effect_rows = EffectRows()
    effect_sink = Sink()
    effect_writer = csv.writer(effect_sink)
    effect_result = effect_writer.writerows(effect_rows)

print("effectful_rows", effect_result, effect_rows.events, effect_sink.writes)
""".strip()
    )


def test_csv_reader_and_writer_reentrancy_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import csv


def outcome(operation):
    try:
        return ("return", operation())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


reader_events = []
nested_reader = csv.reader(["nested,1\n"])


class OuterInput:
    def __init__(self):
        self.used = False

    def __iter__(self):
        reader_events.append("outer_iter")
        return self

    def __next__(self):
        reader_events.append("outer_next")
        if self.used:
            raise StopIteration
        self.used = True
        reader_events.append(("nested", next(nested_reader)))
        return "outer,2\n"


outer_reader = csv.reader(OuterInput())
print("reader_reentry", outcome(lambda: list(outer_reader)), reader_events)


writer_events = []


class ReentrantSink:
    def __init__(self):
        self.writer = None
        self.nested = False

    def write(self, data):
        writer_events.append(("write", data))
        if not self.nested:
            self.nested = True
            writer_events.append(("nested_result", self.writer.writerow(["inner", 1])))
        return len(data)


sink = ReentrantSink()
writer = csv.writer(sink)
sink.writer = writer
print("writer_reentry", outcome(lambda: writer.writerow(["outer", 2])), writer_events)
""".strip()
    )
