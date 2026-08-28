"""Explicit adapters for state held by selected native-library operations."""

import itertools
from collections.abc import Callable
from contextvars import Context, ContextVar
from functools import wraps
import platform
import sys
from typing import Any, cast, overload

from .winds import wind_count, wind_range, wind_repeat


type AdapterFactory = Callable[..., Any]


class AdapterRegistry:
    """Map exact operation identities to state-preserving adapter factories."""

    def __init__(self) -> None:
        self._entries: list[tuple[object, AdapterFactory]] = []

    @overload
    def register(self, operation: object, adapter: AdapterFactory) -> AdapterFactory: ...

    @overload
    def register(self, operation: object) -> Callable[[AdapterFactory], AdapterFactory]: ...

    def register(
        self,
        operation: object,
        adapter: AdapterFactory | None = None,
    ) -> AdapterFactory | Callable[[AdapterFactory], AdapterFactory]:
        """Register an adapter, replacing only an exact identity match."""

        if adapter is None:

            def decorate(factory: AdapterFactory) -> AdapterFactory:
                self.register(operation, factory)
                return factory

            return decorate

        if not callable(adapter):
            raise TypeError("adapter must be callable")

        for index, (registered, _) in enumerate(self._entries):
            if registered is operation:
                self._entries[index] = (registered, adapter)
                return adapter

        self._entries.append((operation, adapter))
        return adapter

    def get(self, operation: object, default: AdapterFactory | None = None) -> AdapterFactory | None:
        """Return the adapter registered for the exact operation identity."""

        for registered, adapter in self._entries:
            if registered is operation:
                return adapter
        return default

    def __getitem__(self, operation: object) -> AdapterFactory:
        adapter = self.get(operation)
        if adapter is None:
            raise KeyError(operation)
        return adapter

    def __contains__(self, operation: object) -> bool:
        return self.get(operation) is not None

    def unregister(self, operation: object) -> None:
        """Remove the adapter registered for the exact operation identity."""

        for index, (registered, _) in enumerate(self._entries):
            if registered is operation:
                del self._entries[index]
                return
        raise KeyError(operation)

    def adapt(self, operation: Callable[..., Any], /, *args: Any, **kwargs: Any) -> Any:
        """Invoke a registered adapter, or fall back to the operation."""

        factory = self.get(operation)
        if factory is None:
            return operation(*args, **kwargs)
        return factory(*args, **kwargs)


adapter_registry = AdapterRegistry()
adapter_registry.register(range, wind_range)
adapter_registry.register(itertools.count, wind_count)
adapter_registry.register(itertools.repeat, wind_repeat)

_native_continuation_depth: ContextVar[int] = ContextVar(
    "aleff_native_continuation_depth",
    default=0,
)


class NativeContinuationUnavailableError(RuntimeError):
    """Raised when ``X(func)`` needs unsupported native-stack cloning."""


def native_continuation_active(context: Context | None = None) -> bool:
    """Return whether a context is inside an explicit ``X(func)`` extent."""

    if context is None:
        return _native_continuation_depth.get() > 0
    return context.get(_native_continuation_depth, 0) > 0


def native_continuation_supported() -> bool:
    """Return whether native continuations are available in this process."""

    gil_enabled = getattr(sys, "_is_gil_enabled", lambda: True)
    return (
        sys.implementation.name == "cpython"
        and (3, 12) <= sys.version_info[:2] < (3, 15)
        and sys.platform == "linux"
        and platform.machine().lower() in {"x86_64", "amd64"}
        and gil_enabled()
        and hasattr(__import__("greenlet").greenlet, "clone")
    )


def adapt(operation: Callable[..., Any], /, *args: Any, **kwargs: Any) -> Any:
    """Invoke the explicit adapter registered for an operation."""

    return adapter_registry.adapt(operation, *args, **kwargs)


class _XSurface:
    """Public entry point for explicit native-operation adapters."""

    registry = adapter_registry

    def __call__(self, operation: Callable[..., Any], /) -> Callable[..., Any]:
        if not callable(operation):
            raise TypeError("X() expects a callable")

        adapter = adapter_registry.get(operation)
        if adapter is not None:

            @wraps(operation)
            def adapted(*args: Any, **kwargs: Any) -> Any:
                return adapter(*args, **kwargs)

            return adapted

        @wraps(operation)
        def unsafe(*args: Any, **kwargs: Any) -> Any:
            depth = _native_continuation_depth.get()
            _native_continuation_depth.set(depth + 1)
            try:
                return operation(*args, **kwargs)
            finally:
                # Native stack clones copy this finally block. ContextVar
                # tokens are single-use and tied to their originating Context,
                # so restoring the integer value is deliberately token-free.
                _native_continuation_depth.set(depth)

        return unsafe

    def register(self, operation: object, adapter: AdapterFactory | None = None) -> Any:
        if adapter is None:
            return adapter_registry.register(operation)
        return adapter_registry.register(operation, adapter)

    def unregister(self, operation: object) -> None:
        adapter_registry.unregister(operation)

    def range(self, *args: int) -> wind_range:
        return cast(wind_range, adapt(range, *args))

    def count(self, start: Any = 0, step: Any = 1) -> wind_count:
        return cast(wind_count, adapt(itertools.count, start, step))

    def repeat[T](self, value: T, times: int | None = None) -> wind_repeat[T]:
        return cast(wind_repeat[T], adapt(itertools.repeat, value, times))


X = _XSurface()


__all__ = [
    "AdapterFactory",
    "AdapterRegistry",
    "NativeContinuationUnavailableError",
    "X",
    "adapt",
    "adapter_registry",
    "native_continuation_supported",
]
