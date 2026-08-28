"""CPython compatibility regressions for open, input, and print."""

from __future__ import annotations

from cpython_compat_support import assert_cpython_compatible


def test_builtin_metadata_and_signatures_match_cpython() -> None:
    assert_cpython_compatible(
        """
import builtins
import inspect

for name in ("open", "input", "print"):
    function = getattr(builtins, name)
    doc = function.__doc__
    first_doc_line = None if doc is None else doc.splitlines()[0]
    try:
        signature = repr(inspect.signature(function))
    except Exception as exc:
        signature = (type(exc).__name__, str(exc))
    print(name, function.__module__, function.__name__, first_doc_line, signature)
        """
    )


def test_open_normal_paths_and_errors_match_cpython() -> None:
    assert_cpython_compatible(
        """
import os
import tempfile

def describe(label, operation):
    try:
        result = operation()
    except Exception as exc:
        print(label, "raise", type(exc).__name__)
    else:
        print(label, "return", type(result).__name__)
        close = getattr(result, "close", None)
        if close is not None:
            close()

with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, "sample.txt")
    with open(path, "w", encoding="utf-8", newline="") as stream:
        print("normal write", stream.write("alpha\\nbeta"))
    with open(path, "r", encoding="utf-8", newline="") as stream:
        print("normal read", repr(stream.read()))

    class BytesPath:
        def __fspath__(self):
            return os.fsencode(path)

    with open(BytesPath(), "rb") as stream:
        print("bytes path", stream.read())

    class InvalidPath:
        def __fspath__(self):
            return 42

    describe("invalid path", lambda: open(InvalidPath(), "rb"))
    describe("missing path", lambda: open(os.path.join(directory, "missing"), "rb"))
    describe("invalid mode", lambda: open(path, "not-a-mode"))
    describe("no arguments", lambda: open())
        """
    )


def test_open_accepts_indexable_file_descriptors() -> None:
    assert_cpython_compatible(
        """
import os
import tempfile

with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, "sample.txt")
    with open(path, "wb") as stream:
        stream.write(b"descriptor")

    descriptor = os.open(path, os.O_RDONLY)

    class FileDescriptor:
        def __index__(self):
            return descriptor

    try:
        with open(FileDescriptor(), "rb") as stream:
            print("result", type(stream).__name__, stream.read())
    except Exception as exc:
        print("error", type(exc).__name__)
    finally:
        try:
            os.fstat(descriptor)
        except OSError:
            pass
        else:
            os.close(descriptor)
        """
    )


def test_open_opener_result_validation_survives_resume() -> None:
    assert_cpython_compatible(
        """
import os
import sys
import tempfile

if "aleff" in sys.modules:
    from aleff import create_handler, effect

with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, "sample.txt")
    with open(path, "wb") as stream:
        stream.write(b"opener")

    def describe(operation):
        try:
            result = operation()
        except Exception as exc:
            return ("raise", type(exc).__name__)
        try:
            return ("return", type(result).__name__)
        finally:
            close = getattr(result, "close", None)
            if close is not None:
                close()

    if "aleff" not in sys.modules:
        values = iter((None, "bad", "fd"))

        def opener(opener_path, flags):
            value = next(values)
            if value == "fd":
                return os.open(opener_path, flags)
            return value

        outcomes = [describe(lambda: open(path, "rb", opener=opener)) for _ in range(3)]
    else:
        choose = effect("choose")
        handler = create_handler(choose)

        def opener(opener_path, flags):
            return choose()

        @handler.on(choose)
        def handle(k):
            descriptor = os.open(path, os.O_RDONLY)
            return [
                describe(lambda value=value: k(value))
                for value in (None, "bad", descriptor)
            ]

        outcomes = handler(lambda: open(path, "rb", opener=opener))
    print(outcomes)
        """
    )


def test_open_audit_event_is_emitted_once_across_opener_resume() -> None:
    assert_cpython_compatible(
        """
import os
import sys
import tempfile

if "aleff" in sys.modules:
    from aleff import create_handler, effect

with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, "sample.txt")
    with open(path, "wb") as stream:
        stream.write(b"audit")
    descriptor = os.open(path, os.O_RDONLY)
    events = []
    sys.addaudithook(lambda event, args: events.append(event))

    if "aleff" not in sys.modules:
        def opener(opener_path, flags):
            return descriptor

        with open(path, "rb", opener=opener) as stream:
            print("content", stream.read())
    else:
        choose = effect("choose")
        handler = create_handler(choose)

        def opener(opener_path, flags):
            return choose()

        @handler.on(choose)
        def handle(k):
            return k(descriptor)

        result = handler(lambda: open(path, "rb", opener=opener))
        print("content", result.read())
        result.close()
    print("open events", events.count("open"))
        """
    )


def test_input_normal_line_and_error_behavior_match_cpython() -> None:
    assert_cpython_compatible(
        """
import io
import sys

def describe(line):
    class Input:
        def readline(self):
            return line

    old_stdin, old_stdout = sys.stdin, sys.stdout
    captured = io.StringIO()
    sys.stdin, sys.stdout = Input(), captured
    try:
        try:
            result = input()
        except Exception as exc:
            return ("raise", type(exc).__name__, str(exc), captured.getvalue())
        return ("return", type(result).__name__, repr(result), captured.getvalue())
    finally:
        sys.stdin, sys.stdout = old_stdin, old_stdout

for line in ("plain\\n", "plain", "", "plain\\r\\n"):
    print(repr(describe(line)))
        """
    )


def test_input_validates_streams_before_writing_prompt() -> None:
    assert_cpython_compatible(
        """
import io
import sys

old_stdin, old_stdout = sys.stdin, sys.stdout
captured = io.StringIO()
sys.stdin, sys.stdout = None, captured
try:
    try:
        input("prompt: ")
    except Exception as exc:
        result = ("raise", type(exc).__name__, str(exc))
    else:
        result = ("return",)
finally:
    sys.stdin, sys.stdout = old_stdin, old_stdout
print(result, repr(captured.getvalue()))
        """
    )


def test_input_flushes_and_orders_stream_operations_like_cpython() -> None:
    assert_cpython_compatible(
        """
import sys

events = []

class Input:
    def readline(self):
        events.append(("stdin", "readline"))
        return "answer\\n"

class Stream:
    def __init__(self, name):
        self.name = name

    def write(self, value):
        events.append((self.name, "write", value))
        return len(value)

    def flush(self):
        events.append((self.name, "flush"))

old_stdin, old_stdout, old_stderr = sys.stdin, sys.stdout, sys.stderr
sys.stdin, sys.stdout, sys.stderr = Input(), Stream("stdout"), Stream("stderr")
try:
    result = input("prompt: ")
finally:
    sys.stdin, sys.stdout, sys.stderr = old_stdin, old_stdout, old_stderr
print(result, events)
        """
    )


def test_input_ignores_noninteractive_prompt_flush_errors() -> None:
    assert_cpython_compatible(
        """
import sys

class Input:
    def readline(self):
        return "answer\\n"

class Output:
    def write(self, value):
        return len(value)

    def flush(self):
        raise RuntimeError("flush failed")

old_stdin, old_stdout = sys.stdin, sys.stdout
sys.stdin, sys.stdout = Input(), Output()
try:
    try:
        result = input("prompt: ")
    except Exception as exc:
        result = ("raise", type(exc).__name__, str(exc))
    else:
        result = ("return", type(result).__name__, result)
finally:
    sys.stdin, sys.stdout = old_stdin, old_stdout
print(result)
        """
    )


def test_input_audit_event_contains_prompt_before_output() -> None:
    assert_cpython_compatible(
        """
import sys

events = []

class Input:
    def readline(self):
        events.append(("readline",))
        return "answer\\n"

class Output:
    def write(self, value):
        events.append(("write", value))
        return len(value)

    def flush(self):
        events.append(("flush",))

sys.addaudithook(lambda event, args: events.append(("audit", event, args)))
old_stdin, old_stdout = sys.stdin, sys.stdout
sys.stdin, sys.stdout = Input(), Output()
try:
    result = input("prompt: ")
finally:
    sys.stdin, sys.stdout = old_stdin, old_stdout
print(result, [event for event in events if event[0] in ("audit", "write", "flush", "readline")])
        """
    )


def test_input_resume_matches_cpython_line_and_error_behavior() -> None:
    assert_cpython_compatible(
        """
import io
import sys

if "aleff" in sys.modules:
    from aleff import create_handler, effect

values = ("answer\\r\\n", "", b"bytes\\n", "done\\n")

def describe(operation):
    try:
        result = operation()
    except Exception as exc:
        return ("raise", type(exc).__name__, str(exc))
    return ("return", type(result).__name__, result)

old_stdin, old_stdout = sys.stdin, sys.stdout
sys.stdout = io.StringIO()

if "aleff" not in sys.modules:
    class Input:
        def __init__(self):
            self.values = iter(values)

        def readline(self):
            return next(self.values)

    sys.stdin = Input()
    outcomes = [describe(lambda: input()) for _ in values]
else:
    choose = effect("choose")
    handler = create_handler(choose)

    class Input:
        def readline(self):
            return choose()

    sys.stdin = Input()

    @handler.on(choose)
    def handle(k):
        return [describe(lambda value=value: k(value)) for value in values]

    outcomes = handler(lambda: input())

sys.stdin, sys.stdout = old_stdin, old_stdout
print(outcomes)
        """
    )


def test_print_normal_output_and_flush_match_cpython() -> None:
    assert_cpython_compatible(
        """
import io

output = io.StringIO()
result = print("alpha", 2, sep="|", end="!", file=output, flush=True)
print(result, repr(output.getvalue()))
        """
    )


def test_print_converts_separator_and_end_before_writing() -> None:
    assert_cpython_compatible(
        """
seen = []

class StringSubclass(str):
    def __str__(self):
        return "converted"

class Output:
    def write(self, value):
        seen.append((value, type(value).__name__))
        return len(value)

print("a", "b", sep=StringSubclass("-"), end=StringSubclass("!"), file=Output())
print(seen)
        """
    )


def test_print_converts_flush_before_validating_separator_and_end() -> None:
    assert_cpython_compatible(
        """
events = []

class Flush:
    def __bool__(self):
        events.append("flush-bool")
        return False

class Output:
    def write(self, value):
        events.append(("write", value))
        return len(value)

def observe(label, **kwargs):
    events.clear()
    try:
        print("value", file=Output(), flush=Flush(), **kwargs)
    except BaseException as exc:
        result = ("raise", type(exc).__name__, str(exc))
    else:
        result = ("return",)
    print(label, result, events)

observe("invalid-sep", sep=object())
observe("invalid-end", end=object())
        """
    )


def test_print_returns_none_and_skips_argument_validation_without_stdout() -> None:
    assert_cpython_compatible(
        """
import sys

old_stdout = sys.stdout
sys.stdout = None
try:
    try:
        result = print(1, sep=1)
    except Exception as exc:
        result = ("raise", type(exc).__name__, str(exc))
    else:
        result = ("return", result)
finally:
    sys.stdout = old_stdout
print(result)
        """
    )
