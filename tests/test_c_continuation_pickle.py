"""Strict subprocess-isolated multi-shot continuation tests for pickle."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import io
import pickle
from pathlib import Path
import subprocess
import sys
from typing import Any, cast

import pytest

from aleff import create_async_handler, create_handler, effect


Choose = Callable[[], Any]
Case = Callable[[], None]
Operation = Callable[[Choose], Any]
Outcome = tuple[str, Any]
_CASES: dict[str, Case] = {}
_active_callback: Choose | None = None


class ExpectedCallbackError(Exception):
    """An exception used to verify callback-exception restoration."""


def _case(name: str) -> Callable[[Case], Case]:
    def register(test_case: Case) -> Case:
        _CASES[name] = test_case
        return test_case

    return register


def _outcome(run: Callable[[], Any]) -> Outcome:
    try:
        return "return", run()
    except Exception as exc:
        return "raise", (type(exc).__name__, str(exc))


def _callback_value() -> Any:
    if _active_callback is None:
        raise AssertionError("pickle callback was used outside a test operation")
    value = _active_callback()
    if value == "raise":
        raise ExpectedCallbackError("pickle callback failed")
    return value


def _with_callback(callback: Choose, operation: Callable[[], Any]) -> Any:
    global _active_callback
    previous = _active_callback
    _active_callback = callback
    try:
        return operation()
    finally:
        _active_callback = previous


def _resume_against_fresh(
    run: Operation,
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
    effect_name: str,
) -> list[Outcome]:
    choose = effect(effect_name)
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        outcomes: list[Outcome] = []
        for decision in decisions:
            expected = _outcome(lambda decision=decision: fresh(decision))
            actual = _outcome(lambda decision=decision: k(decision))
            assert actual == expected, (
                f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            )
            outcomes.append(actual)
        return outcomes

    outcomes = handler(lambda: run(choose))
    assert suspension_count == 1, "the scenario must suspend at exactly one callback"
    return cast(list[Outcome], outcomes)


def _assert_normal_error_corner(outcomes: list[Outcome]) -> None:
    assert outcomes[0][0] == "return", outcomes
    assert outcomes[1][0] == "return", outcomes
    assert outcomes[0] == outcomes[-1], outcomes
    assert outcomes[0] != outcomes[1], outcomes
    assert outcomes[2][0] == "raise", outcomes
    error = outcomes[2][1]
    assert isinstance(error, tuple) and error[0] == "ExpectedCallbackError", outcomes


def _rebuild_pair(value: Any) -> tuple[str, Any]:
    return "reduced", value


class _ReducePayload:
    def __reduce_ex__(self, protocol: Any) -> Any:
        del protocol
        return _rebuild_pair, (_callback_value(),)


def _reduce_operation(operation: str, callback: Choose, protocol: int = 4) -> Any:
    payload = _ReducePayload()

    def execute() -> Any:
        if operation == "dumps":
            return pickle.dumps(payload, protocol=protocol)
        if operation == "dump":
            stream = io.BytesIO()

            def reset_stream() -> Any:
                decision = callback()
                stream.seek(0)
                stream.truncate()
                return decision

            result = _with_callback(
                reset_stream,
                lambda: pickle.dump(payload, stream, protocol=protocol),
            )
            return result, stream.getvalue()
        raise AssertionError(operation)

    if operation == "dump":
        return execute()
    return _with_callback(callback, execute)


@_case("dumps_reduce_ex_multishot")
def _dumps_reduce_ex_multishot() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _reduce_operation("dumps", choose),
        lambda decision: _reduce_operation("dumps", lambda: decision),
        ("first", "second", "raise", "first"),
        "pickle-reduce-ex",
    )
    _assert_normal_error_corner(outcomes)


@_case("dumps_reduce_ex_protocol_zero_multishot")
def _dumps_reduce_ex_protocol_zero_multishot() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _reduce_operation("dumps", choose, protocol=0),
        lambda decision: _reduce_operation("dumps", lambda: decision, protocol=0),
        ("ascii", "unicode-✓", "raise", "ascii"),
        "pickle-reduce-ex-protocol-zero",
    )
    _assert_normal_error_corner(outcomes)


@_case("dump_reduce_ex_multishot")
def _dump_reduce_ex_multishot() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _reduce_operation("dump", choose),
        lambda decision: _reduce_operation("dump", lambda: decision),
        ("first", "second", "raise", "first"),
        "pickle-dump-reduce-ex",
    )
    _assert_normal_error_corner(outcomes)


class _ReducerOverridePayload:
    pass


class _ReducerOverridePickler(pickle.Pickler):
    def reducer_override(self, obj: Any) -> Any:  # pyright: ignore[reportIncompatibleMethodOverride]
        if isinstance(obj, _ReducerOverridePayload):
            return _rebuild_pair, (_callback_value(),)
        return NotImplemented


def _pickler_dump_operation(callback: Choose) -> tuple[None, bytes]:
    stream = io.BytesIO()

    def reset_stream() -> Any:
        decision = callback()
        stream.seek(0)
        stream.truncate()
        return decision

    result = _with_callback(
        reset_stream,
        lambda: _ReducerOverridePickler(stream, protocol=4).dump(_ReducerOverridePayload()),
    )
    return result, stream.getvalue()


@_case("pickler_dump_reducer_override_multishot")
def _pickler_dump_reducer_override_multishot() -> None:
    outcomes = _resume_against_fresh(
        _pickler_dump_operation,
        lambda decision: _pickler_dump_operation(lambda: decision),
        ("first", "second", "raise", "first"),
        "pickle-reducer-override",
    )
    _assert_normal_error_corner(outcomes)


class _DispatchPayload:
    pass


def _pickler_dump_dispatch_table_operation(callback: Choose) -> tuple[None, bytes]:
    stream = io.BytesIO()

    def reduce_dispatch(_value: _DispatchPayload) -> Any:
        decision = _callback_value()
        stream.seek(0)
        stream.truncate()
        return _rebuild_pair, (decision,)

    pickler = pickle.Pickler(stream, protocol=4)
    cast(Any, pickler).dispatch_table = {_DispatchPayload: reduce_dispatch}
    result = _with_callback(callback, lambda: pickler.dump(_DispatchPayload()))
    return result, stream.getvalue()


@_case("pickler_dump_dispatch_table_reducer_multishot")
def _pickler_dump_dispatch_table_reducer_multishot() -> None:
    outcomes = _resume_against_fresh(
        _pickler_dump_dispatch_table_operation,
        lambda decision: _pickler_dump_dispatch_table_operation(lambda: decision),
        ("dispatch-a", None, "raise", "dispatch-a"),
        "pickle-dispatch-table-reducer",
    )
    _assert_normal_error_corner(outcomes)


class _ReduceFallbackPayload:
    def __reduce__(self) -> Any:
        return _rebuild_pair, (_callback_value(),)


@_case("dumps_reduce_fallback_multishot")
def _dumps_reduce_fallback_multishot() -> None:
    def operation(callback: Choose) -> bytes:
        return _with_callback(callback, lambda: pickle.dumps(_ReduceFallbackPayload(), protocol=4))

    outcomes = _resume_against_fresh(
        operation,
        lambda decision: operation(lambda: decision),
        ("reduce-a", None, "raise", "reduce-a"),
        "pickle-reduce-fallback",
    )
    _assert_normal_error_corner(outcomes)


class _GetStatePayload:
    def __getstate__(self) -> dict[str, Any]:
        return {"value": _callback_value()}


@_case("dumps_getstate_multishot")
def _dumps_getstate_multishot() -> None:
    def operation(callback: Choose) -> bytes:
        return _with_callback(callback, lambda: pickle.dumps(_GetStatePayload(), protocol=4))

    outcomes = _resume_against_fresh(
        operation,
        lambda decision: operation(lambda: decision),
        ("state-a", "state-b", "raise", "state-a"),
        "pickle-getstate",
    )
    _assert_normal_error_corner(outcomes)


class _SetStatePayload:
    def __init__(self, value: Any = "initial") -> None:
        self.value = value

    def __getstate__(self) -> dict[str, Any]:
        return {"value": "wire-value"}

    def __setstate__(self, state: dict[str, Any]) -> None:
        self.value = (_callback_value(), state["value"])


def _setstate_data() -> bytes:
    return pickle.dumps(_SetStatePayload(), protocol=4)


def _loads_setstate_operation(callback: Choose) -> Any:
    value = _with_callback(callback, lambda: pickle.loads(_setstate_data()))
    assert isinstance(value, _SetStatePayload)
    return value.value


@_case("loads_setstate_multishot")
def _loads_setstate_multishot() -> None:
    outcomes = _resume_against_fresh(
        _loads_setstate_operation,
        lambda decision: _loads_setstate_operation(lambda: decision),
        ("loaded-a", "loaded-b", "raise", "loaded-a"),
        "pickle-setstate",
    )
    _assert_normal_error_corner(outcomes)


def _load_reduce_rebuild(wire_value: str) -> tuple[str, str, Any]:
    return "reduce-load", wire_value, _callback_value()


class _LoadReducePayload:
    def __reduce__(self) -> Any:
        return _load_reduce_rebuild, ("wire",)


_LOAD_REDUCE_DATA = pickle.dumps(_LoadReducePayload(), protocol=4)


@_case("loads_reduce_callable_multishot")
def _loads_reduce_callable_multishot() -> None:
    def operation(callback: Choose) -> Any:
        return _with_callback(callback, lambda: pickle.loads(_LOAD_REDUCE_DATA))

    outcomes = _resume_against_fresh(
        operation,
        lambda decision: operation(lambda: decision),
        ("rebuild-a", None, "raise", "rebuild-a"),
        "pickle-load-reduce-callable",
    )
    _assert_normal_error_corner(outcomes)


class _LoadNewPayload:
    new_value: tuple[Any, str]
    value: str

    def __new__(cls, value: str = "initial") -> "_LoadNewPayload":
        instance = super().__new__(cls)
        decision = _callback_value() if _active_callback is not None else "data-build"
        instance.new_value = (decision, value)
        return instance

    def __init__(self, value: str = "initial") -> None:
        self.value = value

    def __getnewargs__(self) -> tuple[str]:
        return (self.value,)

    def __getstate__(self) -> dict[str, str]:
        return {"value": self.value}

    def __setstate__(self, state: dict[str, str]) -> None:
        self.value = state["value"]


_LOAD_NEW_DATA = pickle.dumps(_LoadNewPayload("wire"), protocol=4)


@_case("loads_custom_new_multishot")
def _loads_custom_new_multishot() -> None:
    def operation(callback: Choose) -> Any:
        value = _with_callback(callback, lambda: pickle.loads(_LOAD_NEW_DATA))
        assert isinstance(value, _LoadNewPayload)
        return value.new_value

    outcomes = _resume_against_fresh(
        operation,
        lambda decision: operation(lambda: decision),
        ("new-a", None, "raise", "new-a"),
        "pickle-load-custom-new",
    )
    _assert_normal_error_corner(outcomes)


class _PersistentPayload:
    def __init__(self, value: str) -> None:
        self.value = value


class _PersistentPickler(pickle.Pickler):
    def persistent_id(self, obj: Any) -> Any:
        if isinstance(obj, _PersistentPayload):
            return "persistent:" + str(_callback_value())
        return None


def _persistent_dump_operation(callback: Choose) -> bytes:
    stream = io.BytesIO()

    def reset_stream() -> Any:
        decision = callback()
        stream.seek(0)
        stream.truncate()
        return decision

    _with_callback(
        reset_stream,
        lambda: _PersistentPickler(stream, protocol=4).dump(
            _PersistentPayload("wire")
        ),
    )
    return stream.getvalue()


@_case("pickler_dump_persistent_id_multishot")
def _pickler_dump_persistent_id_multishot() -> None:
    outcomes = _resume_against_fresh(
        _persistent_dump_operation,
        lambda decision: _persistent_dump_operation(lambda: decision),
        ("id-a", "id-b", "raise", "id-a"),
        "pickle-persistent-id",
    )
    _assert_normal_error_corner(outcomes)


class _PersistentLoadUnpickler(pickle.Unpickler):
    def persistent_load(self, pid: Any) -> Any:
        return {"pid": pid, "decision": _callback_value()}


def _persistent_load_data() -> bytes:
    stream = io.BytesIO()

    class FixedPersistentPickler(pickle.Pickler):
        def persistent_id(self, obj: Any) -> Any:
            if isinstance(obj, _PersistentPayload):
                return "persistent:wire"
            return None

    FixedPersistentPickler(stream, protocol=4).dump(_PersistentPayload("wire"))
    return stream.getvalue()


def _unpickler_persistent_load_operation(callback: Choose) -> Any:
    stream = io.BytesIO(_persistent_load_data())
    unpickler = _PersistentLoadUnpickler(stream)
    return _with_callback(callback, unpickler.load)


@_case("unpickler_load_persistent_load_multishot")
def _unpickler_load_persistent_load_multishot() -> None:
    outcomes = _resume_against_fresh(
        _unpickler_persistent_load_operation,
        lambda decision: _unpickler_persistent_load_operation(lambda: decision),
        ("load-a", "load-b", "raise", "load-a"),
        "pickle-persistent-load",
    )
    _assert_normal_error_corner(outcomes)


class _FindClassPayload:
    def __init__(self, value: str) -> None:
        self.value = value


class _FindClassUnpickler(pickle.Unpickler):
    def __init__(self, file: Any, callback: Choose) -> None:
        super().__init__(file)
        self.callback = callback
        self.decisions: list[Any] = []

    def find_class(self, module: str, name: str) -> Any:
        decision = self.callback()
        self.decisions.clear()
        if decision == "raise":
            raise ExpectedCallbackError("find_class failed")
        self.decisions.append(decision)
        return super().find_class(module, name)


def _find_class_data() -> bytes:
    return pickle.dumps(_FindClassPayload("wire"), protocol=4)


def _unpickler_find_class_operation(callback: Choose) -> tuple[Any, tuple[Any, ...]]:
    unpickler = _FindClassUnpickler(io.BytesIO(_find_class_data()), callback)
    value = unpickler.load()
    return value.value, tuple(unpickler.decisions)


@_case("unpickler_load_find_class_multishot")
def _unpickler_load_find_class_multishot() -> None:
    outcomes = _resume_against_fresh(
        _unpickler_find_class_operation,
        lambda decision: _unpickler_find_class_operation(lambda: decision),
        ("class-a", "class-b", "raise", "class-a"),
        "pickle-find-class",
    )
    _assert_normal_error_corner(outcomes)


def _buffer_dump_operation(callback: Choose) -> tuple[bytes, tuple[bytes, ...]]:
    seen: list[bytes] = []

    def buffer_callback(buffer: pickle.PickleBuffer) -> bool:
        seen.append(buffer.raw().tobytes())
        decision = _callback_value()
        seen[:] = [buffer.raw().tobytes()]
        if decision not in ("in-band", "out-of-band"):
            raise AssertionError(decision)
        return decision == "in-band"

    encoded = _with_callback(
        callback,
        lambda: pickle.dumps(pickle.PickleBuffer(b"buffer-wire"), protocol=5, buffer_callback=buffer_callback),
    )
    return encoded, tuple(seen)


@_case("dumps_buffer_callback_multishot")
def _dumps_buffer_callback_multishot() -> None:
    outcomes = _resume_against_fresh(
        _buffer_dump_operation,
        lambda decision: _buffer_dump_operation(lambda: decision),
        ("in-band", "out-of-band", "raise", "in-band"),
        "pickle-buffer-callback",
    )
    _assert_normal_error_corner(outcomes)


def _out_of_band_buffer_data() -> bytes:
    return pickle.dumps(
        pickle.PickleBuffer(b"buffer-wire"),
        protocol=5,
        buffer_callback=lambda buffer: False,
    )


def _loads_buffers_operation(callback: Choose) -> bytes:
    class Buffers:
        def __iter__(self) -> Any:
            return self

        def __next__(self) -> memoryview:
            decision = _callback_value()
            if not isinstance(decision, str):
                raise AssertionError(decision)
            return memoryview(decision.encode())

    value = _with_callback(
        callback,
        lambda: pickle.loads(_out_of_band_buffer_data(), buffers=Buffers()),
    )
    assert isinstance(value, memoryview)
    return value.tobytes()


@_case("loads_buffers_iterator_multishot")
def _loads_buffers_iterator_multishot() -> None:
    outcomes = _resume_against_fresh(
        _loads_buffers_operation,
        lambda decision: _loads_buffers_operation(lambda: decision),
        ("buffer-a", "buffer-b", "raise", "buffer-a"),
        "pickle-buffers",
    )
    _assert_normal_error_corner(outcomes)


def _dump_write_operation(callback: Choose) -> tuple[None, bytes, tuple[Any, ...]]:
    stream = io.BytesIO()
    events: list[Any] = []

    class Writer:
        def write(self, data: bytes) -> int:
            if not events:
                decision = _callback_value()
                stream.seek(0)
                stream.truncate()
                events[:] = [decision]
            return stream.write(data)

    result = _with_callback(
        callback,
        lambda: pickle.dump({"stream": [1, 2, 3]}, Writer(), protocol=4),
    )
    return result, stream.getvalue(), tuple(events)


@_case("dump_stream_write_multishot")
def _dump_stream_write_multishot() -> None:
    outcomes = _resume_against_fresh(
        _dump_write_operation,
        lambda decision: _dump_write_operation(lambda: decision),
        ("write-a", "write-b", "raise", "write-a"),
        "pickle-stream-write",
    )
    _assert_normal_error_corner(outcomes)


def _load_stream_operation(method: str, callback: Choose) -> tuple[Any, tuple[Any, ...], tuple[Any, ...]]:
    source = io.BytesIO(pickle.dumps({"stream": [1, 2, 3]}, protocol=0))
    events: list[Any] = []
    calls: list[tuple[str, int]] = []

    class Reader:
        def read(self, size: int = -1) -> bytes:
            calls.append(("read", size))
            if method == "read" and not events:
                decision = _callback_value()
                source.seek(0)
                events[:] = [decision]
                calls[:] = [("read", size)]
            return source.read(size)

        def readline(self) -> bytes:
            calls.append(("readline", -1))
            if method == "readline" and not events:
                decision = _callback_value()
                source.seek(0)
                events[:] = [decision]
                calls[:] = [("readline", -1)]
            return source.readline()

    result = _with_callback(callback, lambda: pickle.load(Reader()))
    return result, tuple(events), tuple(calls)


@_case("load_stream_read_multishot")
def _load_stream_read_multishot() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _load_stream_operation("read", choose),
        lambda decision: _load_stream_operation("read", lambda: decision),
        ("read-a", "read-b", "raise", "read-a"),
        "pickle-stream-read",
    )
    _assert_normal_error_corner(outcomes)


@_case("load_stream_readline_multishot")
def _load_stream_readline_multishot() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _load_stream_operation("readline", choose),
        lambda decision: _load_stream_operation("readline", lambda: decision),
        ("line-a", "line-b", "raise", "line-a"),
        "pickle-stream-readline",
    )
    _assert_normal_error_corner(outcomes)


async def _async_outcome(awaitable: Any) -> Outcome:
    try:
        value = await awaitable
    except Exception as exc:
        return "raise", (type(exc).__name__, str(exc))
    return "return", value


@_case("dumps_reduce_ex_async_multishot")
def _dumps_reduce_ex_async_multishot() -> None:
    async def exercise() -> list[Outcome]:
        choose = effect("pickle-async-reduce-ex")
        handler = create_async_handler(choose)

        async def run() -> bytes:
            return _reduce_operation("dumps", lambda: choose())

        @handler.on(choose)
        async def resume(k: Any) -> list[Outcome]:
            outcomes: list[Outcome] = []
            for decision in ("async-a", "async-b", "raise", "async-a"):
                expected = _outcome(
                    lambda decision=decision: _reduce_operation("dumps", lambda: decision)
                )
                actual = await _async_outcome(k(decision))
                assert actual == expected, (
                    f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
                )
                outcomes.append(actual)
            return outcomes

        return cast(list[Outcome], await handler(run))

    outcomes = asyncio.run(exercise())
    _assert_normal_error_corner(outcomes)


def _run_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", name],
        text=True,
        capture_output=True,
        timeout=20,
        check=False,
    )


@pytest.mark.parametrize("case_name", list(_CASES))
def test_pickle_continuation(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_pickle.py --case CASE")
    _CASES[sys.argv[2]]()
