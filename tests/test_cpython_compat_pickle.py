"""CPython differential tests for pickle's public and C accelerator APIs."""

from __future__ import annotations

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible


def _compatible(source: str, *, timeout: float = 10) -> None:
    assert_cpython_compatible(dedent(source), timeout=timeout)


def test_pickle_public_and_c_api_shape_matches_cpython() -> None:
    _compatible(
        """
        import inspect
        import pickle
        import _pickle


        def describe(value):
            try:
                signature = str(inspect.signature(value))
            except BaseException as exc:
                signature = (type(exc).__name__, str(exc))
            return (
                type(value).__module__,
                type(value).__name__,
                getattr(value, "__module__", None),
                getattr(value, "__name__", None),
                signature,
                repr(getattr(value, "__text_signature__", None)),
            )


        for module in (pickle, _pickle):
            print("module", module.__name__, getattr(module, "__file__", None) is not None)
            for name in (
                "dump",
                "dumps",
                "load",
                "loads",
                "Pickler",
                "Unpickler",
                "PickleBuffer",
            ):
                value = getattr(module, name, None)
                print(module.__name__, name, describe(value) if value is not None else None)

        for owner, names in (
            (pickle.Pickler, ("dump", "clear_memo", "persistent_id", "reducer_override")),
            (pickle.Unpickler, ("load", "find_class", "persistent_load")),
            (pickle.PickleBuffer, ("raw", "release")),
        ):
            for name in names:
                value = getattr(owner, name, None)
                print("method", owner.__name__, name, describe(value) if value is not None else None)

        print(
            "aliases",
            pickle.dump is _pickle.dump,
            pickle.dumps is _pickle.dumps,
            pickle.load is _pickle.load,
            pickle.loads is _pickle.loads,
            pickle.Pickler is _pickle.Pickler,
            pickle.Unpickler is _pickle.Unpickler,
            pickle.PickleBuffer is getattr(_pickle, "PickleBuffer", None),
            pickle.Pickler is pickle._Pickler,
            pickle.Unpickler is pickle._Unpickler,
        )
        print(
            "constants",
            pickle.DEFAULT_PROTOCOL,
            pickle.HIGHEST_PROTOCOL,
            pickle.format_version,
            pickle.compatible_formats,
        )
        print("pure_python", pickle._Pickler.__module__, pickle._Unpickler.__module__)
        """
    )


def test_pickle_protocols_memo_and_cycles_match_cpython() -> None:
    _compatible(
        """
        import io
        import pickle
        import _pickle


        def graph_summary(value):
            shared = value["left"]
            return (
                type(value).__name__,
                value["self"] is value,
                value["left"] is value["right"],
                shared[0] is value,
                value["number"],
                value["text"],
            )


        graph = {"number": 7, "text": "cycle", "self": None}
        shared = [graph, "shared"]
        graph["self"] = graph
        graph["left"] = shared
        graph["right"] = shared

        for protocol in (0, 4, 5):
            for name, dumper, loader in (
                ("pickle", pickle.dumps, pickle.loads),
                ("_pickle", _pickle.dumps, _pickle.loads),
            ):
                payload = dumper(graph, protocol=protocol)
                restored = loader(payload)
                print(name, "protocol", protocol, "payload", repr(payload))
                print(name, "summary", graph_summary(restored))

            stream = io.BytesIO()
            pickle.dump(graph, stream, protocol=protocol)
            stream.seek(0)
            restored = _pickle.load(stream)
            print("stream", protocol, graph_summary(restored), stream.tell(), len(stream.getvalue()))

        pickler_stream = io.BytesIO()
        pickler = pickle.Pickler(pickler_stream, protocol=4)
        print("memo_initial", len(pickler.memo.copy()))
        pickler.dump([graph, graph])
        memo_size = len(pickler.memo.copy())
        pickler.clear_memo()
        print("memo_after_dump", memo_size, "memo_after_clear", len(pickler.memo.copy()))
        """
    )


def test_pickle_reducers_and_state_callbacks_match_cpython() -> None:
    _compatible(
        """
        import io
        import pickle


        events = []


        def rebuild_reduced(value):
            events.append(("rebuild", value))
            result = Reduced.__new__(Reduced, value)
            result.value = value
            return result


        class Reduced:
            def __new__(cls, value):
                events.append(("new", value))
                return super().__new__(cls)

            def __init__(self, value):
                events.append(("init", value))
                self.value = value
                self.state = "original"

            def __reduce_ex__(self, protocol):
                events.append(("reduce_ex", protocol, self.value))
                return (rebuild_reduced, (self.value,), {"state": self.state})

            def __setstate__(self, state):
                events.append(("setstate", state))
                self.state = state["state"] + ":restored"


        class GetState:
            def __init__(self, value):
                self.value = value

            def __getstate__(self):
                events.append(("getstate", self.value))
                return {"value": self.value, "extra": 11}

            def __setstate__(self, state):
                events.append(("setstate_default", state))
                self.value = state["value"] + state["extra"]


        def describe(value):
            return (type(value).__name__, value.__dict__)


        for protocol in (0, 4, 5):
            events.clear()
            value = Reduced(protocol)
            restored = pickle.loads(pickle.dumps(value, protocol=protocol))
            print("reduced", protocol, describe(restored), events)

            events.clear()
            value = GetState(protocol)
            restored = pickle.loads(pickle.dumps(value, protocol=protocol))
            print("state", protocol, describe(restored), events)

        class Dispatch:
            def __init__(self, value):
                self.value = value


        def reduce_dispatch(value):
            events.append(("dispatch", value.value))
            return (rebuild_reduced, (value.value + 100,))


        dispatch_stream = io.BytesIO()
        dispatch_pickler = pickle.Pickler(dispatch_stream, protocol=4)
        dispatch_pickler.dispatch_table = {Dispatch: reduce_dispatch}
        events.clear()
        dispatch_pickler.dump(Dispatch(3))
        dispatch_events = tuple(events)
        restored = pickle.loads(dispatch_stream.getvalue())
        print("dispatch", describe(restored), dispatch_events, tuple(events))

        class Override:
            def __init__(self, value):
                self.value = value


        class OverridePickler(pickle.Pickler):
            def reducer_override(self, value):
                if isinstance(value, Override):
                    events.append(("override", value.value))
                    return (rebuild_reduced, (value.value + 200,))
                return NotImplemented


        override_stream = io.BytesIO()
        events.clear()
        OverridePickler(override_stream, protocol=4).dump(Override(4))
        override_events = tuple(events)
        restored = pickle.loads(override_stream.getvalue())
        print("override", describe(restored), override_events, tuple(events))

        def outcome(call):
            try:
                return ("return", call())
            except BaseException as exc:
                return ("raise", type(exc).__name__, str(exc))


        class BadReduce:
            def __reduce_ex__(self, protocol):
                return ("not-a-callable",)


        class ReducerError(Exception):
            pass


        class RaisingReduce:
            def __reduce_ex__(self, protocol):
                raise ReducerError("reducer failed")


        print("bad_reduce", outcome(lambda: pickle.dumps(BadReduce(), protocol=4)))
        print("raising_reduce", outcome(lambda: pickle.dumps(RaisingReduce(), protocol=4)))
        """
    )


def test_pickle_persistent_id_and_load_callbacks_match_cpython() -> None:
    _compatible(
        """
        import io
        import pickle


        events = []


        class External:
            def __init__(self, name):
                self.name = name


        class PersistentPickler(pickle.Pickler):
            def persistent_id(self, value):
                if isinstance(value, External):
                    events.append(("persistent_id", value.name))
                    return ("external", value.name)
                return None


        class PersistentUnpickler(pickle.Unpickler):
            def persistent_load(self, pid):
                events.append(("persistent_load", pid))
                return External("loaded:" + pid[1])


        stream = io.BytesIO()
        PersistentPickler(stream, protocol=4).dump(
            {"external": External("alpha"), "ordinary": [1, 2, 3]}
        )
        restored = PersistentUnpickler(io.BytesIO(stream.getvalue())).load()
        print(
            "callbacks",
            sorted(restored),
            restored["external"].name,
            restored["ordinary"],
            events,
        )

        def outcome(call):
            try:
                return ("return", call())
            except BaseException as exc:
                return ("raise", type(exc).__name__, str(exc))


        class RaisingPersistentPickler(pickle.Pickler):
            def persistent_id(self, value):
                if isinstance(value, External):
                    raise RuntimeError("persistent id failed")
                return None


        class RaisingPersistentUnpickler(pickle.Unpickler):
            def persistent_load(self, pid):
                raise RuntimeError("persistent load failed:" + pid[1])


        print(
            "persistent_id_error",
            outcome(
                lambda: RaisingPersistentPickler(io.BytesIO(), protocol=4).dump(
                    External("beta")
                )
            ),
        )
        print(
            "persistent_load_error",
            outcome(
                lambda: RaisingPersistentUnpickler(io.BytesIO(stream.getvalue())).load()
            ),
        )
        """
    )


def test_pickle_protocol_five_buffers_and_picklebuffer_match_cpython() -> None:
    _compatible(
        """
        import io
        import pickle
        import _pickle


        class BufferValue:
            def __init__(self, value):
                self.value = bytes(value)

            def __reduce_ex__(self, protocol):
                if protocol < 5:
                    return (type(self), (self.value,))
                return (type(self), (pickle.PickleBuffer(self.value),))


        def describe_buffer(buffer):
            raw = buffer.raw()
            return (
                type(buffer).__module__,
                type(buffer).__name__,
                raw.format,
                raw.itemsize,
                raw.ndim,
                raw.shape,
                raw.readonly,
                raw.tobytes(),
            )


        def round_trip(dumps, loads, label):
            buffers = []
            observed = []

            def callback(buffer):
                observed.append(describe_buffer(buffer))
                buffers.append(buffer)
                return None

            payload = dumps(BufferValue(b"buffer-value"), protocol=5, buffer_callback=callback)
            restored = loads(payload, buffers=iter(buffers))
            print(label, "payload", repr(payload), "buffers", observed, "value", restored.value)


        round_trip(pickle.dumps, pickle.loads, "pickle")
        round_trip(_pickle.dumps, _pickle.loads, "_pickle")

        stream = io.BytesIO()
        buffers = []
        pickle.Pickler(stream, protocol=5, buffer_callback=buffers.append).dump(
            BufferValue(b"stream-buffer")
        )
        restored = _pickle.Unpickler(io.BytesIO(stream.getvalue()), buffers=iter(buffers)).load()
        print("stream", repr(stream.getvalue()), len(buffers), restored.value)

        def outcome(call):
            try:
                return ("return", call())
            except BaseException as exc:
                return ("raise", type(exc).__name__, str(exc))


        print(
            "protocol_four_callback",
            outcome(lambda: pickle.dumps(BufferValue(b"x"), protocol=4, buffer_callback=lambda _: None)),
        )
        payload = pickle.dumps(BufferValue(b"missing"), protocol=5, buffer_callback=lambda _: None)
        print("missing_buffers", outcome(lambda: pickle.loads(payload)))

        buffer = pickle.PickleBuffer(bytearray(b"mutable"))
        print("buffer_before_release", describe_buffer(buffer), outcome(buffer.release))
        print("buffer_after_release", outcome(buffer.raw))
        """
    )


def test_pickle_stream_methods_and_partial_io_match_cpython() -> None:
    _compatible(
        """
        import io
        import pickle


        class Writer:
            def __init__(self):
                self.chunks = []
                self.data = []

            def write(self, value):
                self.chunks.append((type(value).__name__, len(value), value[:1]))
                self.data.append(bytes(value))
                return 12345


        class Reader:
            def __init__(self, value):
                self.value = value
                self.position = 0
                self.calls = []

            def read(self, size=-1):
                self.calls.append(("read", size))
                if size < 0:
                    size = len(self.value) - self.position
                result = self.value[self.position : self.position + size]
                self.position += len(result)
                return result

            def readline(self):
                self.calls.append(("readline",))
                end = self.value.find(b"\\n", self.position) + 1
                if end == 0:
                    end = len(self.value)
                result = self.value[self.position:end]
                self.position = end
                return result


        value = {"key": [1, 2, 3], "nested": (True, None)}
        writer = Writer()
        result = pickle.Pickler(writer, protocol=4).dump(value)
        payload = b"".join(writer.data)
        print(
            "writer",
            result,
            len(writer.chunks),
            writer.chunks[:4],
            len(payload),
            pickle.loads(payload),
        )

        reader = Reader(pickle.dumps(value, protocol=4))
        restored = pickle.Unpickler(reader).load()
        print("reader", restored, reader.position, reader.calls)

        joined = pickle.dumps("first", protocol=4) + pickle.dumps("second", protocol=4)
        reader = Reader(joined)
        unpickler = pickle.Unpickler(reader)
        print("sequential", unpickler.load(), unpickler.load(), reader.position)

        protocol_zero = Reader(pickle.dumps(value, protocol=0))
        print(
            "readline",
            pickle.Unpickler(protocol_zero).load(),
            [call for call in protocol_zero.calls if call[0] == "readline"],
        )

        def outcome(call):
            try:
                return ("return", call())
            except BaseException as exc:
                return ("raise", type(exc).__name__, str(exc))


        class FailingWriter:
            def write(self, value):
                raise OSError("write failed")


        class MissingWriter:
            pass


        class MissingReader:
            pass


        class ShortReader(Reader):
            def read(self, size=-1):
                return super().read(min(size, 1) if size >= 0 else size)


        print("write_error", outcome(lambda: pickle.dump(value, FailingWriter())))
        print("missing_write", outcome(lambda: pickle.dump(value, MissingWriter())))
        print("missing_read", outcome(lambda: pickle.load(MissingReader())))
        short_reader = ShortReader(pickle.dumps(value, protocol=4))
        print("short_read", outcome(lambda: pickle.load(short_reader)), short_reader.calls)
        print("bad_reader", outcome(lambda: pickle.loads(b"not-a-pickle")))
        """
    )


def test_pickle_argument_errors_and_version_contracts_match_cpython() -> None:
    _compatible(
        """
        import pickle
        import sys
        import _pickle


        def outcome(call):
            try:
                value = call()
            except BaseException as exc:
                return ("raise", type(exc).__name__, str(exc))
            return ("return", type(value).__name__, repr(value))


        for name, operation in (
            ("pickle.dumps_missing", lambda: pickle.dumps()),
            ("pickle.dumps_unknown", lambda: pickle.dumps(None, unknown=True)),
            ("pickle.loads_missing", lambda: pickle.loads()),
            ("pickle.loads_text", lambda: pickle.loads("not-bytes")),
            ("pickle.dump_missing", lambda: pickle.dump(None)),
            ("pickle.load_missing", lambda: pickle.load()),
            ("pickle.dumps_bad_protocol", lambda: pickle.dumps(None, protocol=6)),
            ("pickle.dumps_bad_protocol_type", lambda: pickle.dumps(None, protocol="4")),
            (
                "pickle.dumps_bad_callback",
                lambda: pickle.dumps(pickle.PickleBuffer(b"x"), protocol=5, buffer_callback=1),
            ),
            ("_pickle.dumps_unknown", lambda: _pickle.dumps(None, unknown=True)),
            ("_pickle.loads_unknown", lambda: _pickle.loads(b"N.", unknown=True)),
        ):
            print(name, outcome(operation))

        print(
            "protocol_contract",
            sys.version_info[:3],
            pickle.DEFAULT_PROTOCOL,
            pickle.HIGHEST_PROTOCOL,
            pickle.DEFAULT_PROTOCOL == (3 if sys.version_info < (3, 8) else 4),
            pickle.HIGHEST_PROTOCOL >= 5,
            hasattr(pickle, "PickleBuffer"),
        )
        if sys.version_info >= (3, 8):
            print(
                "protocol_five_contract",
                outcome(lambda: pickle.dumps(None, protocol=5)),
                outcome(lambda: pickle.dumps(None, protocol=5, buffer_callback=lambda _: None)),
                outcome(lambda: pickle.loads(pickle.dumps(None, protocol=5), buffers=())),
            )
        else:
            print("protocol_five_contract", "unavailable")

        """
    )
