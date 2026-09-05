"""Diagnostics for unsupported C boundaries captured by continuations."""

import sys
import threading
import types
import warnings
import weakref
from collections.abc import Callable
from dataclasses import dataclass
from inspect import getattr_static
from types import CodeType
from typing import Any, Protocol, cast

import greenlet as gl

from ._aleff import _has_continuation_adapter  # pyright: ignore[reportPrivateUsage]


_TOOL_IDS = (3, 4)


class _ToolName(str):
    pass


_previous_tool_name = globals().get("_TOOL_NAME")
_previous_tool_id = globals().get("_tool_id")
_previous_generation = globals().get("_generation")
_previous_warning_type = cast(
    "type[CFrameContinuationWarning] | None",
    globals().get("CFrameContinuationWarning"),
)
_reloading = _previous_tool_name is not None
_TOOL_NAME = (
    cast(_ToolName, _previous_tool_name) if _previous_tool_name is not None else _ToolName("aleff.c-boundary-warnings")
)
_STATE_ATTRIBUTE = "_aleff_c_boundary_state"
_lock = threading.RLock()
_tool_id = cast(int | None, _previous_tool_id)
_generation = cast(int, _previous_generation) if _previous_generation is not None else 0
type _WarningRegistryKey = str | tuple[str, type[Warning], int]
type _MonitoringCallback = Callable[..., object]

_warning_registries: weakref.WeakKeyDictionary[CodeType, dict[_WarningRegistryKey, int]] = weakref.WeakKeyDictionary()


if _previous_warning_type is None:

    class CFrameContinuationWarning(RuntimeWarning):
        """A continuation snapshot crossed an unsupported C boundary."""

else:
    CFrameContinuationWarning = _previous_warning_type


@dataclass(slots=True)
class _BoundaryState:
    generation: int
    active: list[tuple[CodeType, int, object]]


type _Boundary = tuple[CodeType, int, object]
type _BoundaryToken = tuple[_Boundary, ...]


class _CallableDescriptor(Protocol):
    @property
    def __func__(self) -> object: ...


class _TypeAttributeDescriptor(Protocol):
    def __get__(self, instance: object, owner: type[object] | None = None) -> object: ...


_TYPE_MODULE = cast(_TypeAttributeDescriptor, type.__dict__["__module__"])
_TYPE_QUALNAME = cast(_TypeAttributeDescriptor, type.__dict__["__qualname__"])


def _new_continuation_greenlet[**P, R](run: Callable[P, R]) -> gl.greenlet:
    continuation = gl.greenlet(run)
    setattr(continuation, _STATE_ATTRIBUTE, _BoundaryState(_generation, []))
    return continuation


def _on_call(code: CodeType, instruction_offset: int, target: object, arg0: object) -> Any:
    del arg0
    current = gl.getcurrent()
    state = getattr(current, _STATE_ATTRIBUTE, None)
    if state is None:
        return None
    if state.generation != _generation:
        state = _BoundaryState(_generation, [])
        setattr(current, _STATE_ATTRIBUTE, state)
    target_type = type(target)
    if target_type is types.FunctionType or target_type is types.MethodType:
        return None
    state.active.append((code, instruction_offset, target))
    return None


def _on_c_exit(code: CodeType, instruction_offset: int, target: object, result: object) -> Any:
    del result
    state = getattr(gl.getcurrent(), _STATE_ATTRIBUTE, None)
    if state is None or state.generation != _generation:
        return None
    for index in range(len(state.active) - 1, -1, -1):
        boundary_code, boundary_offset, boundary_target = state.active[index]
        if boundary_code is code and boundary_offset == instruction_offset and boundary_target is target:
            del state.active[index]
            break
    return None


def _resolve_callable_descriptor(target: object) -> object:
    while True:
        target_type = type(target)
        if target_type is staticmethod or target_type is classmethod:
            target = cast(_CallableDescriptor, target).__func__
            continue
        descriptor: object = getattr_static(type(target), "__call__", None)
        descriptor_type = type(descriptor)
        if descriptor_type is not staticmethod and descriptor_type is not classmethod:
            return target
        target = cast(_CallableDescriptor, descriptor).__func__


def _is_python_backed_callable(target: object) -> bool:
    target = _resolve_callable_descriptor(target)
    descriptor: object = getattr_static(type(target), "__call__", None)
    target_type = type(target)
    return (
        target_type is types.FunctionType or target_type is types.MethodType or type(descriptor) is types.FunctionType
    )


def _freeze_unsupported_boundaries() -> _BoundaryToken:
    if not c_warnings_enabled():
        return ()
    state = getattr(gl.getcurrent(), _STATE_ATTRIBUTE, None)
    if state is None or state.generation != _generation:
        return ()
    unsupported: list[_Boundary] = []
    for boundary in state.active:
        target = _resolve_callable_descriptor(boundary[2])
        if not _is_python_backed_callable(target) and not _has_continuation_adapter(target):
            unsupported.append(boundary)
    return tuple(unsupported)


def _callable_name(value: object) -> str:
    value_type = type(value)
    builtin_callable = (
        value_type is types.BuiltinFunctionType
        or value_type is types.BuiltinMethodType
        or value_type is types.MethodDescriptorType
        or value_type is types.MethodWrapperType
        or value_type is types.WrapperDescriptorType
    )
    class_object = any(base is type for base in type.mro(value_type))
    target = value if builtin_callable or class_object else value_type

    if builtin_callable:
        try:
            raw_module = object.__getattribute__(target, "__module__")
        except (AttributeError, TypeError):
            raw_module = None
        try:
            raw_name = object.__getattribute__(target, "__qualname__")
        except (AttributeError, TypeError):
            raw_name = None
    else:
        try:
            raw_module = _TYPE_MODULE.__get__(target, type(target))
        except (AttributeError, TypeError):
            raw_module = None
        try:
            raw_name = _TYPE_QUALNAME.__get__(target, type(target))
        except (AttributeError, TypeError):
            raw_name = None

    if type(raw_name) is not str:
        return "<unsupported C callable>"
    name = raw_name
    if type(raw_module) is not str:
        return name
    module = raw_module
    return name if module == "builtins" else f"{module}.{name}"


def _line_number(code: CodeType, instruction_offset: int) -> int:
    for start, end, line in code.co_lines():
        if start <= instruction_offset < end:
            return code.co_firstlineno if line is None else line
    return code.co_firstlineno


def _warn_unsupported_boundaries(token: _BoundaryToken) -> None:
    for code, instruction_offset, target in token:
        message = (
            f"{_callable_name(target)} is an unsupported C boundary captured by an Aleff continuation; "
            "register a continuation adapter or wrap the callable with aleffy()"
        )
        with _lock:
            registry = _warning_registries.setdefault(code, {})
            warnings.warn_explicit(
                message,
                CFrameContinuationWarning,
                code.co_filename,
                _line_number(code, instruction_offset),
                registry=registry,
            )


def _claim_tool_id() -> tuple[int, bool]:
    for tool_id in _TOOL_IDS:
        owner = sys.monitoring.get_tool(tool_id)
        if owner is _TOOL_NAME:
            return tool_id, False
        if owner is not None:
            continue
        try:
            sys.monitoring.use_tool_id(tool_id, _TOOL_NAME)
        except ValueError:
            continue
        return tool_id, True
    raise RuntimeError("no sys.monitoring tool ID is available")


def _configure_tool(tool_id: int) -> None:
    events = (
        (sys.monitoring.events.CALL, _on_call),
        (sys.monitoring.events.C_RETURN, _on_c_exit),
        (sys.monitoring.events.C_RAISE, _on_c_exit),
    )
    previous_events = sys.monitoring.get_events(tool_id)
    previous_callbacks: list[tuple[int, _MonitoringCallback | None]] = []
    try:
        sys.monitoring.set_events(tool_id, sys.monitoring.events.NO_EVENTS)
        for event, callback in events:
            previous = cast(
                _MonitoringCallback | None,
                sys.monitoring.register_callback(tool_id, event, callback),
            )
            previous_callbacks.append((event, previous))
        sys.monitoring.set_events(tool_id, sys.monitoring.events.CALL)
    except BaseException:
        for event, previous in reversed(previous_callbacks):
            sys.monitoring.register_callback(tool_id, event, previous)
        sys.monitoring.set_events(tool_id, previous_events)
        raise


def _enable_c_warnings(refresh: bool) -> None:
    global _generation, _tool_id
    with _lock:
        if c_warnings_enabled() and not refresh:
            return

        tool_id, newly_claimed = _claim_tool_id()
        try:
            _configure_tool(tool_id)
        except BaseException:
            if newly_claimed and sys.monitoring.get_tool(tool_id) is _TOOL_NAME:
                sys.monitoring.free_tool_id(tool_id)
            raise

        _generation += 1
        _tool_id = tool_id


def enable_c_warnings() -> None:
    """Enable unsupported C-boundary warnings for Aleff snapshots."""

    _enable_c_warnings(False)


def disable_c_warnings() -> None:
    """Disable C-boundary warnings and release Aleff's monitoring tool ID."""

    global _generation, _tool_id
    with _lock:
        tool_id = _tool_id
        if tool_id is None:
            return
        _generation += 1
        if sys.monitoring.get_tool(tool_id) is not _TOOL_NAME:
            _tool_id = None
            return
        sys.monitoring.set_events(tool_id, sys.monitoring.events.NO_EVENTS)
        sys.monitoring.register_callback(tool_id, sys.monitoring.events.CALL, None)
        sys.monitoring.register_callback(tool_id, sys.monitoring.events.C_RETURN, None)
        sys.monitoring.register_callback(tool_id, sys.monitoring.events.C_RAISE, None)
        sys.monitoring.free_tool_id(tool_id)
        _tool_id = None


def c_warnings_enabled() -> bool:
    """Return whether unsupported C-boundary monitoring is active."""

    tool_id = _tool_id
    return (
        tool_id is not None
        and sys.monitoring.get_tool(tool_id) is _TOOL_NAME
        and sys.monitoring.get_events(tool_id) == sys.monitoring.events.CALL
    )


try:
    _enable_c_warnings(_reloading)
except RuntimeError as error:
    warnings.warn(str(error), RuntimeWarning, stacklevel=2)


__all__ = [
    "CFrameContinuationWarning",
    "c_warnings_enabled",
    "disable_c_warnings",
    "enable_c_warnings",
]
