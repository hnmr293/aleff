"""Acceptance tests for native callback continuations from Issue #54."""

from __future__ import annotations

import json
import platform
import subprocess
import sys
import textwrap

import pytest


def _supported_target() -> bool:
    gil_enabled = getattr(sys, "_is_gil_enabled", lambda: True)
    return sys.platform == "linux" and platform.machine().lower() in {"x86_64", "amd64"} and gil_enabled()


requires_native_target = pytest.mark.skipif(
    not _supported_target(),
    reason="native continuation initially supports Linux x86-64 GIL CPython",
)


def _run_native_case(body: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(body)],
        capture_output=True,
        text=True,
        timeout=20,
    )


def _assert_native_case(body: str) -> dict[str, object]:
    result = _run_native_case(body)
    assert result.returncode == 0, (
        f"native continuation child exited with {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    return json.loads(result.stdout)


def test_x_rejects_non_callable() -> None:
    from aleff import X

    with pytest.raises(TypeError):
        X(None)  # type: ignore[arg-type]


@requires_native_target
def test_native_stack_support_reports_supported_target() -> None:
    from aleff import native_continuation_supported

    assert native_continuation_supported()


@requires_native_target
def test_x_resumes_through_active_ctypes_callback_without_replay() -> None:
    payload = _assert_native_case(
        """
        import json
        from aleff import Effect, Handler, Resume, X, create_handler, effect
        from aleff._multishot.v1._aleff import _test_native_call

        choose: Effect[[], int] = effect("native-choose")
        handler: Handler[list[int]] = create_handler(choose)
        calls = []
        native_invoke = X(_test_native_call)

        def callback():
            calls.append("callback")
            return choose()

        @handler.on(choose)
        def handle_choose(k: Resume[int, list[int]]):
            return k(4) + k(5)

        result = handler(lambda: [native_invoke(callback)])
        print(json.dumps({"result": result, "calls": calls}))
        """
    )

    assert payload == {"result": [4, 5], "calls": ["callback"]}


@requires_native_target
def test_x_preserves_falsey_resume_values() -> None:
    payload = _assert_native_case(
        """
        import json
        from aleff import X, create_handler, effect
        from aleff._multishot.v1._aleff import _test_native_call

        choose = effect("native-falsey")
        handler = create_handler(choose)
        @handler.on(choose)
        def handle_choose(k):
            return [k(None), k(0), k(False), k("")]

        result = handler(lambda: X(_test_native_call)(lambda: choose()))
        print(json.dumps({"result": result}))
        """
    )

    assert payload == {"result": [None, 0, False, ""]}


@requires_native_target
def test_x_preserves_exception_after_resume_point() -> None:
    payload = _assert_native_case(
        """
        import json
        from aleff import X, create_handler, effect
        from aleff._multishot.v1._aleff import _test_native_call

        choose = effect("native-exception")
        handler = create_handler(choose)
        calls = 0
        def callback():
            global calls
            calls += 1
            value = choose()
            if value == 2:
                raise ValueError("after resume")
            return value

        @handler.on(choose)
        def handle_choose(k):
            first = k(1)
            try:
                k(2)
            except ValueError as exc:
                return {"first": first, "error": str(exc), "calls": calls}
            raise AssertionError("second shot did not raise")

        result = handler(lambda: X(_test_native_call)(callback))
        print(json.dumps(result))
        """
    )

    assert payload == {"first": 1, "error": "after resume", "calls": 1}


@requires_native_target
def test_x_supports_sequential_resumes_from_async_handlers() -> None:
    payload = _assert_native_case(
        """
        import asyncio
        import json
        from aleff import X, create_async_handler, effect
        from aleff._multishot.v1._aleff import _test_native_call

        choose = effect("native-async")
        handler = create_async_handler(choose)
        calls = 0

        def callback():
            global calls
            calls += 1
            return choose()

        @handler.on(choose)
        async def handle_choose(k):
            return [await k(3), await k(4)]

        async def main():
            result = await handler(lambda: X(_test_native_call)(callback))
            print(json.dumps({"result": result, "calls": calls}))

        asyncio.run(main())
        """
    )

    assert payload == {"result": [3, 4], "calls": 1}
