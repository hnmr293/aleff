from __future__ import annotations

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
    if sys.version_info[:2] != (3, 12):
        pytest.skip("the aleffy feasibility spike supports CPython 3.12 only")
    compiler = shutil.which(os.environ.get("CC", "cc"))
    include = sysconfig.get_path("include")
    if compiler is None or not (Path(include) / "Python.h").is_file():
        pytest.skip("a C compiler and Python development headers are required")

    output = tmp_path_factory.mktemp("aleffy-helper") / "aleffy_helper.so"
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
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", "-c", source, str(helper)],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )


def test_aleffy_rejects_non_callable() -> None:
    with pytest.raises(TypeError, match="aleffy requires a callable"):
        aleffy(42)  # pyright: ignore[reportArgumentType]


@pytest.mark.skipif(
    sys.platform.startswith("linux")
    and os.uname().machine == "x86_64"
    and sys.version_info[:2] == (3, 12)
    and not sysconfig.get_config_var("Py_GIL_DISABLED"),
    reason="the current interpreter supports the aleffy feasibility spike",
)
def test_aleffy_rejects_unsupported_build() -> None:
    with pytest.raises(
        NotImplementedError,
        match="aleffy feasibility spike requires Linux x86-64 with GIL-enabled CPython 3.12",
    ):
        aleffy(abs)(-1)


def test_aleffy_transparently_returns_without_effect(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import ctypes
import sys
from aleff import aleffy

helper = ctypes.PyDLL(sys.argv[1])
helper.aleff_test_aleffy_call.argtypes = [ctypes.py_object]
helper.aleff_test_aleffy_call.restype = ctypes.py_object
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
import ctypes
import sys
from aleff import aleffy

helper = ctypes.PyDLL(sys.argv[1])
helper.aleff_test_aleffy_call.argtypes = [ctypes.py_object]
helper.aleff_test_aleffy_call.restype = ctypes.py_object

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
import ctypes
import sys
from aleff import aleffy, create_handler, effect

helper = ctypes.PyDLL(sys.argv[1])
helper.aleff_test_aleffy_call.argtypes = [ctypes.py_object]
helper.aleff_test_aleffy_call.restype = ctypes.py_object
helper.aleff_test_aleffy_before_count.restype = ctypes.c_int
helper.aleff_test_aleffy_after_count.restype = ctypes.c_int
helper.aleff_test_aleffy_reset.restype = None

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


def test_aleffy_can_resume_again_after_c_suffix_raises(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import ctypes
import sys
from aleff import aleffy, create_handler, effect

helper = ctypes.PyDLL(sys.argv[1])
helper.aleff_test_aleffy_call.argtypes = [ctypes.py_object]
helper.aleff_test_aleffy_call.restype = ctypes.py_object

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


def test_aleffy_rejects_nested_boundaries(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
from aleff import aleffy

outer = aleffy(lambda: aleffy(abs)(-1))
try:
    outer()
except RuntimeError as exc:
    assert str(exc) == "nested aleffy calls are not supported by the feasibility spike"
else:
    raise AssertionError("nested aleffy call was not rejected")
""",
    )
    assert result.returncode == 0, result.stderr


def test_aleffy_rejects_resume_from_another_thread(aleffy_helper: Path) -> None:
    result = _run_isolated(
        aleffy_helper,
        """
import ctypes
import sys
import threading
from aleff import aleffy, create_handler, effect

helper = ctypes.PyDLL(sys.argv[1])
helper.aleff_test_aleffy_call.argtypes = [ctypes.py_object]
helper.aleff_test_aleffy_call.restype = ctypes.py_object

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
import ctypes
import sys
from aleff import aleffy, create_handler, effect

helper = ctypes.PyDLL(sys.argv[1])
helper.aleff_test_aleffy_call.argtypes = [ctypes.py_object]
helper.aleff_test_aleffy_call.restype = ctypes.py_object
helper.aleff_test_aleffy_before_count.restype = ctypes.c_int
helper.aleff_test_aleffy_after_count.restype = ctypes.c_int

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
