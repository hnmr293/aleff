"""Tests for unsupported C-boundary monitoring."""

import asyncio
import os
import bisect
import importlib
import itertools
import subprocess
import sys
import time
import warnings
from collections import deque
from collections.abc import Callable, Iterable, Iterator
from functools import partial
from pathlib import Path
from typing import Any, Literal, cast

import greenlet as gl
import pytest

from aleff import (
    CFrameContinuationWarning,
    c_warnings_enabled,
    create_async_handler,
    create_handler,
    disable_c_warnings,
    effect,
    enable_c_warnings,
)


ROOT = Path(__file__).parents[1]


@pytest.fixture(autouse=True)
def _restore_monitoring() -> Iterator[None]:
    enable_c_warnings()
    yield
    disable_c_warnings()
    enable_c_warnings()


def test_c_boundary_warnings_are_enabled_by_default() -> None:
    assert c_warnings_enabled() is True
    assert any(sys.monitoring.get_tool(tool_id) == "aleff.c-boundary-warnings" for tool_id in (3, 4))


def test_boundary_freeze_is_scoped_to_the_adapter_lifecycle(monkeypatch: pytest.MonkeyPatch) -> None:
    effects_module = importlib.import_module("aleff._multishot.v1.effects")
    suspend = effect("suspend")
    adapter_token = object()
    events: list[object] = []

    def suspend_adapters() -> object:
        events.append("suspend")
        return adapter_token

    def freeze_boundaries() -> tuple[()]:
        events.append("freeze")
        return ()

    def restore_adapters(token: object) -> None:
        events.extend(("restore", token))

    monkeypatch.setattr(effects_module, "_suspend_adapters", suspend_adapters)
    monkeypatch.setattr(effects_module, "_freeze_unsupported_boundaries", freeze_boundaries)
    monkeypatch.setattr(effects_module, "_restore_adapters", restore_adapters)

    caller = gl.greenlet(suspend)
    context = caller.switch()
    assert context.effect is suspend
    assert caller.switch("handled") == "handled"
    assert events == ["suspend", "freeze", "restore", adapter_token]


def test_boundary_freeze_failure_restores_the_acquired_adapter_token(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    effects_module = importlib.import_module("aleff._multishot.v1.effects")
    suspend = effect("suspend")
    adapter_token = object()
    restored: list[object] = []

    monkeypatch.setattr(effects_module, "_suspend_adapters", lambda: adapter_token)

    def fail_freeze() -> tuple[()]:
        raise MemoryError("injected boundary freeze failure")

    monkeypatch.setattr(effects_module, "_freeze_unsupported_boundaries", fail_freeze)
    monkeypatch.setattr(effects_module, "_restore_adapters", restored.append)

    caller = gl.greenlet(suspend)
    with pytest.raises(MemoryError, match="injected boundary freeze failure"):
        caller.switch()

    assert restored == [adapter_token]


def test_boundary_freeze_failure_does_not_corrupt_a_following_adapter_snapshot(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    effects_module = importlib.import_module("aleff._multishot.v1.effects")
    choose = effect("choose")
    handler = create_handler(choose)
    original_freeze = effects_module._freeze_unsupported_boundaries
    fail_next = True

    @handler.on(choose)
    def choose_twice(resume: Callable[[int], list[int]]) -> list[list[int]]:
        return [resume(1), resume(2)]

    def freeze_boundaries() -> object:
        nonlocal fail_next
        if fail_next:
            fail_next = False
            raise MemoryError("injected boundary freeze failure")
        return original_freeze()

    def callback(_value: object) -> int:
        try:
            choose()
        except MemoryError:
            pass
        return choose()

    monkeypatch.setattr(effects_module, "_freeze_unsupported_boundaries", freeze_boundaries)

    assert handler(lambda: list(map(callback, [None]))) == [[1], [2]]


def test_completed_c_call_before_snapshot_does_not_warn() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    def caller() -> str:
        time.time()
        return suspend()

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(caller) == "handled"

    assert caught == []


def test_c_call_that_raises_before_snapshot_does_not_warn() -> None:
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        with pytest.raises(ValueError):
            create_handler()(lambda: time.sleep(-1))

    assert caught == []


def test_default_monitoring_does_not_change_recursion_limit_behavior() -> None:
    def count_recursive_comparisons() -> int:
        calls = 0

        class Item:
            def __lt__(self, other: Any) -> bool:
                nonlocal calls
                calls += 1
                return bisect.bisect_left([self], other) == 0

        try:
            bisect.bisect_left([Item()], Item())
        except RecursionError:
            return calls
        raise AssertionError("recursive comparison unexpectedly completed")

    previous_limit = sys.getrecursionlimit()
    try:
        sys.setrecursionlimit(80)
        disable_c_warnings()
        without_monitoring = count_recursive_comparisons()
        enable_c_warnings()
        with_monitoring = count_recursive_comparisons()
    finally:
        sys.setrecursionlimit(previous_limit)

    assert with_monitoring == without_monitoring


def test_default_monitoring_does_not_invoke_user_metaclass_hooks() -> None:
    events: list[str] = []

    class Meta(type):
        def __getattribute__(cls, name: str) -> object:
            events.append(name)
            return super().__getattribute__(name)

    class Target(metaclass=Meta):
        pass

    bool(Target())

    assert events == []


def test_python_backed_callable_object_is_not_a_c_boundary() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    class Callback:
        def __call__(self) -> object:
            return suspend()

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: Callback()()) == "handled"

    assert caught == []


@pytest.mark.parametrize("wrapper_depth", [1, 2], ids=["static", "nested-static"])
def test_staticmethod_wrapped_unsupported_c_callable_warns(wrapper_depth: int) -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)
    deque_factory = cast(Callable[[Iterable[None]], deque[None]], deque)

    class StaticCallback:
        __call__ = staticmethod(deque_factory)

    class NestedStaticCallback:
        __call__ = staticmethod(staticmethod(deque_factory))

    class Items:
        def __iter__(self) -> Iterator[None]:
            suspend()
            yield None

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    callback_type = StaticCallback if wrapper_depth == 1 else NestedStaticCallback
    callback = cast(Callable[[Iterable[None]], object], callback_type())
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: callback(Items())) == "handled"

    assert len(caught) == 1
    assert caught[0].category is CFrameContinuationWarning


def test_classmethod_wrapped_unsupported_c_callable_warns() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)
    deque_from_class = cast(Callable[[type[object]], deque[object]], deque)

    class Meta(type):
        def __iter__(cls) -> Iterator[None]:
            suspend()
            yield None

    class Callback(metaclass=Meta):
        __call__ = classmethod(deque_from_class)

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    callback = cast(Callable[[], object], Callback())
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: callback()) == "handled"

    assert len(caught) == 1
    assert caught[0].category is CFrameContinuationWarning


def test_staticmethod_wrapped_native_callable_object_warns() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)
    deque_factory = cast(Callable[[Iterable[None]], deque[None]], deque)
    native = partial(deque_factory)

    class Callback:
        __call__ = staticmethod(native)

    class Items:
        def __iter__(self) -> Iterator[None]:
            suspend()
            yield None

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    callback = cast(Callable[[Iterable[None]], object], Callback())
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: callback(Items())) == "handled"

    assert len(caught) == 1
    assert caught[0].category is CFrameContinuationWarning


def test_staticmethod_wrapped_adapter_backed_callable_does_not_warn() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)
    maximum = cast(Callable[[Iterable[int]], int], max)

    class Callback:
        __call__ = staticmethod(maximum)

    class Items:
        def __iter__(self) -> Iterator[int]:
            suspend()
            yield 1

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    callback = cast(Callable[[Iterable[int]], object], Callback())
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: callback(Items())) == "handled"

    assert caught == []


def test_staticmethod_wrapped_python_callable_object_does_not_warn() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)

    class PythonCallable:
        def __call__(self) -> object:
            return suspend()

    class Callback:
        __call__ = staticmethod(PythonCallable())

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: Callback()()) == "handled"

    assert caught == []


def test_metaclass_staticmethod_wrapped_unsupported_c_callable_warns() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)
    deque_factory = cast(Callable[[Iterable[None]], deque[None]], deque)

    class Meta(type):
        pass

    setattr(Meta, "__call__", staticmethod(deque_factory))

    class Target(metaclass=Meta):
        pass

    class Items:
        def __iter__(self) -> Iterator[None]:
            suspend()
            yield None

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    target = cast(Callable[[Iterable[None]], object], Target)
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: target(Items())) == "handled"

    assert len(caught) == 1
    assert caught[0].category is CFrameContinuationWarning
    expected_name = f"{Target.__module__}.{Target.__qualname__}"
    assert str(caught[0].message).startswith(f"{expected_name} is an unsupported C boundary")


def test_warning_name_preserves_builtin_callable_names() -> None:
    monitoring = importlib.import_module("aleff._multishot.v1.monitoring")

    assert monitoring._callable_name(len) == "len"
    assert monitoring._callable_name(getattr(list, "append")) == "list.append"
    assert monitoring._callable_name(partial(deque)) == "functools.partial"


@pytest.mark.parametrize(("action", "warning_count"), [("always", 1), ("ignore", 0)])
def test_warning_name_does_not_invoke_metaclass_hooks(action: Literal["always", "ignore"], warning_count: int) -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)
    hooks: list[str] = []
    deque_factory = cast(Callable[[Iterable[None]], deque[None]], deque)

    class Meta(type):
        def __getattribute__(cls, name: str) -> object:
            if name in {"__objclass__", "__module__", "__qualname__"}:
                hooks.append(name)
                raise LookupError(name)
            return super().__getattribute__(name)

    class Callback(metaclass=Meta):
        __call__ = staticmethod(deque_factory)

    class Items:
        def __iter__(self) -> Iterator[None]:
            suspend()
            yield None

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter(action, CFrameContinuationWarning)
        assert handler(lambda: Callback()(Items())) == "handled"

    assert len(caught) == warning_count
    assert hooks == []


def test_warning_name_does_not_invoke_instance_class_hook() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)
    deque_factory = cast(Callable[[Iterable[None]], deque[None]], deque)

    class Callback:
        __call__ = staticmethod(deque_factory)

        def __getattribute__(self, name: str) -> object:
            if name == "__class__":
                raise LookupError(name)
            return super().__getattribute__(name)

    class Items:
        def __iter__(self) -> Iterator[None]:
            suspend()
            yield None

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: Callback()(Items())) == "handled"

    assert len(caught) == 1


def test_warning_name_rejects_non_string_metadata_without_formatting_it() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)
    deque_factory = cast(Callable[[Iterable[None]], deque[None]], deque)

    class HostileMetadata:
        def __hash__(self) -> int:
            raise LookupError("hash")

        def __format__(self, _format_spec: str) -> str:
            raise LookupError("format")

    class Callback:
        __call__ = staticmethod(deque_factory)

    setattr(Callback, "__module__", HostileMetadata())

    class Items:
        def __iter__(self) -> Iterator[None]:
            suspend()
            yield None

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: Callback()(Items())) == "handled"

    assert len(caught) == 1
    assert str(caught[0].message).startswith(f"{Callback.__qualname__} is an unsupported C boundary")


def test_async_warning_name_does_not_invoke_metaclass_hooks() -> None:
    suspend = effect("suspend")
    handler = create_async_handler(suspend)
    deque_factory = cast(Callable[[Iterable[None]], deque[None]], deque)

    class Meta(type):
        def __getattribute__(cls, name: str) -> object:
            if name in {"__objclass__", "__module__", "__qualname__"}:
                raise LookupError(name)
            return super().__getattribute__(name)

    class Callback(metaclass=Meta):
        __call__ = staticmethod(deque_factory)

    class Items:
        def __iter__(self) -> Iterator[None]:
            suspend()
            yield None

    @handler.on(suspend)
    async def handle(_resume: object) -> str:
        return "handled"

    async def run() -> str:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", CFrameContinuationWarning)
            return await handler(lambda: Callback()(Items()))

    assert asyncio.run(run()) == "handled"


def test_adapter_backed_boundary_active_at_snapshot_does_not_warn() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: list(map(lambda _: suspend(), [None]))) == "handled"

    assert caught == []


@pytest.mark.parametrize("constructor", [itertools.combinations, itertools.product])
def test_adapter_backed_itertools_constructor_does_not_warn(constructor: object) -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)

    class Index:
        def __index__(self) -> int:
            suspend()
            return 1

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        if constructor is itertools.product:
            result = handler(lambda: itertools.product([1], repeat=cast(int, Index())))
        else:
            result = handler(lambda: itertools.combinations([1], cast(int, Index())))

    assert result == "handled"
    assert caught == []


def test_core_restore_and_coroutine_bridge_boundaries_do_not_warn() -> None:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def choose_twice(resume: Callable[[int], int]) -> list[int]:
        return [resume(1), resume(2)]

    async def coroutine() -> int:
        return choose()

    def caller() -> int:
        pending = coroutine()
        try:
            pending.send(None)
        except StopIteration as completed:
            return completed.value
        finally:
            pending.close()
        raise AssertionError("coroutine unexpectedly suspended")

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(caller) == [1, 2]

    assert caught == []


def _call_function_ex(suspend: Callable[[], object]) -> list[object]:
    return list(*(map(lambda _: suspend(), [None]),), **{})


def _call_keyword(suspend: Callable[[], object]) -> None:
    return min([None], key=lambda _: 0 if suspend() is None else 1)


@pytest.mark.parametrize(
    ("caller", "expected"),
    [
        pytest.param(_call_function_ex, [[None], [None]], id="call-function-ex"),
        pytest.param(_call_keyword, [None, None], id="call-keyword"),
    ],
)
def test_instrumented_call_variants_restore_as_their_base_opcode(
    caller: Callable[[Callable[[], object]], object],
    expected: list[object],
) -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)

    @handler.on(suspend)
    def handle(resume: Callable[[None], object]) -> list[object]:
        return [resume(None), resume(None)]

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: caller(suspend)) == expected

    assert caught == []


def test_disabled_monitoring_does_not_warn_for_completed_calls_or_snapshots() -> None:
    suspend = effect("suspend")
    handler = create_handler(suspend)

    @handler.on(suspend)
    def handle(_resume: object) -> str:
        return "handled"

    disable_c_warnings()

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", CFrameContinuationWarning)
        assert handler(lambda: (time.time(), suspend())[1]) == "handled"

    assert caught == []
    assert c_warnings_enabled() is False


def test_enable_and_disable_are_idempotent_and_release_the_tool_id() -> None:
    enable_c_warnings()
    enable_c_warnings()
    claimed = [tool_id for tool_id in (3, 4) if sys.monitoring.get_tool(tool_id) == "aleff.c-boundary-warnings"]
    assert len(claimed) == 1
    assert sys.monitoring.get_events(claimed[0]) == sys.monitoring.events.CALL

    disable_c_warnings()
    disable_c_warnings()

    assert all(sys.monitoring.get_tool(tool_id) != "aleff.c-boundary-warnings" for tool_id in (3, 4))


def test_default_enable_reports_tool_id_exhaustion_without_overwriting_tools() -> None:
    script = """
import sys
import warnings

sys.monitoring.use_tool_id(3, "occupied-three")
sys.monitoring.use_tool_id(4, "occupied-four")
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    import aleff

print(aleff.c_warnings_enabled())
print(sys.monitoring.get_tool(3))
print(sys.monitoring.get_tool(4))
print(type(caught[-1].message).__name__)
print(caught[-1].message)
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert lines[:4] == ["False", "occupied-three", "occupied-four", "RuntimeWarning"]
    assert "no sys.monitoring tool ID is available" in lines[4]


def test_explicit_enable_fails_when_both_tool_ids_are_occupied_and_can_retry() -> None:
    script = """
import sys
import warnings

sys.monitoring.use_tool_id(3, "occupied-three")
sys.monitoring.use_tool_id(4, "occupied-four")
with warnings.catch_warnings():
    warnings.simplefilter("ignore")
    import aleff

try:
    aleff.enable_c_warnings()
except RuntimeError as error:
    print(error)
else:
    raise AssertionError("enable unexpectedly succeeded")

sys.monitoring.free_tool_id(4)
aleff.enable_c_warnings()
print(aleff.c_warnings_enabled())
aleff.disable_c_warnings()
print(sys.monitoring.get_tool(4))
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["no sys.monitoring tool ID is available", "True", "None"]


def test_same_named_tool_is_not_adopted_or_overwritten() -> None:
    script = """
import sys

call_count = 0

def sentinel(code, instruction_offset, target, argument):
    global call_count
    if target is len:
        call_count += 1

sys.monitoring.use_tool_id(3, "aleff.c-boundary-warnings")
sys.monitoring.register_callback(3, sys.monitoring.events.CALL, sentinel)
sys.monitoring.set_events(3, sys.monitoring.events.CALL)

import aleff

call_count = 0
len(())
print(aleff.c_warnings_enabled())
print(sys.monitoring.get_tool(3))
print(sys.monitoring.get_tool(4))
print(call_count)
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "True",
        "aleff.c-boundary-warnings",
        "aleff.c-boundary-warnings",
        "1",
    ]


def test_same_named_tools_exhaust_ids_without_being_overwritten() -> None:
    script = """
import sys
import warnings

sentinels = []
for tool_id in (3, 4):
    sys.monitoring.use_tool_id(tool_id, "aleff.c-boundary-warnings")
    callback = lambda *args, tool_id=tool_id: None
    sentinels.append(callback)
    sys.monitoring.register_callback(tool_id, sys.monitoring.events.CALL, callback)
    sys.monitoring.set_events(tool_id, sys.monitoring.events.CALL)

with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    import aleff

print(aleff.c_warnings_enabled())
print(type(caught[-1].message).__name__)
print(caught[-1].message)
for tool_id, sentinel in zip((3, 4), sentinels, strict=True):
    previous = sys.monitoring.register_callback(tool_id, sys.monitoring.events.CALL, sentinel)
    print(previous is sentinel)
    print(sys.monitoring.get_events(tool_id))
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "False",
        "RuntimeWarning",
        "no sys.monitoring tool ID is available",
        "True",
        str(sys.monitoring.events.CALL),
        "True",
        str(sys.monitoring.events.CALL),
    ]


def test_disable_does_not_release_an_externally_reclaimed_same_named_tool() -> None:
    script = """
import sys
import aleff

tool_id = next(
    tool_id
    for tool_id in (3, 4)
    if sys.monitoring.get_tool(tool_id) == "aleff.c-boundary-warnings"
)
sys.monitoring.free_tool_id(tool_id)

call_count = 0
name = "".join(("aleff.c-boundary-", "warnings"))
sys.monitoring.use_tool_id(tool_id, name)

def sentinel(code, instruction_offset, target, argument):
    global call_count
    if target is len:
        call_count += 1

sys.monitoring.register_callback(tool_id, sys.monitoring.events.CALL, sentinel)
sys.monitoring.set_events(tool_id, sys.monitoring.events.CALL)

print(aleff.c_warnings_enabled())
aleff.disable_c_warnings()
len(())
print(sys.monitoring.get_tool(tool_id))
print(sys.monitoring.get_events(tool_id))
print(call_count)
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "False",
        "aleff.c-boundary-warnings",
        str(sys.monitoring.events.CALL),
        "1",
    ]


def test_reload_reuses_the_owned_monitoring_tool() -> None:
    script = """
import importlib
import sys
import warnings

import aleff._multishot.v1.monitoring as monitoring

owner = monitoring._TOOL_NAME
warning_type = monitoring.CFrameContinuationWarning
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    importlib.reload(monitoring)

claimed = [tool_id for tool_id in (3, 4) if sys.monitoring.get_tool(tool_id) is owner]
print(monitoring.c_warnings_enabled())
print(len(claimed))
print(monitoring._TOOL_NAME is owner)
print(monitoring.CFrameContinuationWarning is warning_type)
print(len(caught))
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["True", "1", "True", "True", "0"]


def test_reload_preserves_public_warning_filters_for_real_boundaries() -> None:
    script = """
import collections
import importlib
import warnings

import aleff
import aleff._multishot.v1.monitoring as monitoring

warning_type = aleff.CFrameContinuationWarning
importlib.reload(monitoring)
suspend = aleff.effect("suspend")
handler = aleff.create_handler(suspend)

class Items:
    def __iter__(self):
        suspend()
        yield None

@handler.on(suspend)
def handle(_resume):
    return "handled"

print(monitoring.CFrameContinuationWarning is warning_type)
with warnings.catch_warnings():
    warnings.simplefilter("error", warning_type)
    try:
        handler(lambda: collections.deque(Items()))
    except BaseException as error:
        print(type(error) is warning_type)
    else:
        print(False)
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["True", "True"]


def test_repeated_reload_keeps_one_tool_that_disable_can_release() -> None:
    script = """
import importlib
import sys
import warnings

import aleff._multishot.v1.monitoring as monitoring

with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    for _ in range(4):
        importlib.reload(monitoring)

claimed = [
    tool_id
    for tool_id in (3, 4)
    if sys.monitoring.get_tool(tool_id) is monitoring._TOOL_NAME
]
print(monitoring.c_warnings_enabled())
print(len(claimed))
print(len(caught))
monitoring.disable_c_warnings()
print([sys.monitoring.get_tool(tool_id) for tool_id in (3, 4)])
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["True", "1", "0", "[None, None]"]


def test_reload_from_disabled_state_enables_one_monitoring_tool() -> None:
    script = """
import importlib
import sys

import aleff._multishot.v1.monitoring as monitoring

owner = monitoring._TOOL_NAME
monitoring.disable_c_warnings()
print([sys.monitoring.get_tool(tool_id) for tool_id in (3, 4)])
importlib.reload(monitoring)
claimed = [tool_id for tool_id in (3, 4) if sys.monitoring.get_tool(tool_id) is owner]
print(monitoring.c_warnings_enabled())
print(len(claimed))
print(monitoring._TOOL_NAME is owner)
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=ROOT,
        env=os.environ.copy(),
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["[None, None]", "True", "1", "True"]
