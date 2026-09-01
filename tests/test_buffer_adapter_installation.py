"""Installation contracts for buffer-consumer continuation adapters."""

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def _run_isolated(source: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-c", source],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )


def test_missing_module_metadata_aborts_install_and_rolls_back() -> None:
    result = _run_isolated(
        r"""
import hashlib


class ModuleLookupError(Exception):
    pass


class MissingModuleCallable:
    def __init__(self, original):
        self.original = original

    def __call__(self, *args, **kwargs):
        return self.original(*args, **kwargs)

    def __getattribute__(self, name):
        if name == "__module__":
            raise ModuleLookupError("missing __module__")
        return object.__getattribute__(self, name)


names = ("new", "md5", "sha1", "sha224", "sha256")
expected = {name: getattr(hashlib, name) for name in names}
replacement = MissingModuleCallable(hashlib.sha256)
hashlib.sha256 = replacement
expected["sha256"] = replacement

try:
    import aleff
except ModuleLookupError as exc:
    print(type(exc).__name__, str(exc))
else:
    print("import succeeded")

print(all(getattr(hashlib, name) is expected[name] for name in names))
""".strip()
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == "ModuleLookupError missing __module__\nTrue\n"


def test_non_c_binascii_function_aborts_install_and_rolls_back() -> None:
    result = _run_isolated(
        r"""
import binascii
import hashlib


binascii_names = (
    "a2b_base64", "a2b_hex", "a2b_qp", "a2b_uu",
    "b2a_base64", "b2a_hex", "b2a_qp", "b2a_uu",
    "crc32", "crc_hqx", "hexlify", "unhexlify",
)
hashlib_names = ("new", "md5", "sha1", "sha256")
expected_binascii = {name: getattr(binascii, name) for name in binascii_names}
expected_hashlib = {name: getattr(hashlib, name) for name in hashlib_names}
replacement = lambda value: value
binascii.hexlify = replacement
expected_binascii["hexlify"] = replacement

try:
    import aleff
except RuntimeError as exc:
    print(type(exc).__name__, str(exc))
else:
    print("import succeeded")

print(all(getattr(binascii, name) is expected_binascii[name] for name in binascii_names))
print(all(getattr(hashlib, name) is expected_hashlib[name] for name in hashlib_names))
""".strip()
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == "RuntimeError binascii.hexlify is not a C function\nTrue\nTrue\n"
