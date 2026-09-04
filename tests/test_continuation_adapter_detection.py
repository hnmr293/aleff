"""Tests for identifying callables covered by continuation adapters."""

import bisect
import bz2
import codecs
import csv
import datetime
import functools
import gc
import heapq
import hashlib
import io
import json
import lzma
import marshal
import math
import operator
import pickle
import re
import struct
import subprocess
import sys
import time
import weakref
import zlib
from zoneinfo import ZoneInfo
from collections.abc import Callable

import pytest

from aleff._multishot.v1._aleff import (  # pyright: ignore[reportPrivateUsage]
    _has_continuation_adapter,
)


@functools.lru_cache
def _cached_function() -> None:
    return None


@pytest.mark.parametrize(
    "adapted",
    [
        pytest.param(len, id="builtin-function"),
        pytest.param(list, id="builtin-type"),
        pytest.param(list.extend, id="method-descriptor"),
        pytest.param([].extend, id="bound-builtin-method"),
        pytest.param(math.floor, id="numeric-module-function"),
        pytest.param(bisect.bisect_left, id="bisect-module-function"),
        pytest.param(heapq.heappush, id="heapq-module-function"),
        pytest.param(marshal.dumps, id="marshal-module-function"),
        pytest.param(pickle.dumps, id="python-module-wrapper"),
        pytest.param(struct.pack, id="struct-module-function"),
        pytest.param(codecs.encode, id="codecs-module-function"),
        pytest.param(operator.add, id="operator-module-function"),
        pytest.param(operator.attrgetter, id="callable-type"),
        pytest.param(operator.attrgetter("value"), id="callable-type-instance"),
        pytest.param(operator.itemgetter(0), id="second-callable-type-instance"),
        pytest.param(operator.methodcaller("clear"), id="third-callable-type-instance"),
        pytest.param(datetime.datetime.now, id="class-method-adapter"),
        pytest.param(re.compile("").sub, id="bound-method-adapter"),
        pytest.param(io.StringIO().readlines, id="inherited-bound-method-adapter"),
        pytest.param(json.dump, id="generated-python-wrapper"),
        pytest.param(_cached_function, id="callable-wrapper-instance"),
        pytest.param(functools.cache(lambda: None), id="unbounded-callable-wrapper-instance"),
        pytest.param(hashlib.sha256, id="generic-hashing-module-function"),
        pytest.param(zlib.compress, id="generic-compression-module-function"),
        pytest.param(io.BufferedReader.read, id="buffered-io-method"),
        pytest.param(io.TextIOWrapper.read, id="text-io-method"),
        pytest.param(type(hashlib.sha256()).update, id="hash-object-method"),
        pytest.param(type(zlib.compressobj()).compress, id="zlib-object-method"),
        pytest.param(type(bz2.BZ2Compressor()).compress, id="bz2-object-method"),
        pytest.param(type(lzma.LZMACompressor()).compress, id="lzma-object-method"),
        pytest.param(type(csv.writer(io.StringIO())).writerow, id="csv-writer-method"),
        pytest.param(ZoneInfo.from_file, id="zoneinfo-class-method"),
    ],
)
def test_registered_adapter_is_detected(adapted: Callable[..., object]) -> None:
    assert _has_continuation_adapter(adapted) is True


@pytest.mark.parametrize(
    "unsupported",
    [
        pytest.param(time.time, id="unregistered-c-function"),
        pytest.param(io.BytesIO().readlines, id="unadapted-shadowing-method"),
        pytest.param(io.BytesIO.read, id="unadapted-buffer-method"),
        pytest.param(lambda: None, id="python-function"),
        pytest.param(object(), id="non-callable-object"),
        pytest.param(None, id="none"),
    ],
)
def test_object_without_registered_adapter_is_not_detected(unsupported: object) -> None:
    assert _has_continuation_adapter(unsupported) is False


def test_all_aliases_of_registered_adapter_are_detected() -> None:
    assert bisect.bisect is bisect.bisect_right
    assert _has_continuation_adapter(bisect.bisect) is True
    assert _has_continuation_adapter(bisect.bisect_right) is True


def test_lru_cache_wrapper_created_before_aleff_import_is_not_detected() -> None:
    source = """
import functools

@functools.lru_cache
def cached():
    return None

from aleff._multishot.v1._aleff import _has_continuation_adapter
print(_has_continuation_adapter(cached))
"""
    result = subprocess.run(
        [sys.executable, "-c", source],
        check=True,
        capture_output=True,
        text=True,
    )

    assert result.stdout.strip() == "False"


def test_adapter_detection_does_not_retain_lru_cache_wrapper() -> None:
    def make_cached_function_reference() -> weakref.ReferenceType[Callable[..., object]]:
        @functools.lru_cache
        def cached() -> None:
            return None

        assert _has_continuation_adapter(cached) is True
        return weakref.ref(cached)

    reference = make_cached_function_reference()
    gc.collect()

    assert reference() is None
