"""Continuation tests for ``binascii`` buffer-consuming C functions."""

from __future__ import annotations

import binascii
from collections.abc import Callable
from functools import partial
from pathlib import Path
import subprocess
import sys
from typing import Any, Literal, cast

import pytest

from aleff import create_handler, effect


CaseKind = Literal["normal", "error", "corner"]
Case = Callable[[], None]
Outcome = tuple[str, Any]
_CASES: dict[str, tuple[CaseKind, Case]] = {}


class ExpectedBufferError(Exception):
    pass


def _case(kind: CaseKind, name: str) -> Callable[[Case], Case]:
    def register(case: Case) -> Case:
        _CASES[name] = kind, case
        return case

    return register


def _outcome(call: Callable[[], Any]) -> Outcome:
    try:
        return "return", call()
    except Exception as exc:
        return "raise", type(exc).__name__


class _EffectBuffer:
    def __init__(self, callback: Callable[[], Any]) -> None:
        self._callback = callback

    def __buffer__(self, _flags: int) -> memoryview:
        value = self._callback()
        if value == "raise":
            raise ExpectedBufferError("buffer acquisition failed")
        return cast(memoryview, value) if value == "invalid" else memoryview(value)


class _ReleaseEffectBuffer:
    def __init__(self, data: bytes, callback: Callable[[], Any]) -> None:
        self._data = data
        self._callback = callback

    def __buffer__(self, _flags: int) -> memoryview:
        return memoryview(self._data)

    def __release_buffer__(self, _view: memoryview) -> None:
        self._callback()


def _resume_against_fresh(
    operation: Callable[[Any], Any],
    decisions: tuple[bytes | str, ...],
) -> None:
    choose = effect("binascii-buffer-choice")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[Outcome, Outcome]]:
        comparisons: list[tuple[Outcome, Outcome]] = []
        for decision in decisions:
            actual = _outcome(lambda decision=decision: k(decision))
            expected = _outcome(
                lambda decision=decision: (
                    operation(decision) if isinstance(decision, bytes) else (_ for _ in ()).throw(ExpectedBufferError())
                )
            )
            comparisons.append((actual, expected))
        return comparisons

    comparisons = handler(lambda: operation(_EffectBuffer(choose)))
    assert all(actual == expected for actual, expected in comparisons), comparisons


_OPERATIONS: tuple[tuple[str, Callable[[Any], Any], tuple[bytes, ...]], ...] = (
    ("a2b_base64", lambda data: binascii.a2b_base64(data), (b"QQ==", b"Qg==", b"")),
    ("a2b_hex", lambda data: binascii.a2b_hex(data), (b"41", b"42", b"")),
    ("a2b_qp", lambda data: binascii.a2b_qp(data), (b"=41", b"=42", b"")),
    (
        "a2b_uu",
        lambda data: binascii.a2b_uu(data),
        (binascii.b2a_uu(b"A"), binascii.b2a_uu(b"B"), binascii.b2a_uu(b"")),
    ),
    ("b2a_base64", lambda data: binascii.b2a_base64(data), (b"A", b"B", b"")),
    ("b2a_hex", lambda data: binascii.b2a_hex(data), (b"A", b"B", b"")),
    ("b2a_qp", lambda data: binascii.b2a_qp(data), (b"A A", b"B=B", b"")),
    ("b2a_uu", lambda data: binascii.b2a_uu(data), (b"A", b"B", b"")),
    ("crc32", lambda data: binascii.crc32(data), (b"A", b"B", b"")),
    ("crc_hqx", lambda data: binascii.crc_hqx(data, 7), (b"A", b"B", b"")),
    ("hexlify", lambda data: binascii.hexlify(data), (b"A", b"B", b"")),
    ("unhexlify", lambda data: binascii.unhexlify(data), (b"41", b"42", b"")),
)


for _name, _operation, _decisions in _OPERATIONS:
    _CASES[f"binascii_{_name}_is_multishot_safe"] = (
        "normal",
        partial(_resume_against_fresh, _operation, _decisions),
    )


@_case("error", "binascii_buffer_exceptions_are_isolated_per_shot")
def _binascii_buffer_exceptions_are_isolated_per_shot() -> None:
    _resume_against_fresh(binascii.hexlify, (b"first", "raise", b"second"))


@_case("error", "binascii_rejects_non_memoryview_callback_result")
def _binascii_rejects_non_memoryview_callback_result() -> None:
    choose = effect("invalid-binascii-buffer")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[Outcome]:
        return [_outcome(lambda: k("invalid")), _outcome(lambda: k(b"valid"))]

    assert handler(lambda: binascii.hexlify(_EffectBuffer(choose))) == [
        ("raise", "TypeError"),
        ("return", binascii.hexlify(b"valid")),
    ]


_OPTIONAL_OPERATIONS: tuple[tuple[str, Callable[[Any], Any], tuple[bytes, ...]], ...] = (
    (
        "a2b_base64_strict_mode",
        lambda data: binascii.a2b_base64(data, strict_mode=True),
        (b"QQ==", b"Qg==", b""),
    ),
    (
        "a2b_qp_header",
        lambda data: binascii.a2b_qp(data, header=True),
        (b"a_b", b"c_d", b""),
    ),
    (
        "b2a_base64_newline",
        lambda data: binascii.b2a_base64(data, newline=False),
        (b"A", b"B", b""),
    ),
    (
        "b2a_qp_options",
        lambda data: binascii.b2a_qp(data, quotetabs=True, istext=False, header=True),
        (b"A A", b"B=B", b""),
    ),
    (
        "b2a_uu_backtick",
        lambda data: binascii.b2a_uu(data, backtick=True),
        (b"A", b"B", b""),
    ),
    (
        "crc32_initial_value",
        lambda data: binascii.crc32(data, 123),
        (b"A", b"B", b""),
    ),
    (
        "hexlify_separator",
        lambda data: binascii.hexlify(data, b":", -2),
        (b"ABCD", b"EFGH", b""),
    ),
)

for _name, _operation, _decisions in _OPTIONAL_OPERATIONS:
    _CASES[f"binascii_{_name}_is_preserved_after_resume"] = (
        "corner",
        partial(_resume_against_fresh, _operation, _decisions),
    )


@_case("corner", "binascii_releases_python_buffer_under_multishot_continuation")
def _binascii_releases_python_buffer_under_multishot_continuation() -> None:
    release = effect("binascii-buffer-release")
    handler = create_handler(release)
    callback_count = 0

    def released() -> Any:
        nonlocal callback_count
        callback_count += 1
        return release()

    @handler.on(release)
    def handle_release(k: Any) -> list[bytes]:
        return [cast(bytes, k(None)) for _ in range(3)]

    expected = binascii.hexlify(b"payload")
    assert handler(lambda: binascii.hexlify(_ReleaseEffectBuffer(b"payload", released))) == [
        expected,
        expected,
        expected,
    ]
    assert callback_count == 1


def _run_case_in_subprocess(case_name: str) -> None:
    result = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), "--case", case_name],
        text=True,
        capture_output=True,
    )
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "normal"])
def test_binascii_buffer_continuation(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "error"])
def test_binascii_buffer_continuation_error(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "corner"])
def test_binascii_buffer_continuation_corner_case(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case" or sys.argv[2] not in _CASES:
        raise SystemExit("usage: test_c_continuation_binascii.py --case CASE_NAME")
    _CASES[sys.argv[2]][1]()
