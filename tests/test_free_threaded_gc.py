"""Regression tests for continuation restoration under free-threaded GC."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import sysconfig
import textwrap

import pytest


PROJECT_ROOT = Path(__file__).parents[1]
IS_FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))

pytestmark = pytest.mark.skipif(
    sys.version_info < (3, 14) or not IS_FREE_THREADED,
    reason="requires free-threaded CPython 3.14+",
)


def run_python(*args: str, timeout: int = 60) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["PYTHONFAULTHANDLER"] = "1"
    return subprocess.run(
        [sys.executable, *args],
        cwd=PROJECT_ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def assert_success(result: subprocess.CompletedProcess[str]) -> None:
    assert result.returncode == 0, (
        f"subprocess exited with {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )


def test_demo_amb_matches_expected_output():
    result = run_python("examples/demo_amb.py")

    assert_success(result)
    expected = (PROJECT_ROOT / "examples/expected/demo_amb.txt").read_text()
    assert result.stdout == expected
    assert result.stderr == ""


def test_demo_amb_survives_concurrent_gc():
    code = textwrap.dedent(
        """
        import gc
        import runpy
        import threading

        stop = threading.Event()

        def collect_repeatedly():
            while not stop.wait(0.001):
                gc.collect()

        collector = threading.Thread(target=collect_repeatedly)
        collector.start()
        try:
            runpy.run_path("examples/demo_amb.py", run_name="__main__")
        finally:
            stop.set()
            collector.join()
        """
    )

    result = run_python("-c", code)

    assert_success(result)
    expected = (PROJECT_ROOT / "examples/expected/demo_amb.txt").read_text()
    assert result.stdout == expected
    assert result.stderr == ""


def test_exception_frame_restores_mortal_and_immortal_values_during_gc():
    code = textwrap.dedent(
        """
        import gc
        import threading

        import greenlet

        from aleff._multishot.v1._aleff import restore_continuation, snapshot_frames

        stop = threading.Event()

        def collect_repeatedly():
            while not stop.wait(0.001):
                gc.collect()

        def perform():
            snapshot = snapshot_frames()
            return greenlet.getcurrent().parent.switch(snapshot)

        def user_code():
            mortal_local = object()
            try:
                raise ValueError("active exception frame")
            except ValueError:
                return mortal_local, 7, perform()

        original = greenlet.greenlet(user_code)
        snapshot = original.switch()
        one_shot_value = object()
        one_shot_result = original.switch(one_shot_value)
        captured_mortal = one_shot_result[0]
        assert one_shot_result == (captured_mortal, 7, one_shot_value)

        collector = threading.Thread(target=collect_repeatedly)
        collector.start()
        try:
            values = [None, object(), 0, object()] * 75
            for value in values:
                resumed = greenlet.greenlet(
                    lambda resume_value=value: restore_continuation(snapshot, resume_value)
                ).switch()
                assert resumed[0] is captured_mortal
                assert resumed[1] == 7
                assert resumed[2] is value
        finally:
            stop.set()
            collector.join()
        """
    )

    result = run_python("-c", code)

    assert_success(result)
    assert result.stdout == ""
    assert result.stderr == ""
