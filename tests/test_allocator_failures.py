"""Regression tests for allocation failures in the continuation adapters.

The helper is deliberately a test-only shared library.  It installs a C
allocator wrapper, performs the operation while the allocator is failing, and
restores the original allocator before returning to Python.  This keeps a
pending ``MemoryError`` from contaminating the test process.
"""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import sysconfig
from typing import NamedTuple

import pytest

from aleff._multishot.v1 import _aleff  # pyright: ignore[reportPrivateUsage]

del _aleff  # importing the extension registers the continuation adapters


ROOT = Path(__file__).resolve().parents[1]
HELPER_SOURCE = Path(__file__).with_name("c_allocator_helper.c")


class AllocatorHelper(NamedTuple):
    library: ctypes.PyDLL
    path: Path


@pytest.fixture(scope="session")
def allocator_helper(tmp_path_factory: pytest.TempPathFactory) -> AllocatorHelper:
    """Build the test allocator wrapper and keep it isolated from the package."""

    if os.name == "nt":
        pytest.skip("the test allocator helper currently supports POSIX builds only")
    compiler = shutil.which(os.environ.get("CC", "cc"))
    include = sysconfig.get_path("include")
    if compiler is None or not (Path(include) / "Python.h").is_file():
        pytest.skip("a C compiler and Python development headers are required")

    output = tmp_path_factory.mktemp("allocator-helper") / "allocator_helper.so"
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
    if sys.platform == "darwin":
        command[6:6] = ["-undefined", "dynamic_lookup"]
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    if result.returncode != 0:
        pytest.fail(f"could not compile allocator helper:\n{result.stdout}\n{result.stderr}")

    helper = ctypes.PyDLL(str(output))
    helper.aleff_test_call_len.argtypes = [ctypes.py_object]
    helper.aleff_test_call_len.restype = ctypes.c_int
    helper.aleff_test_call_install.argtypes = [
        ctypes.c_void_p,
        ctypes.py_object,
        ctypes.py_object,
        ctypes.c_size_t,
    ]
    helper.aleff_test_call_install.restype = ctypes.c_int
    helper.aleff_test_call_hashing_install.argtypes = [
        ctypes.c_void_p,
        ctypes.py_object,
        ctypes.py_object,
        ctypes.py_object,
    ]
    helper.aleff_test_call_hashing_install.restype = ctypes.c_int
    helper.aleff_test_call_compression_install.argtypes = [
        ctypes.c_void_p,
        ctypes.py_object,
        ctypes.py_object,
        ctypes.py_object,
        ctypes.py_object,
    ]
    helper.aleff_test_call_compression_install.restype = ctypes.c_int
    return AllocatorHelper(helper, output)


def test_adapter_enter_allocation_failure_is_memory_error(allocator_helper: AllocatorHelper) -> None:
    """A failed adapter node allocation must be returned as ``MemoryError``."""

    status = allocator_helper.library.aleff_test_call_len([1])

    # status bits: error=1, NULL result=2, MemoryError=4, SystemError=8.
    assert status == 7, f"allocator failure status was {status}, expected MemoryError"


def test_allocator_is_restored_after_adapter_failure(allocator_helper: AllocatorHelper) -> None:
    """A failed call must not poison subsequent allocations or calls."""

    assert allocator_helper.library.aleff_test_call_len([1]) == 7
    assert len(["after", "failure"]) == 2

    with pytest.raises(TypeError):
        len(None)  # pyright: ignore[reportArgumentType]


def test_install_failure_rolls_back_all_mutations(allocator_helper: AllocatorHelper) -> None:
    """Bootstrap failure must leave builtins unchanged and permit a retry."""

    extension_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if not isinstance(extension_suffix, str):
        pytest.skip("the extension suffix is unavailable")
    extension = ROOT / "src/aleff/_multishot/v1" / f"_aleff{extension_suffix}"
    if not extension.is_file():
        pytest.skip("the in-place extension is not built")

    script = """
import builtins
import ctypes
import shutil
import sys
import tempfile

extension = sys.argv[1]
helper_path = sys.argv[2]
with tempfile.NamedTemporaryFile(suffix=".so") as copied:
    shutil.copyfile(extension, copied.name)
    library = ctypes.CDLL(copied.name)
    address = ctypes.cast(library.aleff_adapter_install, ctypes.c_void_p)
    helper = ctypes.PyDLL(helper_path)
    helper.aleff_test_call_install.argtypes = [
        ctypes.c_void_p, ctypes.py_object, ctypes.py_object, ctypes.c_size_t
    ]
    helper.aleff_test_call_install.restype = ctypes.c_int
    original_dir = builtins.dir
    dir_key = "dir"
    failed = helper.aleff_test_call_install(address, original_dir, dir_key, 0)
    unchanged = builtins.dir is original_dir
    retried = helper.aleff_test_call_install(
        address, original_dir, dir_key, (1 << 63) - 1
    )
    print(f"{failed} {int(unchanged)} {retried}")
"""
    result = subprocess.run(
        [
            sys.executable,
            "-X",
            "faulthandler",
            "-c",
            script,
            str(extension),
            str(allocator_helper.path),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "11 1 4"


@pytest.mark.parametrize(
    ("mode", "target"),
    [
        (0, "lambda: None"),
        (1, "lambda: None"),
        (2, "len"),
    ],
    ids=["callable-list", "callable-list-items", "c-function-array"],
)
def test_registry_allocation_failure_is_atomic_and_retryable(
    allocator_helper: AllocatorHelper,
    mode: int,
    target: str,
) -> None:
    """Each registry allocation must fail atomically and permit a retry."""

    extension_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if not isinstance(extension_suffix, str):
        pytest.skip("the extension suffix is unavailable")
    extension = ROOT / "src/aleff/_multishot/v1" / f"_aleff{extension_suffix}"
    if not extension.is_file():
        pytest.skip("the in-place extension is not built")

    script = f"""
import ctypes
import shutil
import sys
import tempfile

with tempfile.NamedTemporaryFile(suffix=".so") as copied:
    shutil.copyfile(sys.argv[1], copied.name)
    library = ctypes.CDLL(copied.name)
    helper = ctypes.PyDLL(sys.argv[2])
    call = helper.aleff_test_registry_allocation_failure
    call.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.py_object, ctypes.c_int,
    ]
    call.restype = ctypes.c_int
    status = call(
        ctypes.cast(library.aleff_adapter_register_callable, ctypes.c_void_p),
        ctypes.cast(library.aleff_adapter_callable_is_registered, ctypes.c_void_p),
        ctypes.cast(library.aleff_adapter_clear_registered_callables, ctypes.c_void_p),
        {target},
        {mode},
    )
print(status)
"""
    result = subprocess.run(
        [sys.executable, "-X", "faulthandler", "-c", script, str(extension), str(allocator_helper.path)],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "7"


def test_registry_failure_rolls_back_bootstrap_and_permits_retry(
    allocator_helper: AllocatorHelper,
) -> None:
    """A late registry failure must restore mutations and clear registry state."""

    extension_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if not isinstance(extension_suffix, str):
        pytest.skip("the extension suffix is unavailable")
    extension = ROOT / "src/aleff/_multishot/v1" / f"_aleff{extension_suffix}"
    if not extension.is_file():
        pytest.skip("the in-place extension is not built")

    script = """
import builtins
import ctypes
import operator
import shutil
import sys
import tempfile

with tempfile.NamedTemporaryFile(suffix=".so") as copied:
    shutil.copyfile(sys.argv[1], copied.name)
    library = ctypes.CDLL(copied.name)
    install = ctypes.cast(library.aleff_adapter_install, ctypes.c_void_p)
    helper = ctypes.PyDLL(sys.argv[2])
    fail = helper.aleff_test_call_install_registry_failure
    fail.argtypes = [
        ctypes.c_void_p, ctypes.py_object, ctypes.py_object, ctypes.py_object,
    ]
    fail.restype = ctypes.c_int
    retry = helper.aleff_test_call_install
    retry.argtypes = [
        ctypes.c_void_p, ctypes.py_object, ctypes.py_object, ctypes.c_size_t,
    ]
    retry.restype = ctypes.c_int
    original_dir = builtins.dir
    dir_key = "dir"
    failed = fail(install, operator.attrgetter, original_dir, dir_key)
    unchanged = builtins.dir is original_dir
    retried = retry(install, original_dir, dir_key, (1 << 63) - 1)
    print(f"{failed} {int(unchanged)} {retried}")
"""
    result = subprocess.run(
        [sys.executable, "-X", "faulthandler", "-c", script, str(extension), str(allocator_helper.path)],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "27 1 4"


def test_managed_registration_failure_precedes_install_and_permits_retry(
    allocator_helper: AllocatorHelper,
) -> None:
    """Managed-callable failure must precede mutations and clear registry state."""

    extension_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if not isinstance(extension_suffix, str):
        pytest.skip("the extension suffix is unavailable")
    extension = ROOT / "src/aleff/_multishot/v1" / f"_aleff{extension_suffix}"
    if not extension.is_file():
        pytest.skip("the in-place extension is not built")

    script = """
import builtins
import ctypes
import shutil
import sys
import tempfile
import types

module = types.SimpleNamespace(
    restore_continuation=len,
    restore_async_continuation=abs,
    _unsafe_call=hash,
)
with tempfile.NamedTemporaryFile(suffix=".so") as copied:
    shutil.copyfile(sys.argv[1], copied.name)
    library = ctypes.CDLL(copied.name)
    helper = ctypes.PyDLL(sys.argv[2])
    call = helper.aleff_test_call_managed_install_failure
    call.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.py_object, ctypes.py_object, ctypes.py_object, ctypes.py_object,
    ]
    call.restype = ctypes.c_int
    status = call(
        ctypes.cast(library.aleff_initialize_adapters, ctypes.c_void_p),
        ctypes.cast(library.aleff_adapter_register_callable, ctypes.c_void_p),
        ctypes.cast(library.aleff_adapter_callable_is_registered, ctypes.c_void_p),
        module,
        round,
        builtins.dir,
        "dir",
    )
print(status)
"""
    result = subprocess.run(
        [sys.executable, "-X", "faulthandler", "-c", script, str(extension), str(allocator_helper.path)],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "7"


@pytest.mark.parametrize(
    ("symbol", "modules", "tracked"),
    [
        (
            "aleff_test_call_hashing_install",
            "import hashlib, hmac",
            'type(hashlib.md5()).__dict__["update"]',
        ),
        (
            "aleff_test_call_compression_install",
            "import bz2, lzma, zlib",
            'type(zlib.compressobj()).__dict__["compress"]',
        ),
    ],
)
def test_method_install_allocation_failure_does_not_leak_descriptor(
    allocator_helper: AllocatorHelper,
    symbol: str,
    modules: str,
    tracked: str,
) -> None:
    """An uncommitted method adapter must not retain its descriptor."""

    extension_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if not isinstance(extension_suffix, str):
        pytest.skip("the extension suffix is unavailable")
    extension = ROOT / "src/aleff/_multishot/v1" / f"_aleff{extension_suffix}"
    if not extension.is_file():
        pytest.skip("the in-place extension is not built")

    script = f"""
import ctypes
import os
import shutil
import sys
import tempfile

{modules}

with tempfile.NamedTemporaryFile(suffix=".so") as copied:
    shutil.copyfile(sys.argv[1], copied.name)
    library = ctypes.CDLL(copied.name)
    installer_name = "adapter_" + sys.argv[3].removeprefix("aleff_test_call_")
    address = ctypes.cast(getattr(library, installer_name), ctypes.c_void_p)
    helper = ctypes.PyDLL(sys.argv[2])
    call = getattr(helper, sys.argv[3])
    if sys.argv[3] == "aleff_test_call_hashing_install":
        call.argtypes = [
            ctypes.c_void_p, ctypes.py_object, ctypes.py_object,
            ctypes.py_object,
        ]
        arguments = (address, hashlib, hmac, {tracked})
    else:
        call.argtypes = [
            ctypes.c_void_p, ctypes.py_object, ctypes.py_object,
            ctypes.py_object, ctypes.py_object,
        ]
        arguments = (address, zlib, bz2, lzma, {tracked})
    call.restype = ctypes.c_int
    status = call(*arguments)
os.write(1, str(status).encode("ascii"))
os._exit(0)
"""
    result = subprocess.run(
        [
            sys.executable,
            "-X",
            "faulthandler",
            "-c",
            script,
            str(extension),
            str(allocator_helper.path),
            symbol,
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )
    assert result.returncode == 0, result.stderr
    status = int(result.stdout)
    assert status & 1
    assert status & 2
    assert status & 4
    assert not status & 8
    assert status & 16
