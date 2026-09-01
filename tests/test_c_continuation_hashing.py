"""Continuation tests for hashing C APIs that consume buffer objects."""

from __future__ import annotations

from collections.abc import Callable
from functools import partial
import hashlib
import hmac
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
_HASH_CONSTRUCTORS = (
    "md5",
    "sha1",
    "sha224",
    "sha256",
    "sha384",
    "sha512",
    "sha3_224",
    "sha3_256",
    "sha3_384",
    "sha3_512",
    "shake_128",
    "shake_256",
    "blake2b",
    "blake2s",
)


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


def _digest(hash_object: Any) -> str:
    if hash_object.name.startswith("shake_"):
        return hash_object.hexdigest(16)
    return hash_object.hexdigest()


def _construct_hash(name: str, data: Any) -> Any:
    constructor = getattr(hashlib, name)
    if name == "md5":
        return constructor(data, usedforsecurity=False)
    return constructor(data)


class _EffectBuffer:
    def __init__(self, callback: Callable[[], Any]) -> None:
        self._callback = callback

    def __buffer__(self, _flags: int) -> memoryview:
        value = self._callback()
        if value == "raise":
            raise ExpectedBufferError("buffer acquisition failed")
        return cast(memoryview, value) if value == "invalid" else memoryview(value)


class _EffectBytearray(bytearray):
    def __new__(cls, callback: Callable[[], Any]) -> _EffectBytearray:
        return super().__new__(cls)

    def __init__(self, callback: Callable[[], Any]) -> None:
        super().__init__(b"placeholder")
        self._callback = callback

    def __buffer__(self, _flags: int) -> memoryview:
        value = self._callback()
        if value == "raise":
            raise ExpectedBufferError("buffer acquisition failed")
        return memoryview(value)


class _ReleaseEffectBuffer:
    def __init__(self, data: bytes, callback: Callable[[], Any]) -> None:
        self._data = data
        self._callback = callback

    def __buffer__(self, _flags: int) -> memoryview:
        return memoryview(self._data)

    def __release_buffer__(self, _view: memoryview) -> None:
        self._callback()


def _resume_against_fresh(
    run: Callable[[Callable[[], Any]], Any],
    fresh: Callable[[bytes], Any],
    decisions: tuple[bytes | str, ...],
) -> None:
    choose = effect("hash-buffer-choice")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[Outcome, Outcome]]:
        comparisons: list[tuple[Outcome, Outcome]] = []
        for decision in decisions:
            actual = _outcome(lambda decision=decision: k(decision))
            expected = _outcome(
                lambda decision=decision: (
                    fresh(decision) if isinstance(decision, bytes) else (_ for _ in ()).throw(ExpectedBufferError())
                )
            )
            comparisons.append((actual, expected))
        return comparisons

    comparisons = handler(lambda: run(choose))
    assert all(actual == expected for actual, expected in comparisons), comparisons


def _check_hash_constructor(name: str) -> None:
    decisions = (b"alpha", b"beta", b"")
    _resume_against_fresh(
        lambda choose: _digest(_construct_hash(name, _EffectBuffer(choose))),
        lambda data: _digest(_construct_hash(name, data)),
        decisions,
    )


for _name in _HASH_CONSTRUCTORS:
    _CASES[f"hashlib_{_name}_constructor_is_multishot_safe"] = (
        "normal",
        partial(_check_hash_constructor, _name),
    )


def _check_hash_constructor_keyword(name: str, keyword: str) -> None:
    constructor = getattr(hashlib, name)
    _resume_against_fresh(
        lambda choose: _digest(constructor(**{keyword: _EffectBuffer(choose)})),
        lambda data: _digest(constructor(**{keyword: data})),
        (b"alpha", b"beta", b""),
    )


for _name in _HASH_CONSTRUCTORS[:-2]:
    _primary_keyword = "string" if sys.version_info < (3, 13) else "data"
    _CASES[f"hashlib_{_name}_{_primary_keyword}_keyword_is_multishot_safe"] = (
        "normal",
        partial(_check_hash_constructor_keyword, _name, _primary_keyword),
    )
    if sys.version_info >= (3, 13):
        _CASES[f"hashlib_{_name}_string_keyword_is_multishot_safe"] = (
            "normal",
            partial(_check_hash_constructor_keyword, _name, "string"),
        )

if sys.version_info >= (3, 13):
    for _name in ("blake2b", "blake2s"):
        for _keyword in ("data", "string"):
            _CASES[f"hashlib_{_name}_{_keyword}_keyword_is_multishot_safe"] = (
                "normal",
                partial(_check_hash_constructor_keyword, _name, _keyword),
            )


@_case("normal", "hashlib_new_is_multishot_safe")
def _hashlib_new_is_multishot_safe() -> None:
    decisions = (b"alpha", b"beta", b"")
    _resume_against_fresh(
        lambda choose: _digest(hashlib.new("sha256", _EffectBuffer(choose))),
        lambda data: _digest(hashlib.new("sha256", data)),
        decisions,
    )


@_case("normal", "hashlib_new_data_keyword_is_multishot_safe")
def _hashlib_new_data_keyword_is_multishot_safe() -> None:
    _resume_against_fresh(
        lambda choose: _digest(hashlib.new("sha256", data=_EffectBuffer(choose))),
        lambda data: _digest(hashlib.new("sha256", data=data)),
        (b"alpha", b"beta", b""),
    )


@_case("normal", "hashlib_new_string_keyword_is_multishot_safe_when_available")
def _hashlib_new_string_keyword_is_multishot_safe_when_available() -> None:
    if sys.version_info < (3, 13):
        return
    _resume_against_fresh(
        lambda choose: _digest(hashlib.new("sha256", string=_EffectBuffer(choose))),
        lambda data: _digest(hashlib.new("sha256", string=data)),
        (b"alpha", b"beta", b""),
    )


def _check_hash_update(name: str) -> None:
    target = _construct_hash(name, b"seed")
    expected = _construct_hash(name, b"seed")
    choose = effect(f"{name}-update-choice")
    handler = create_handler(choose)
    decisions = (b"-a", b"-b", b"-c")

    def run() -> str:
        target.update(_EffectBuffer(choose))
        return _digest(target)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[str, str]]:
        comparisons: list[tuple[str, str]] = []
        for decision in decisions:
            actual = cast(str, k(decision))
            expected.update(decision)
            comparisons.append((actual, _digest(expected)))
        return comparisons

    comparisons = handler(run)
    assert all(actual == wanted for actual, wanted in comparisons), comparisons


for _name in _HASH_CONSTRUCTORS:
    _CASES[f"hashlib_{_name}_update_preserves_shared_state"] = (
        "normal",
        partial(_check_hash_update, _name),
    )


@_case("normal", "hmac_new_key_is_multishot_safe")
def _hmac_new_key_is_multishot_safe() -> None:
    key_decisions = (b"first-key", b"second-key", b"third-key")
    _resume_against_fresh(
        lambda choose: hmac.new(_EffectBytearray(choose), b"message", "sha256").hexdigest(),
        lambda data: hmac.new(bytearray(data), b"message", "sha256").hexdigest(),
        key_decisions,
    )


@_case("normal", "hmac_new_message_is_multishot_safe")
def _hmac_new_message_is_multishot_safe() -> None:
    _resume_against_fresh(
        lambda choose: hmac.new(b"key", _EffectBuffer(choose), "sha256").hexdigest(),
        lambda data: hmac.new(b"key", data, "sha256").hexdigest(),
        (b"first", b"second", b"third"),
    )


@_case("normal", "hmac_digest_key_is_multishot_safe")
def _hmac_digest_key_is_multishot_safe() -> None:
    key_decisions = (b"first-key", b"second-key", b"third-key")
    _resume_against_fresh(
        lambda choose: hmac.digest(_EffectBytearray(choose), b"message", "sha256"),
        lambda data: hmac.digest(bytearray(data), b"message", "sha256"),
        key_decisions,
    )


@_case("normal", "hmac_digest_message_is_multishot_safe")
def _hmac_digest_message_is_multishot_safe() -> None:
    _resume_against_fresh(
        lambda choose: hmac.digest(b"key", _EffectBuffer(choose), "sha256"),
        lambda data: hmac.digest(b"key", data, "sha256"),
        (b"first", b"second", b"third"),
    )


@_case("error", "hash_buffer_exceptions_are_isolated_per_shot")
def _hash_buffer_exceptions_are_isolated_per_shot() -> None:
    _resume_against_fresh(
        lambda choose: hashlib.sha256(_EffectBuffer(choose)).hexdigest(),
        lambda data: hashlib.sha256(data).hexdigest(),
        (b"first", "raise", b"second"),
    )


@_case("error", "hash_buffer_rejects_non_memoryview_callback_result")
def _hash_buffer_rejects_non_memoryview_callback_result() -> None:
    choose = effect("invalid-hash-buffer")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[Outcome]:
        return [_outcome(lambda: k("invalid")), _outcome(lambda: k(b"valid"))]

    assert handler(lambda: hashlib.sha256(_EffectBuffer(choose)).hexdigest()) == [
        ("raise", "TypeError"),
        ("return", hashlib.sha256(b"valid").hexdigest()),
    ]


def _check_blake2_keyword(name: str, keyword: str) -> None:
    constructor = getattr(hashlib, name)
    _resume_against_fresh(
        lambda choose: constructor(b"", **{keyword: _EffectBuffer(choose)}).hexdigest(),
        lambda data: constructor(b"", **{keyword: data}).hexdigest(),
        (b"a", b"value", b""),
    )


for _name in ("blake2b", "blake2s"):
    for _keyword in ("key", "salt", "person"):
        _CASES[f"hashlib_{_name}_{_keyword}_is_multishot_safe"] = (
            "corner",
            partial(_check_blake2_keyword, _name, _keyword),
        )


@_case("corner", "hashlib_releases_python_buffer_under_multishot_continuation")
def _hashlib_releases_python_buffer_under_multishot_continuation() -> None:
    release = effect("hash-buffer-release")
    handler = create_handler(release)
    callback_count = 0

    def released() -> Any:
        nonlocal callback_count
        callback_count += 1
        return release()

    @handler.on(release)
    def handle_release(k: Any) -> list[str]:
        return [cast(str, k(value)) for value in (None, None, None)]

    expected = hashlib.sha256(b"payload").hexdigest()
    assert handler(lambda: hashlib.sha256(_ReleaseEffectBuffer(b"payload", released)).hexdigest()) == [
        expected,
        expected,
        expected,
    ]
    assert callback_count == 1


@_case("corner", "hashlib_static_buffer_descriptor_is_multishot_safe")
def _hashlib_static_buffer_descriptor_is_multishot_safe() -> None:
    choose = effect("static-buffer-choice")
    handler = create_handler(choose)

    class StaticBuffer:
        @staticmethod
        def __buffer__(_flags: int) -> memoryview:
            return memoryview(choose())

    @handler.on(choose)
    def handle_choose(k: Any) -> list[str]:
        return [cast(str, k(data)) for data in (b"first", b"second", b"third")]

    assert handler(lambda: hashlib.sha256(StaticBuffer()).hexdigest()) == [
        hashlib.sha256(data).hexdigest() for data in (b"first", b"second", b"third")
    ]


def _run_case_in_subprocess(case_name: str) -> None:
    result = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), "--case", case_name],
        text=True,
        capture_output=True,
    )
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "normal"])
def test_hash_buffer_continuation(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "error"])
def test_hash_buffer_continuation_error(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "corner"])
def test_hash_buffer_continuation_corner_case(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case" or sys.argv[2] not in _CASES:
        raise SystemExit("usage: test_c_continuation_hashing.py --case CASE_NAME")
    _CASES[sys.argv[2]][1]()
