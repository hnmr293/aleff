"""Continuation tests for compression C APIs that consume buffers."""

from __future__ import annotations

import bz2
from collections.abc import Callable
import importlib
import lzma
from pathlib import Path
import subprocess
import sys
from typing import Any, Literal, cast
import zlib

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
    choose = effect("compression-buffer-choice")
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


@_case("normal", "zlib_adler32_is_multishot_safe")
def _zlib_adler32_is_multishot_safe() -> None:
    plain = (b"alpha" * 100, b"beta" * 100, b"")
    _resume_against_fresh(zlib.adler32, plain)


@_case("normal", "zlib_crc32_is_multishot_safe")
def _zlib_crc32_is_multishot_safe() -> None:
    _resume_against_fresh(zlib.crc32, (b"alpha" * 100, b"beta" * 100, b""))


@_case("normal", "zlib_compress_is_multishot_safe")
def _zlib_compress_is_multishot_safe() -> None:
    _resume_against_fresh(zlib.compress, (b"alpha" * 100, b"beta" * 100, b""))


@_case("normal", "zlib_decompress_is_multishot_safe")
def _zlib_decompress_is_multishot_safe() -> None:
    plain = (b"alpha" * 100, b"beta" * 100, b"")
    _resume_against_fresh(zlib.decompress, tuple(zlib.compress(data) for data in plain))


@_case("normal", "bz2_compress_is_multishot_safe")
def _bz2_compress_is_multishot_safe() -> None:
    _resume_against_fresh(bz2.compress, (b"alpha" * 100, b"beta" * 100, b""))


@_case("normal", "bz2_decompress_is_multishot_safe")
def _bz2_decompress_is_multishot_safe() -> None:
    plain = (b"alpha" * 100, b"beta" * 100, b"")
    _resume_against_fresh(bz2.decompress, tuple(bz2.compress(data) for data in plain))


@_case("normal", "lzma_compress_is_multishot_safe")
def _lzma_compress_is_multishot_safe() -> None:
    _resume_against_fresh(lzma.compress, (b"alpha" * 100, b"beta" * 100, b""))


@_case("normal", "lzma_decompress_is_multishot_safe")
def _lzma_decompress_is_multishot_safe() -> None:
    plain = (b"alpha" * 100, b"beta" * 100, b"")
    _resume_against_fresh(lzma.decompress, tuple(lzma.compress(data) for data in plain))


def _assert_stateful_method_matches_sequential_calls(
    actual_receiver: Any,
    expected_receiver: Any,
    operation: Callable[[Any, Any], Any],
    decisions: tuple[bytes, ...],
    label: str,
) -> None:
    choose = effect(f"{label}-choice")
    handler = create_handler(choose)

    def run() -> Any:
        return operation(actual_receiver, _EffectBuffer(choose))

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[Outcome, Outcome]]:
        comparisons: list[tuple[Outcome, Outcome]] = []
        for decision in decisions:
            actual = _outcome(lambda decision=decision: k(decision))
            expected = _outcome(lambda decision=decision: operation(expected_receiver, decision))
            comparisons.append((actual, expected))
        return comparisons

    comparisons = handler(run)
    assert all(actual == expected for actual, expected in comparisons), comparisons


@_case("normal", "zlib_compressor_preserves_shared_native_state")
def _zlib_compressor_preserves_shared_native_state() -> None:
    decisions = (b"alpha" * 100, b"beta" * 100, b"gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        zlib.compressobj(level=0),
        zlib.compressobj(level=0),
        lambda receiver, data: receiver.compress(data),
        decisions,
        "zlib-compressor",
    )


@_case("normal", "bz2_compressor_preserves_shared_native_state")
def _bz2_compressor_preserves_shared_native_state() -> None:
    decisions = (b"alpha" * 100, b"beta" * 100, b"gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        bz2.BZ2Compressor(),
        bz2.BZ2Compressor(),
        lambda receiver, data: receiver.compress(data),
        decisions,
        "bz2-compressor",
    )


@_case("normal", "lzma_compressor_preserves_shared_native_state")
def _lzma_compressor_preserves_shared_native_state() -> None:
    decisions = (b"alpha" * 100, b"beta" * 100, b"gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        lzma.LZMACompressor(),
        lzma.LZMACompressor(),
        lambda receiver, data: receiver.compress(data),
        decisions,
        "lzma-compressor",
    )


def _three_chunks(data: bytes) -> tuple[bytes, bytes, bytes]:
    first = max(1, len(data) // 3)
    second = max(first + 1, (len(data) * 2) // 3)
    return data[:first], data[first:second], data[second:]


@_case("normal", "zlib_decompressor_preserves_shared_native_state")
def _zlib_decompressor_preserves_shared_native_state() -> None:
    payload = b"alpha-beta-gamma" * 100
    _assert_stateful_method_matches_sequential_calls(
        zlib.decompressobj(),
        zlib.decompressobj(),
        lambda receiver, data: receiver.decompress(data),
        _three_chunks(zlib.compress(payload)),
        "zlib-decompressor",
    )


@_case("normal", "bz2_decompressor_preserves_shared_native_state")
def _bz2_decompressor_preserves_shared_native_state() -> None:
    payload = b"alpha-beta-gamma" * 100
    _assert_stateful_method_matches_sequential_calls(
        bz2.BZ2Decompressor(),
        bz2.BZ2Decompressor(),
        lambda receiver, data: receiver.decompress(data),
        _three_chunks(bz2.compress(payload)),
        "bz2-decompressor",
    )


@_case("normal", "lzma_decompressor_preserves_shared_native_state")
def _lzma_decompressor_preserves_shared_native_state() -> None:
    payload = b"alpha-beta-gamma" * 100
    _assert_stateful_method_matches_sequential_calls(
        lzma.LZMADecompressor(),
        lzma.LZMADecompressor(),
        lambda receiver, data: receiver.decompress(data),
        _three_chunks(lzma.compress(payload)),
        "lzma-decompressor",
    )


def _import_zstd() -> Any | None:
    if sys.version_info < (3, 14):
        return None
    try:
        return importlib.import_module("compression.zstd")
    except ModuleNotFoundError:
        return None


@_case("normal", "zstd_get_frame_size_is_multishot_safe_when_available")
def _zstd_get_frame_size_is_multishot_safe_when_available() -> None:
    zstd = _import_zstd()
    if zstd is None:
        return
    payloads = (b"alpha" * 100, b"beta" * 100, b"gamma" * 100)
    frames = tuple(zstd.compress(data) for data in payloads)
    _resume_against_fresh(zstd.get_frame_size, frames)


@_case("normal", "zstd_compressor_preserves_shared_native_state_when_available")
def _zstd_compressor_preserves_shared_native_state_when_available() -> None:
    zstd = _import_zstd()
    if zstd is None:
        return
    payloads = (b"alpha" * 100, b"beta" * 100, b"gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        zstd.ZstdCompressor(),
        zstd.ZstdCompressor(),
        lambda receiver, data: receiver.compress(data),
        payloads,
        "zstd-compressor",
    )


@_case("normal", "zstd_decompressor_preserves_shared_native_state_when_available")
def _zstd_decompressor_preserves_shared_native_state_when_available() -> None:
    zstd = _import_zstd()
    if zstd is None:
        return
    compressed = zstd.compress(b"alpha-beta-gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        zstd.ZstdDecompressor(),
        zstd.ZstdDecompressor(),
        lambda receiver, data: receiver.decompress(data),
        _three_chunks(compressed),
        "zstd-decompressor",
    )


@_case("error", "compression_buffer_exceptions_are_isolated_per_shot")
def _compression_buffer_exceptions_are_isolated_per_shot() -> None:
    _resume_against_fresh(zlib.compress, (b"first", "raise", b"second"))


@_case("error", "compression_rejects_non_memoryview_callback_result")
def _compression_rejects_non_memoryview_callback_result() -> None:
    choose = effect("invalid-compression-buffer")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[Outcome]:
        return [_outcome(lambda: k("invalid")), _outcome(lambda: k(b"valid"))]

    assert handler(lambda: zlib.compress(_EffectBuffer(choose))) == [
        ("raise", "TypeError"),
        ("return", zlib.compress(b"valid")),
    ]


@_case("corner", "zlib_adler32_preserves_initial_value_and_empty_buffers")
def _zlib_adler32_preserves_initial_value_and_empty_buffers() -> None:
    _resume_against_fresh(lambda data: zlib.adler32(data, 123), (b"", b"a", b"payload" * 20))


@_case("corner", "zlib_crc32_preserves_initial_value_and_empty_buffers")
def _zlib_crc32_preserves_initial_value_and_empty_buffers() -> None:
    _resume_against_fresh(lambda data: zlib.crc32(data, 123), (b"", b"a", b"payload" * 20))


@_case("corner", "zlib_compress_preserves_options_and_empty_buffers")
def _zlib_compress_preserves_options_and_empty_buffers() -> None:
    _resume_against_fresh(
        lambda data: zlib.compress(data, level=0, wbits=-15),
        (b"", b"a", b"payload" * 20),
    )


@_case("corner", "bz2_compress_preserves_level_and_empty_buffers")
def _bz2_compress_preserves_level_and_empty_buffers() -> None:
    _resume_against_fresh(
        lambda data: bz2.compress(data, compresslevel=1),
        (b"", b"a", b"payload" * 20),
    )


@_case("corner", "lzma_compress_preserves_options_and_empty_buffers")
def _lzma_compress_preserves_options_and_empty_buffers() -> None:
    _resume_against_fresh(
        lambda data: lzma.compress(data, format=lzma.FORMAT_XZ, preset=0),
        (b"", b"a", b"payload" * 20),
    )


@_case("corner", "zlib_decompressor_preserves_max_length")
def _zlib_decompressor_preserves_max_length() -> None:
    payload = zlib.compress(b"alpha-beta-gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        zlib.decompressobj(),
        zlib.decompressobj(),
        lambda receiver, data: receiver.decompress(data, 7),
        _three_chunks(payload),
        "zlib-decompressor-max-length",
    )


@_case("corner", "bz2_decompressor_preserves_max_length")
def _bz2_decompressor_preserves_max_length() -> None:
    payload = bz2.compress(b"alpha-beta-gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        bz2.BZ2Decompressor(),
        bz2.BZ2Decompressor(),
        lambda receiver, data: receiver.decompress(data, max_length=7),
        _three_chunks(payload),
        "bz2-decompressor-max-length",
    )


@_case("corner", "bz2_decompressor_accepts_data_keyword")
def _bz2_decompressor_accepts_data_keyword() -> None:
    payload = bz2.compress(b"alpha-beta-gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        bz2.BZ2Decompressor(),
        bz2.BZ2Decompressor(),
        lambda receiver, data: receiver.decompress(data=data),
        _three_chunks(payload),
        "bz2-decompressor-data-keyword",
    )


@_case("corner", "lzma_decompressor_preserves_max_length")
def _lzma_decompressor_preserves_max_length() -> None:
    payload = lzma.compress(b"alpha-beta-gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        lzma.LZMADecompressor(),
        lzma.LZMADecompressor(),
        lambda receiver, data: receiver.decompress(data, max_length=7),
        _three_chunks(payload),
        "lzma-decompressor-max-length",
    )


@_case("corner", "lzma_decompressor_accepts_data_keyword")
def _lzma_decompressor_accepts_data_keyword() -> None:
    payload = lzma.compress(b"alpha-beta-gamma" * 100)
    _assert_stateful_method_matches_sequential_calls(
        lzma.LZMADecompressor(),
        lzma.LZMADecompressor(),
        lambda receiver, data: receiver.decompress(data=data),
        _three_chunks(payload),
        "lzma-decompressor-data-keyword",
    )


@_case("corner", "compression_releases_python_buffer_under_multishot_continuation")
def _compression_releases_python_buffer_under_multishot_continuation() -> None:
    release = effect("compression-buffer-release")
    handler = create_handler(release)
    callback_count = 0

    def released() -> Any:
        nonlocal callback_count
        callback_count += 1
        return release()

    @handler.on(release)
    def handle_release(k: Any) -> list[int]:
        return [cast(int, k(None)) for _ in range(3)]

    expected = zlib.crc32(b"payload")
    assert handler(lambda: zlib.crc32(_ReleaseEffectBuffer(b"payload", released))) == [
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
def test_compression_buffer_continuation(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "error"])
def test_compression_buffer_continuation_error(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "corner"])
def test_compression_buffer_continuation_corner_case(case_name: str) -> None:
    _run_case_in_subprocess(case_name)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case" or sys.argv[2] not in _CASES:
        raise SystemExit("usage: test_c_continuation_compression.py --case CASE_NAME")
    _CASES[sys.argv[2]][1]()
