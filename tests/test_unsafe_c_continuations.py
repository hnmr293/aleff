from __future__ import annotations

from collections.abc import Callable
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import sysconfig

import pytest

from aleff import aleffy


ROOT = Path(__file__).resolve().parents[1]
HELPER_SOURCE = Path(__file__).with_name("c_aleffy_helper.c")


@pytest.fixture(scope="session")
def aleffy_helper(tmp_path_factory: pytest.TempPathFactory) -> Path:
    if not sys.platform.startswith("linux") or os.uname().machine != "x86_64":
        pytest.skip("the aleffy feasibility spike supports Linux x86-64 only")
    if sys.version_info[:2] not in {(3, 12), (3, 13), (3, 14)}:
        pytest.skip("the aleffy feasibility spike supports CPython 3.12 through 3.14 only")
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        pytest.skip("the aleffy feasibility spike requires GIL-enabled CPython")
    compiler = shutil.which(os.environ.get("CC", "cc"))
    include = sysconfig.get_path("include")
    if compiler is None or not (Path(include) / "Python.h").is_file():
        pytest.skip("a C compiler and Python development headers are required")

    extension_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if not extension_suffix:
        pytest.fail("Python did not report an extension module suffix")
    output = tmp_path_factory.mktemp("aleffy-helper") / f"aleffy_test_helper{extension_suffix}"
    command = [
        compiler,
        "-std=c2x",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-shared",
        "-fPIC",
        f"-I{include}",
        "-o",
        str(output),
        str(HELPER_SOURCE),
    ]
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    if result.returncode != 0:
        pytest.fail(f"could not compile aleffy helper:\n{result.stdout}\n{result.stderr}")
    return output


def _run_isolated(helper: Path, source: str) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join(path for path in (str(helper.parent), env.get("PYTHONPATH")) if path)
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", "-c", source],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        timeout=15,
    )


def test_aleffy_rejects_non_callable() -> None:
    with pytest.raises(TypeError, match="aleffy requires a callable"):
        aleffy(42)  # pyright: ignore[reportArgumentType]


@pytest.mark.parametrize("kind", ["cdll", "pydll", "cfuncptr"])
def test_aleffy_rejects_ctypes_function_pointers(kind: str) -> None:
    def assert_rejected(function: Callable[..., object]) -> None:
        with pytest.raises(TypeError, match="aleffy does not support ctypes callables"):
            aleffy(function)

    if kind == "cdll":
        assert_rejected(
            ctypes.CDLL("kernel32.dll").GetCurrentProcessId if sys.platform == "win32" else ctypes.CDLL(None).strlen
        )
    elif kind == "pydll":
        assert_rejected(ctypes.pythonapi.Py_IncRef)
    else:

        @ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)
        def callback(value: int) -> int:
            return value

        assert_rejected(callback)


@pytest.mark.skipif(
    sys.platform.startswith("linux")
    and os.uname().machine == "x86_64"
    and sys.version_info[:2] in {(3, 12), (3, 13), (3, 14)}
    and not sysconfig.get_config_var("Py_GIL_DISABLED"),
    reason="the current interpreter supports the aleffy feasibility spike",
)
def test_aleffy_rejects_unsupported_build() -> None:
    with pytest.raises(
        NotImplementedError,
        match=("aleffy feasibility spike requires Linux x86-64 with GIL-enabled CPython 3.12 through 3.14"),
    ):
        aleffy(abs)(-1)


def test_aleffy_transparently_returns_without_effect(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy

assert aleffy(helper.aleff_test_aleffy_call)(lambda: 5) == 705
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_forwards_positional_and_keyword_arguments(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
from aleff import aleffy

assert aleffy(pow)(2, 5) == 32
assert aleffy(dict)(answer=42) == {"answer": 42}
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_propagates_callback_exception(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy

def fail():
    raise ValueError("callback failed")

try:
    aleffy(helper.aleff_test_aleffy_call)(fail)
except ValueError as exc:
    assert str(exc) == "callback failed"
else:
    raise AssertionError("callback exception was not propagated")
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_resumes_c_function_without_replaying_prefix(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy, create_handler, effect

choose = effect("choose")
handler = create_handler(choose)

@handler.on(choose)
def choose_three_times(k):
    return [k(11), k(29), k(47)]

wrapped = aleffy(helper.aleff_test_aleffy_call)
for iteration in range(100):
    helper.aleff_test_aleffy_reset()
    result = handler(lambda: wrapped(choose))
    assert result == [711, 729, 747], (iteration, result)
    assert helper.aleff_test_aleffy_before_count() == 1, iteration
    assert helper.aleff_test_aleffy_after_count() == 3, iteration
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_restores_native_stack_on_small_thread_stack(
    aleffy_helper: Path,
) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
import threading
from aleff import aleffy, create_handler, effect

choose = effect("choose")
handler = create_handler(choose)

@handler.on(choose)
def choose_twice(k):
    return [k(17), k(31)]

results = []
errors = []

def worker():
    try:
        wrapped = aleffy(helper.aleff_test_aleffy_deep_call)
        results.append(handler(lambda: wrapped(choose, 64)))
    except BaseException as exc:
        errors.append(exc)

original_stack_size = threading.stack_size()
try:
    threading.stack_size(256 * 1024)
    thread = threading.Thread(target=worker)
    thread.start()
    thread.join(10)
finally:
    threading.stack_size(original_stack_size)

assert not thread.is_alive()
assert not errors, errors
assert results == [[17, 31]], results
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_can_resume_again_after_c_suffix_raises(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy, create_handler, effect

choose = effect("choose")
handler = create_handler(choose)

@handler.on(choose)
def fail_then_succeed(k):
    try:
        k("not an integer")
    except TypeError:
        pass
    else:
        raise AssertionError("the restored C suffix did not propagate its error")
    return k(5)

assert handler(lambda: aleffy(helper.aleff_test_aleffy_call)(choose)) == 705
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_handles_another_effect_after_resuming_native_stack(
    aleffy_helper: Path,
) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy, create_handler, effect

choose = effect("choose")
handler = create_handler(choose)

@handler.on(choose)
def choose_twice(k):
    return [k(1), k(2)]

helper.aleff_test_aleffy_reset()
wrapped = aleffy(helper.aleff_test_aleffy_call_twice)
result = handler(lambda: wrapped(choose))
assert result == [[1001, 1002], [2001, 2002]], result
assert helper.aleff_test_aleffy_before_count() == 1
assert helper.aleff_test_aleffy_after_count() == 4
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_propagates_exception_from_callback_after_native_resume(
    aleffy_helper: Path,
) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy, create_handler, effect

choose = effect("choose")
handler = create_handler(choose)
calls = 0

def callback():
    global calls
    calls += 1
    if calls == 1:
        return choose()
    raise ValueError("second callback failed")

@handler.on(choose)
def resume_and_collect_errors(k):
    errors = []
    for value in (3, 7):
        try:
            k(value)
        except ValueError as exc:
            errors.append(str(exc))
    return errors

helper.aleff_test_aleffy_reset()
wrapped = aleffy(helper.aleff_test_aleffy_call_twice)
assert handler(lambda: wrapped(callback)) == [
    "second callback failed",
    "second callback failed",
]
assert helper.aleff_test_aleffy_before_count() == 1
assert helper.aleff_test_aleffy_after_count() == 0
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_aborts_from_effect_after_native_resume(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy, create_handler, effect

choose = effect("choose")
stop = effect("stop")
handler = create_handler(choose, stop)
calls = 0

def callback():
    global calls
    calls += 1
    if calls == 1:
        return choose()
    return stop()

@handler.on(choose)
def resume_twice(k):
    return [k(5), k(9)]

@handler.on(stop)
def abort(_k):
    return 123

helper.aleff_test_aleffy_reset()
wrapped = aleffy(helper.aleff_test_aleffy_call_twice)
assert handler(lambda: wrapped(callback)) == [123, 123]
assert helper.aleff_test_aleffy_before_count() == 1
assert helper.aleff_test_aleffy_after_count() == 0
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_allows_nested_boundaries_without_an_effect(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
from aleff import aleffy

outer = aleffy(lambda: aleffy(abs)(-1))
assert outer() == 1
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_resumes_nested_boundaries_without_replaying_prefix(
    aleffy_helper: Path,
) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy, create_handler, effect

choose = effect("choose")
handler = create_handler(choose)

@handler.on(choose)
def choose_three_times(k):
    return [k(11), k(29), k(47)]

inner = aleffy(helper.aleff_test_aleffy_call)
outer = aleffy(helper.aleff_test_aleffy_call)
helper.aleff_test_aleffy_reset()
result = handler(lambda: outer(lambda: inner(choose)))
assert result == [1411, 1429, 1447], result
assert helper.aleff_test_aleffy_before_count() == 2
assert helper.aleff_test_aleffy_after_count() == 6
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_restores_nested_state_after_exception(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
from aleff import aleffy

def fail():
    raise LookupError("nested failure")

try:
    aleffy(lambda: aleffy(fail)())()
except LookupError as exc:
    assert str(exc) == "nested failure"
else:
    raise AssertionError("nested exception was not propagated")

assert aleffy(abs)(-9) == 9
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_allows_overlapping_boundaries_on_two_threads(
    aleffy_helper: Path,
) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import threading
from aleff import aleffy

barrier = threading.Barrier(3)
results = []

def worker(value):
    def wait_for_other_boundary():
        barrier.wait(5)
        return value
    results.append(aleffy(wait_for_other_boundary)())

threads = [threading.Thread(target=worker, args=(value,)) for value in (11, 29)]
for thread in threads:
    thread.start()
barrier.wait(5)
for thread in threads:
    thread.join(5)
assert all(not thread.is_alive() for thread in threads)
assert sorted(results) == [11, 29], results
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_preserves_existing_eval_hook_for_other_threads(
    aleffy_helper: Path,
) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
import threading
from aleff import aleffy

ready = threading.Event()
release = threading.Event()

def leaf(value):
    return value + 1

def worker():
    ready.set()
    assert release.wait(5)
    for value in range(100):
        assert leaf(value) == value + 1

helper.aleff_test_eval_hook_install()
try:
    thread = threading.Thread(target=worker)
    thread.start()
    assert ready.wait(5)
    helper.aleff_test_eval_hook_reset()

    def run_worker():
        release.set()
        thread.join(5)
        assert not thread.is_alive()
        return 7

    assert aleffy(run_worker)() == 7
    assert helper.aleff_test_eval_hook_owner_thread_count() > 0
    assert helper.aleff_test_eval_hook_other_thread_count() > 0

    previous_count = helper.aleff_test_eval_hook_other_thread_count()
    later = threading.Thread(target=lambda: leaf(1))
    later.start()
    later.join(5)
    assert helper.aleff_test_eval_hook_other_thread_count() > previous_count
finally:
    helper.aleff_test_eval_hook_uninstall()
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_restores_existing_eval_hook_after_last_overlapping_boundary(
    aleffy_helper: Path,
) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
import threading
from aleff import aleffy

entered = [threading.Event(), threading.Event()]
release = [threading.Event(), threading.Event()]

def worker(index):
    def wait_for_release():
        entered[index].set()
        assert release[index].wait(5)
        return index
    assert aleffy(wait_for_release)() == index

helper.aleff_test_eval_hook_install()
try:
    threads = [threading.Thread(target=worker, args=(index,)) for index in range(2)]
    threads[0].start()
    assert entered[0].wait(5)
    threads[1].start()
    assert entered[1].wait(5)
    assert not helper.aleff_test_eval_hook_is_current()

    release[0].set()
    threads[0].join(5)
    assert not threads[0].is_alive()
    assert not helper.aleff_test_eval_hook_is_current()

    release[1].set()
    threads[1].join(5)
    assert not threads[1].is_alive()
    assert helper.aleff_test_eval_hook_is_current()
finally:
    release[0].set()
    release[1].set()
    helper.aleff_test_eval_hook_uninstall()
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_rejects_resume_from_another_thread(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
import threading
from aleff import aleffy, create_handler, effect

choose = effect("choose")
handler = create_handler(choose)
saved = []

@handler.on(choose)
def save_resume(k):
    saved.append(k)
    return "captured"

assert handler(lambda: aleffy(helper.aleff_test_aleffy_call)(choose)) == "captured"
errors = []

def resume_elsewhere():
    try:
        saved[0](11)
    except BaseException as exc:
        errors.append(exc)

thread = threading.Thread(target=resume_elsewhere)
thread.start()
thread.join()
assert len(errors) == 1, errors
assert isinstance(errors[0], RuntimeError), errors[0]
assert str(errors[0]) == "aleffy continuation belongs to another thread or interpreter"
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_abort_does_not_run_c_suffix(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import aleffy_test_helper as helper
from aleff import aleffy, create_handler, effect

stop = effect("stop")
handler = create_handler(stop)

@handler.on(stop)
def abort(_k):
    return 123

assert handler(lambda: aleffy(helper.aleff_test_aleffy_call)(stop)) == 123
assert helper.aleff_test_aleffy_before_count() == 1
assert helper.aleff_test_aleffy_after_count() == 0
""",
    )
    assert result.returncode == 0, result.stderr
