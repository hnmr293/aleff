"""CPython compatibility checks for iterator built-ins and awaitables."""

import textwrap

from cpython_compat_support import assert_cpython_compatible


def test_iter_normal_errors_and_corner_cases() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        def observe(label, operation):
            try:
                print(label, "return", operation())
            except BaseException as exc:
                print(label, "raise", type(exc).__name__, str(exc))

        class Sequence:
            def __getitem__(self, index):
                if index == 0:
                    return "first"
                if index == 1:
                    return "second"
                raise IndexError

        print("getitem", list(iter(Sequence())))

        class SelfIterator:
            def __iter__(self):
                return self

            def __next__(self):
                raise StopIteration

        iterator = SelfIterator()
        print("identity", iter(iterator) is iterator)
        print("empty", list(iterator))

        class CallableValue:
            def __init__(self):
                self.values = iter(("item", "stop", "after"))

            def __call__(self):
                return next(self.values)

        print("callable-sentinel", list(iter(CallableValue(), "stop")))

        class BadIterable:
            def __iter__(self):
                return 42

        observe("bad-result", lambda: iter(BadIterable()))
        observe("not-iterable", lambda: iter(42))
        observe("missing-argument", lambda: iter())
        observe("too-many-arguments", lambda: iter([], None, None))
            """
        )
    )


def test_next_normal_errors_and_long_type_names() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        def observe(label, operation):
            try:
                print(label, "return", operation())
            except BaseException as exc:
                print(label, "raise", type(exc).__name__, str(exc))

        class Values:
            def __init__(self):
                self.values = iter((1, 2))

            def __iter__(self):
                return self

            def __next__(self):
                return next(self.values)

        iterator = Values()
        print("values", next(iterator), next(iterator))
        observe("default", lambda: next(iterator, "fallback"))
        observe("without-default", lambda: next(iterator))

        class Failing:
            def __iter__(self):
                return self

            def __next__(self):
                raise RuntimeError("iterator failure")

        observe("propagated-error", lambda: next(Failing()))
        observe("not-iterator", lambda: next(42))

        LongName = type("T" * 300, (), {})
        observe("long-type-name", lambda: next(LongName()))
        observe("missing-argument", lambda: next())
        observe("too-many-arguments", lambda: next(iter(()), 1, 2))
            """
        )
    )


def test_next_and_anext_builtin_metadata() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        import builtins
        import inspect

        for name in ("next", "anext"):
            function = getattr(builtins, name)
            print(name, "text-signature", repr(getattr(function, "__text_signature__", None)))
            print(name, "doc", repr(function.__doc__))
            try:
                print(name, "signature", inspect.signature(function))
            except BaseException as exc:
                print(name, "signature-error", type(exc).__name__, str(exc))
            """
        )
    )


def test_aiter_normal_errors_and_corner_cases() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        def observe(label, operation):
            try:
                print(label, "return", operation())
            except BaseException as exc:
                print(label, "raise", type(exc).__name__, str(exc))

        class AsyncIterator:
            def __aiter__(self):
                return self

            async def __anext__(self):
                raise StopAsyncIteration

        iterator = AsyncIterator()
        print("identity", aiter(iterator) is iterator)

        class AsyncIterable:
            def __aiter__(self):
                return AsyncIterator()

        result = aiter(AsyncIterable())
        print("fresh-type", type(result).__name__, result is not iterator)

        class BadAsyncIterable:
            def __aiter__(self):
                return 42

        observe("bad-result", lambda: aiter(BadAsyncIterable()))
        observe("not-async-iterable", lambda: aiter(42))
        observe("missing-argument", lambda: aiter())
        observe("too-many-arguments", lambda: aiter(iterator, iterator))
            """
        )
    )


def test_anext_normal_default_and_error_behavior() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        import asyncio

        def observe(label, operation):
            try:
                print(label, "return", operation())
            except BaseException as exc:
                print(label, "raise", type(exc).__name__, str(exc))

        class Values:
            def __init__(self):
                self.values = (1, 2)
                self.index = 0

            def __aiter__(self):
                return self

            async def __anext__(self):
                if self.index == len(self.values):
                    raise StopAsyncIteration
                value = self.values[self.index]
                self.index += 1
                return value

        async def consume_values():
            iterator = Values()
            return (
                await anext(iterator),
                await anext(iterator),
                await anext(iterator, "fallback"),
            )

        print("values", asyncio.run(consume_values()))

        class Empty:
            def __aiter__(self):
                return self

            async def __anext__(self):
                raise StopAsyncIteration

        async def consume_empty():
            return await anext(Empty(), "default")

        print("default", asyncio.run(consume_empty()))

        async def consume_without_default():
            return await anext(Empty())

        observe("stop-without-default", lambda: asyncio.run(consume_without_default()))

        class Failing:
            def __aiter__(self):
                return self

            async def __anext__(self):
                raise RuntimeError("async iterator failure")

        async def consume_failure():
            return await anext(Failing())

        observe("propagated-error", lambda: asyncio.run(consume_failure()))
        observe("not-async-iterator", lambda: anext(42))
        observe("missing-argument", lambda: anext())
        observe("too-many-arguments", lambda: anext(Empty(), 1, 2))

        class InvalidResult:
            def __aiter__(self):
                return self

            def __anext__(self):
                return 42

        async def consume_invalid_result():
            return await anext(InvalidResult())

        observe("invalid-awaitable", lambda: asyncio.run(consume_invalid_result()))
            """
        )
    )


def test_anext_awaitable_protocol_shape_and_resume_methods() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        import inspect

        class Awaitable:
            def __await__(self):
                def run():
                    received = yield "paused"
                    return ("done", received)

                return run()

        class AsyncIterator:
            def __aiter__(self):
                return self

            def __anext__(self):
                return Awaitable()

        awaitable = anext(AsyncIterator())
        print("type", type(awaitable).__name__)
        print("inspect", inspect.iscoroutine(awaitable), inspect.isawaitable(awaitable))
        iterator = awaitable.__await__()
        print(
            "iterator-methods",
            [hasattr(iterator, name) for name in ("send", "throw", "close")],
        )
        print("first", next(iterator))
        try:
            result = iterator.send(None)
        except StopIteration as exc:
            print("send", "StopIteration", repr(exc.value))
        except BaseException as exc:
            print("send", "raise", type(exc).__name__, str(exc))
        else:
            print("send", "return", result)
            """
        )
    )


def test_anext_without_default_preserves_native_shape_across_multishot_resume() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        import inspect
        import sys

        def resumed(body, values=(1, 10)):
            def shot(value):
                try:
                    return ("return", body(lambda: value))
                except BaseException as exc:
                    return ("raise", type(exc).__name__, str(exc))

            if "aleff" not in sys.modules:
                return [shot(value) for value in values]

            from aleff import create_handler, effect

            choose = effect("anext-native-shape-choice")
            handler = create_handler(choose)

            @handler.on(choose)
            def handle(k):
                results = []
                for value in values:
                    try:
                        results.append(("return", k(value)))
                    except BaseException as exc:
                        results.append(("raise", type(exc).__name__, str(exc)))
                return results

            return handler(lambda: body(choose))

        def body(choose):
            class AsyncIterator:
                def __aiter__(self):
                    return self

                async def __anext__(self):
                    return choose() + 100

            async def consume():
                awaitable = anext(AsyncIterator())
                iterator = awaitable.__await__()
                visible_shape = (
                    type(awaitable).__module__,
                    type(awaitable).__name__,
                    inspect.iscoroutine(awaitable),
                    inspect.isawaitable(awaitable),
                    type(iterator).__module__,
                    type(iterator).__name__,
                    tuple(hasattr(iterator, name) for name in ("send", "throw", "close")),
                )
                return visible_shape, await awaitable

            coroutine = consume()
            try:
                coroutine.send(None)
            except StopIteration as completed:
                return completed.value
            finally:
                coroutine.close()
            raise AssertionError("consumer coroutine unexpectedly suspended")

        print(repr(resumed(body)))
            """
        )
    )


def test_anext_awaitable_throw_and_close_are_preserved() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        def observe(label, operation):
            try:
                print(label, "return", operation())
            except BaseException as exc:
                print(label, "raise", type(exc).__name__, str(exc))

        class Awaitable:
            def __init__(self, events):
                self.events = events

            def __await__(self):
                def run():
                    try:
                        yield "paused"
                    except ValueError:
                        return "caught"
                    finally:
                        self.events.append("closed")

                return run()

        class AsyncIterator:
            def __init__(self, awaitable):
                self.awaitable = awaitable

            def __aiter__(self):
                return self

            def __anext__(self):
                return self.awaitable

        events = []
        thrown = anext(AsyncIterator(Awaitable(events))).__await__()
        next(thrown)
        def throw_value():
            try:
                thrown.throw(ValueError("injected"))
            except StopIteration as exc:
                return ("StopIteration", repr(exc.value), list(events))

        observe("throw", throw_value)

        closed = anext(AsyncIterator(Awaitable(events))).__await__()
        next(closed)
        observe("close", lambda: (closed.close(), list(events)))
            """
        )
    )


def test_anext_supports_generator_based_coroutines() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        import asyncio
        import types

        @types.coroutine
        def generator_coroutine():
            if False:
                yield None
            return 42

        class AsyncIterator:
            def __aiter__(self):
                return self

            def __anext__(self):
                return generator_coroutine()

        async def consume():
            return await anext(AsyncIterator())

        try:
            print("result", asyncio.run(consume()))
        except BaseException as exc:
            print("error", type(exc).__name__, str(exc))
            """
        )
    )


def test_anext_calls_await_each_time_without_caching() -> None:
    assert_cpython_compatible(
        textwrap.dedent(
            """
        class Reusable:
            def __init__(self):
                self.calls = 0

            def __await__(self):
                self.calls += 1

                def done():
                    if False:
                        yield None
                    return 42

                return done()

        class AsyncIterator:
            def __init__(self):
                self.awaitable = Reusable()

            def __aiter__(self):
                return self

            def __anext__(self):
                return self.awaitable

        source = AsyncIterator()
        awaitable = anext(source)
        first = awaitable.__await__()
        second = awaitable.__await__()
        print("same-iterator", first is second)
        print("await-calls", source.awaitable.calls)
            """
        )
    )
