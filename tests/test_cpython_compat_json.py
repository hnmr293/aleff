"""CPython differential tests for the json parsing and serialization APIs."""

from __future__ import annotations

from cpython_compat_support import assert_cpython_compatible


def test_json_public_api_shape_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import inspect
import json
from json import decoder, encoder, scanner
import re
import sys


def shape(label, value):
    try:
        signature = ("return", re.sub(r"0x[0-9a-fA-F]+", "0xADDR", str(inspect.signature(value))))
    except BaseException as exc:
        signature = ("raise", type(exc).__name__, str(exc))
    print(
        label,
        type(value).__module__,
        type(value).__name__,
        getattr(value, "__module__", None),
        getattr(value, "__name__", None),
        signature,
        repr(getattr(value, "__text_signature__", None)),
    )


print("runtime", sys.version_info[:2])
print(
    "accelerators",
    encoder.c_make_encoder is not None,
    scanner.c_make_scanner is not None,
    decoder.c_scanstring is not None,
)
for name in ("dump", "dumps", "load", "loads"):
    shape(name, getattr(json, name))

encoder = json.JSONEncoder()
decoder = json.JSONDecoder()
for name, value in (
    ("JSONEncoder", json.JSONEncoder),
    ("JSONEncoder.encode", encoder.encode),
    ("JSONEncoder.iterencode", encoder.iterencode),
    ("JSONDecoder", json.JSONDecoder),
    ("JSONDecoder.decode", decoder.decode),
    ("JSONDecoder.raw_decode", decoder.raw_decode),
):
    shape(name, value)

print("aliases", json.dump.__module__, json.dumps.__module__, json.load.__module__, json.loads.__module__)
""".strip()
    )


def test_json_encode_callbacks_order_arguments_and_one_shot_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import json


class Value:
    def __init__(self, label, number):
        self.label = label
        self.number = number


value = {"outer": [Value("first", 1), {"inner": Value("second", 2)}]}


def exercise(label, operation):
    events = []

    def default(obj):
        events.append(("default", type(obj).__name__, obj.label, obj.number))
        return {"label": obj.label, "number": obj.number}

    result = operation(default)
    print(label, repr(result), events)


exercise("dumps", lambda default: json.dumps(value, default=default, sort_keys=True))


class Writer:
    def __init__(self, events):
        self.events = events

    def write(self, text):
        self.events.append(("write", type(text).__name__, text))
        return object()


events = []


def default_for_dump(obj):
    events.append(("default", type(obj).__name__, obj.label, obj.number))
    return {"label": obj.label, "number": obj.number}


writer = Writer(events)
result = json.dump(value, writer, default=default_for_dump, sort_keys=True)
print("dump", result, events)


encoder_events = []


def default_for_encoder(obj):
    encoder_events.append(obj.label)
    return {"label": obj.label, "number": obj.number}


encoder = json.JSONEncoder(default=default_for_encoder, sort_keys=True)
for one_shot in (False, True):
    encoder_events.clear()
    chunks = encoder.iterencode(value, _one_shot=one_shot)
    print("iterencode", one_shot, type(chunks).__name__, repr("".join(chunks)), encoder_events)

encoder_events.clear()
print("encode", encoder.encode(value), encoder_events)
""".strip()
    )


def test_json_decode_callbacks_order_arguments_and_hook_precedence_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import json


source = '{"outer": [1, 2.5, -Infinity, Infinity, NaN, {"inner": 3}], "last": 4}'


def parse_exercise(label, operation, use_pairs=False):
    events = []

    def parse_int(text):
        events.append(("parse_int", text))
        return "I:" + text

    def parse_float(text):
        events.append(("parse_float", text))
        return "F:" + text

    def parse_constant(text):
        events.append(("parse_constant", text))
        return "C:" + text

    def object_hook(obj):
        events.append(("object_hook", list(obj.items())))
        return ("object", tuple(obj.items()))

    def object_pairs_hook(pairs):
        events.append(("object_pairs_hook", pairs))
        return ("pairs", tuple(pairs))

    callbacks = dict(
        parse_int=parse_int,
        parse_float=parse_float,
        parse_constant=parse_constant,
        object_hook=object_hook,
    )
    if use_pairs:
        callbacks["object_pairs_hook"] = object_pairs_hook
    if operation == "loads":
        result = json.loads(source, **callbacks)
    else:
        class Reader:
            def read(self):
                return source

        result = json.load(Reader(), **callbacks)
    print(label, repr(result), events)


for operation in ("loads", "load"):
    parse_exercise(operation + "_object_hook", operation)
    parse_exercise(operation + "_object_pairs_hook", operation, use_pairs=True)


class Decoder(json.JSONDecoder):
    def __init__(self, *args, **kwargs):
        print("decoder_init", args, sorted(kwargs))
        super().__init__(*args, **kwargs)


decoder_events = []


def decoder_hook(obj):
    decoder_events.append(("hook", list(obj.items())))
    return obj


decoder = Decoder(object_hook=decoder_hook)
print("decode", decoder.decode('{"a": {"b": 1}}'), decoder_events)
decoder_events.clear()
print("raw_decode", decoder.raw_decode('{"a": 1} trailing'), decoder_events)
""".strip()
    )


def test_json_input_types_reader_protocol_and_argument_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import json


def outcome(call):
    try:
        value = call()
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc), tuple(getattr(exc, "__notes__", ())))
    return ("return", type(value).__name__, repr(value))


payloads = ('{"text": "é", "value": 2}', b'{"text": "\xc3\xa9", "value": 2}', bytearray(b'{"value": 2}'))
for payload in payloads:
    print("loads", type(payload).__name__, outcome(lambda payload=payload: json.loads(payload)))

for payload in (memoryview(b"{}"), b"\xff", 42, None):
    print("loads_invalid", type(payload).__name__, outcome(lambda payload=payload: json.loads(payload)))


class Reader:
    def __init__(self, value):
        self.value = value
        self.events = []

    def read(self, *args):
        self.events.append(("read", args))
        return self.value


for payload in (b'{"value": 1}', bytearray(b'{"value": 2}'), '{"value": 3}'):
    reader = Reader(payload)
    print("load", type(payload).__name__, outcome(lambda: json.load(reader)), reader.events)


class MissingRead:
    pass


class BadRead:
    def read(self):
        return 42


print("load_invalid", outcome(lambda: json.load(MissingRead())))
print("load_bad_read", outcome(lambda: json.load(BadRead())))

for label, call in (
    ("dumps_missing", lambda: json.dumps()),
    ("dumps_too_many", lambda: json.dumps({}, None)),
    ("dumps_unknown", lambda: json.dumps({}, unknown=True)),
    ("loads_missing", lambda: json.loads()),
    ("loads_too_many", lambda: json.loads("{}", 1)),
    ("loads_unknown", lambda: json.loads("{}", unknown=True)),
    ("load_missing", lambda: json.load()),
    ("dump_missing", lambda: json.dump({})),
):
    print(label, outcome(call))
""".strip()
    )


def test_json_options_nested_values_and_encoder_decoder_classes_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import json


def outcome(call):
    try:
        value = call()
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc), tuple(getattr(exc, "__notes__", ())))
    return ("return", type(value).__name__, repr(value))


def recursion_outcome(call):
    try:
        value = call()
    except BaseException as exc:
        message = str(exc)
        if message.startswith("Stack overflow (used "):
            _usage, separator, suffix = message.partition(" kB)")
            if separator:
                message = "Stack overflow" + suffix
        notes = tuple(dict.fromkeys(getattr(exc, "__notes__", ())))
        return ("raise", type(exc).__name__, message, notes)
    return ("return", type(value).__name__, repr(value))


value = {"z": "é", "a": [1, 2]}
for label, options in (
    ("default", {}),
    ("ascii_off_sorted", {"ensure_ascii": False, "sort_keys": True}),
    ("indent", {"indent": 2}),
    ("indent_string", {"indent": "\t"}),
    ("separators", {"separators": (",", ":")}),
):
    print(label, outcome(lambda options=options: json.dumps(value, **options)))

mixed_keys = {"value": "text", 7: "integer key", (1, 2): "tuple key"}
print("skipkeys", outcome(lambda: json.dumps(mixed_keys, skipkeys=True)))
print("skipkeys_error", outcome(lambda: json.dumps({(1, 2): "tuple"})))
print("skipkeys_ok", outcome(lambda: json.dumps({(1, 2): "tuple"}, skipkeys=True)))
print("nan_ok", outcome(lambda: json.dumps(float("nan"))))
print("nan_error", outcome(lambda: json.dumps(float("nan"), allow_nan=False)))

cycle = []
cycle.append(cycle)
print("circular", outcome(lambda: json.dumps(cycle)))
print("no_circular", recursion_outcome(lambda: json.dumps(cycle, check_circular=False)))


class Encoder(json.JSONEncoder):
    def __init__(self, *args, **kwargs):
        print(
            "encoder_init",
            args,
            [(key, type(value).__name__, repr(value)) for key, value in sorted(kwargs.items())],
        )
        super().__init__(*args, **kwargs)

    def default(self, obj):
        print("encoder_default", type(obj).__name__, obj)
        if isinstance(obj, complex):
            return [obj.real, obj.imag]
        return super().default(obj)


print("custom_encoder", json.dumps({"value": 1 + 2j}, cls=Encoder, indent=1))


class Decoder(json.JSONDecoder):
    def __init__(self, *args, **kwargs):
        print(
            "custom_decoder_init",
            args,
            [(key, type(value).__name__, repr(value)) for key, value in sorted(kwargs.items())],
        )
        super().__init__(*args, **kwargs)


print("custom_decoder", json.loads('{"value": 1}', cls=Decoder, strict=False))
print("strict_default", outcome(lambda: json.loads('"line\nbreak"')))
print("strict_false", outcome(lambda: json.loads('"line\nbreak"', strict=False)))
""".strip()
    )


def test_json_callback_exceptions_and_writer_failures_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import json


class CallbackError(Exception):
    pass


def outcome(call):
    try:
        value = call()
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc), tuple(getattr(exc, "__notes__", ())))
    return ("return", type(value).__name__, repr(value))


default_events = []


class Value:
    pass


def failing_default(obj):
    default_events.append(type(obj).__name__)
    raise CallbackError("default failed")


print("default_error", outcome(lambda: json.dumps([Value(), Value()], default=failing_default)), default_events)

parse_events = []


def failing_int(text):
    parse_events.append(text)
    raise CallbackError("integer failed")


print("parse_int_error", outcome(lambda: json.loads('[1, 2]', parse_int=failing_int)), parse_events)

hook_events = []


def failing_hook(obj):
    hook_events.append(list(obj.items()))
    raise CallbackError("hook failed")


print(
    "object_hook_error",
    outcome(lambda: json.loads('{"outer": {"inner": 1}}', object_hook=failing_hook)),
    hook_events,
)

pairs_events = []


def failing_pairs(pairs):
    pairs_events.append(pairs)
    raise CallbackError("pairs hook failed")


print(
    "object_pairs_hook_error",
    outcome(lambda: json.loads('{"value": 1}', object_pairs_hook=failing_pairs)),
    pairs_events,
)

float_events = []


def failing_float(text):
    float_events.append(text)
    raise CallbackError("float failed")


print("parse_float_error", outcome(lambda: json.loads("[1.5]", parse_float=failing_float)), float_events)

constant_events = []


def failing_constant(text):
    constant_events.append(text)
    raise CallbackError("constant failed")


print("parse_constant_error", outcome(lambda: json.loads("[NaN]", parse_constant=failing_constant)), constant_events)


class Writer:
    def __init__(self):
        self.events = []

    def write(self, text):
        self.events.append(text)
        raise CallbackError("write failed")


writer = Writer()
print("writer_error", outcome(lambda: json.dump({"a": [1, 2]}, writer)), writer.events)
""".strip()
    )
