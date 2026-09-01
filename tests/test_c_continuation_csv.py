"""Strict continuation tests for the :mod:`csv` C accelerator callbacks."""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Iterator
import csv
import inspect
from pathlib import Path
import subprocess
import sys
from typing import Any, Literal, cast

import pytest

from aleff import create_async_handler, create_handler, effect


Choose = Callable[[], Any]
Run = Callable[[Choose], Any]
Case = Callable[[], None]
Outcome = tuple[str, Any]
CaseKind = Literal["normal", "error", "corner"]
_CASES: dict[str, tuple[CaseKind, Case]] = {}


class ExpectedCallbackError(Exception):
    """An exception used to verify callback-exception restoration."""


def _case(kind: CaseKind, name: str) -> Callable[[Case], Case]:
    def register(case: Case) -> Case:
        _CASES[name] = (kind, case)
        return case

    return register


def _outcome(call: Callable[[], Any]) -> Outcome:
    try:
        return "return", call()
    except Exception as exc:
        return "raise", (type(exc).__name__, str(exc))


def _callback(value: Any, operation: str) -> Any:
    if value == "raise":
        raise ExpectedCallbackError(f"{operation} callback failed")
    return value


def _resume_against_fresh(
    run: Run,
    fresh: Callable[[Any], Any],
    decisions: tuple[Any, ...],
    effect_name: str,
) -> list[Outcome]:
    """Compare every multi-shot resume with a new ordinary execution."""

    choose = effect(effect_name)
    handler = create_handler(choose)
    suspension_count = 0

    @handler.on(choose)
    def resume(k: Any) -> list[Outcome]:
        nonlocal suspension_count
        suspension_count += 1
        outcomes: list[Outcome] = []
        for decision in decisions:
            actual = _outcome(lambda decision=decision: k(decision))
            expected = _outcome(lambda decision=decision: fresh(decision))
            assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
            outcomes.append(actual)
        return outcomes

    result = handler(lambda: run(choose))
    assert suspension_count == 1, "the scenario must suspend at exactly one callback"
    return cast(list[Outcome], result)


def _reader_next(callback: Callable[[], Any]) -> tuple[Any, int, int]:
    class Lines:
        def __init__(self) -> None:
            self.calls = 0

        def __iter__(self) -> "Lines":
            return self

        def __next__(self) -> Any:
            self.calls += 1
            result = callback()
            self.calls = 1
            return result

    source = Lines()
    reader = csv.reader(source)
    result = reader.__next__()
    return result, reader.line_num, source.calls


@_case("normal", "reader_next_multishot_compares_with_fresh_execution")
def _reader_next_multishot_compares_with_fresh_execution() -> None:
    decisions = ("alpha,1", "beta,2", "alpha,1")
    outcomes = _resume_against_fresh(
        lambda choose: _reader_next(lambda: _callback(choose(), "reader")),
        lambda decision: _reader_next(lambda: _callback(decision, "reader")),
        decisions,
        "csv-reader-next",
    )
    assert outcomes[0] == outcomes[2]
    assert outcomes[0] != outcomes[1]
    assert outcomes[0] == ("return", (["alpha", "1"], 1, 1))


@_case("error", "reader_next_callback_errors_are_isolated_per_shot")
def _reader_next_callback_errors_are_isolated_per_shot() -> None:
    outcomes = _resume_against_fresh(
        lambda choose: _reader_next(lambda: _callback(choose(), "reader")),
        lambda decision: _reader_next(lambda: _callback(decision, "reader")),
        ("raise", b"not text", "gamma,3", "raise"),
        "csv-reader-next-error",
    )
    assert outcomes[0] == ("raise", ("ExpectedCallbackError", "reader callback failed"))
    assert outcomes[1][0] == "raise"
    assert outcomes[2] == ("return", (["gamma", "3"], 1, 1))
    assert outcomes[0] == outcomes[3]


@_case("corner", "reader_next_empty_and_quoted_records_are_shot_stable")
def _reader_next_empty_and_quoted_records_are_shot_stable() -> None:
    decisions = ("", '"left\nright",tail', "")
    outcomes = _resume_against_fresh(
        lambda choose: _reader_next(lambda: _callback(choose(), "reader")),
        lambda decision: _reader_next(lambda: _callback(decision, "reader")),
        decisions,
        "csv-reader-next-corner",
    )
    assert outcomes == [
        ("return", ([], 1, 1)),
        ("return", (["left\nright", "tail"], 1, 1)),
        ("return", ([], 1, 1)),
    ]


def _writerow(callback: Callable[[], Any], row: list[Any]) -> tuple[Any, tuple[str, ...], int]:
    writes: list[str] = []

    class Sink:
        def write(self, data: str) -> Any:
            writes.append(data)
            result = callback()
            writes[:] = [data]
            return result

    writer = csv.writer(Sink(), lineterminator="\n")
    result = writer.writerow(row)
    return result, tuple(writes), len(writes)


@_case("normal", "writerow_write_callback_multishot_compares_with_fresh_execution")
def _writerow_write_callback_multishot_compares_with_fresh_execution() -> None:
    row = ["alpha", 1, None]
    outcomes = _resume_against_fresh(
        lambda choose: _writerow(lambda: _callback(choose(), "writerow"), row),
        lambda decision: _writerow(lambda: _callback(decision, "writerow"), row),
        (9, 0, 9),
        "csv-writerow-write",
    )
    assert outcomes[0] == outcomes[2]
    assert outcomes[0] != outcomes[1]
    assert outcomes[0] == ("return", (9, ("alpha,1,\n",), 1))


@_case("error", "writerow_write_callback_errors_are_isolated_per_shot")
def _writerow_write_callback_errors_are_isolated_per_shot() -> None:
    row = ["error", "case"]
    outcomes = _resume_against_fresh(
        lambda choose: _writerow(lambda: _callback(choose(), "writerow"), row),
        lambda decision: _writerow(lambda: _callback(decision, "writerow"), row),
        ("raise", 1, "raise"),
        "csv-writerow-write-error",
    )
    assert outcomes[0] == ("raise", ("ExpectedCallbackError", "writerow callback failed"))
    assert outcomes[1] == ("return", (1, ("error,case\n",), 1))
    assert outcomes[2] == outcomes[0]


def _writer_row_protocol(
    operation: str,
    callback_kind: str,
    callback: Callable[[], Any],
) -> tuple[Any, tuple[str, ...]]:
    writes: list[str] = []

    class Sink:
        def write(self, data: str) -> int:
            writes[:] = [data]
            return len(data)

    class Field:
        def __str__(self) -> str:
            if callback_kind != "field_str":
                return "fixed"
            return cast(str, _callback(callback(), f"{operation} field.__str__"))

    class Row:
        def __init__(self) -> None:
            self.used = False

        def __iter__(self) -> Iterator[Any]:
            if callback_kind == "row_iter":
                values = cast(tuple[Any, ...], _callback(callback(), f"{operation} row.__iter__"))
                return iter(values)
            return self

        def __next__(self) -> Any:
            if self.used:
                raise StopIteration
            self.used = True
            if callback_kind == "row_next":
                return _callback(callback(), f"{operation} row.__next__")
            return Field()

    writer = csv.writer(Sink(), lineterminator="\n")
    if operation == "writerow":
        result = writer.writerow(Row())
    elif operation == "writerows":
        result = writer.writerows([Row()])
    else:
        raise AssertionError(operation)
    return result, tuple(writes)


def _writer_row_protocol_case(operation: str, callback_kind: str) -> None:
    callback_label = {
        "row_iter": "row.__iter__",
        "row_next": "row.__next__",
        "field_str": "field.__str__",
    }[callback_kind]
    if callback_kind == "row_iter":
        decisions: tuple[Any, ...] = (("alpha", 1), (), "raise", ("alpha", 1))
    else:
        decisions = ("alpha", "", "raise", "alpha")

    outcomes = _resume_against_fresh(
        lambda choose: _writer_row_protocol(operation, callback_kind, choose),
        lambda decision: _writer_row_protocol(operation, callback_kind, lambda: decision),
        decisions,
        f"csv-{operation}-{callback_kind}",
    )
    assert outcomes[0][0] == "return"
    assert outcomes[1][0] == "return"
    assert outcomes[2] == (
        "raise",
        ("ExpectedCallbackError", f"{operation} {callback_label} callback failed"),
    )
    assert outcomes[0] == outcomes[3]
    assert outcomes[0] != outcomes[1]


for _operation in ("writerow", "writerows"):
    for _callback_kind in ("row_iter", "row_next", "field_str"):
        _case(
            "corner",
            f"{_operation}_{_callback_kind}_callback_multishot_matches_fresh_execution",
        )(
            lambda operation=_operation, callback_kind=_callback_kind: _writer_row_protocol_case(
                operation,
                callback_kind,
            )
        )


def _writer_stop_protocol(
    callback_kind: str,
    callback: Callable[[], Any],
) -> tuple[Any, tuple[str, ...]]:
    writes: list[str] = []

    class Sink:
        def write(self, data: str) -> int:
            writes[:] = [data]
            return len(data)

    class Row:
        def __init__(self) -> None:
            self.done = False

        def __iter__(self) -> "Row":
            return self

        def __next__(self) -> str:
            if self.done:
                raise StopIteration
            self.done = True
            decision = callback()
            if decision == "stop":
                raise StopIteration
            return cast(str, _callback(decision, "row.__next__"))

    class Rows:
        def __init__(self) -> None:
            self.done = False

        def __iter__(self) -> "Rows":
            return self

        def __next__(self) -> Any:
            if self.done:
                raise StopIteration
            self.done = True
            if callback_kind == "rows_next":
                decision = callback()
                if decision == "stop":
                    raise StopIteration
                return decision
            return Row()

    writer = csv.writer(Sink(), lineterminator="\n")
    if callback_kind == "rows_next":
        result = writer.writerows(Rows())
    else:
        result = writer.writerow(Row())
    return result, tuple(writes)


for _callback_kind in ("rows_next", "row_next"):

    @_case("corner", f"writer_{_callback_kind}_stop_iteration_multishot")
    def _writer_stop_iteration(
        callback_kind: str = _callback_kind,
    ) -> None:
        outcomes = _resume_against_fresh(
            lambda choose: _writer_stop_protocol(callback_kind, choose),
            lambda decision: _writer_stop_protocol(
                callback_kind,
                lambda: decision,
            ),
            ("stop", "stop"),
            f"csv-writer-{callback_kind}-stop",
        )
        assert outcomes[0][0] == "return"
        assert outcomes[0] == outcomes[1]


@_case("error", "writerow_resumed_row_iter_typeerror_is_csv_error")
def _writerow_resumed_row_iter_typeerror_is_csv_error() -> None:
    def call(callback: Callable[[], Any]) -> Any:
        class Sink:
            def write(self, value: str) -> int:
                return len(value)

        class Row:
            def __iter__(self) -> Iterator[Any]:
                decision = callback()
                if decision == "typeerror":
                    raise TypeError("row iteration failed")
                return iter(cast(tuple[Any, ...], decision))

        return csv.writer(Sink()).writerow(Row())

    outcomes = _resume_against_fresh(
        lambda choose: call(choose),
        lambda decision: call(lambda: decision),
        ("typeerror", ("ok",), "typeerror"),
        "csv-writerow-row-iter-typeerror",
    )
    assert outcomes[0][0] == "raise"
    assert outcomes[0][1][0] == "Error"
    assert outcomes[1][0] == "return"
    assert outcomes[0] == outcomes[2]


def _writerows(
    callback: Callable[[], Any],
    callback_index: int,
    rows: list[list[Any]],
) -> tuple[Any, tuple[str, ...], int]:
    writes: list[str] = []
    calls = 0

    class Sink:
        def write(self, data: str) -> Any:
            nonlocal calls
            calls += 1
            writes.append(data)
            if calls == callback_index:
                prefix = tuple(writes)
                result = callback()
                calls = callback_index
                writes[:] = prefix
                return result
            return len(data)

    writer = csv.writer(Sink(), lineterminator="\n")
    result = writer.writerows(rows)
    return result, tuple(writes), calls


def _writerows_case(callback_index: int, decisions: tuple[Any, ...], rows: list[list[Any]]) -> list[Outcome]:
    return _resume_against_fresh(
        lambda choose: _writerows(
            lambda: _callback(choose(), "writerows"),
            callback_index,
            rows,
        ),
        lambda decision: _writerows(
            lambda: _callback(decision, "writerows"),
            callback_index,
            rows,
        ),
        decisions,
        f"csv-writerows-write-{callback_index}",
    )


for _callback_index in (1, 2, 3):

    @_case("normal", f"writerows_write_callback_{_callback_index}_multishot")
    def _writerows_normal(
        callback_index: int = _callback_index,
    ) -> None:
        rows = [["one", 1], ["two", 2], ["three", 3]]
        outcomes = _writerows_case(callback_index, (7, 0, 7), rows)
        expected = ("return", (None, ("one,1\n", "two,2\n", "three,3\n"), 3))
        assert outcomes == [expected, expected, expected]


for _callback_index in (1, 2, 3):

    @_case("error", f"writerows_write_callback_{_callback_index}_error_is_isolated")
    def _writerows_error(
        callback_index: int = _callback_index,
    ) -> None:
        rows = [["one"], ["two"], ["three"]]
        outcomes = _writerows_case(callback_index, ("raise", 4, "raise"), rows)
        expected_error = (
            "raise",
            ("ExpectedCallbackError", "writerows callback failed"),
        )
        assert outcomes[0] == outcomes[2] == expected_error
        assert outcomes[1] == (
            "return",
            (None, ("one\n", "two\n", "three\n"), 3),
        )


@_case("corner", "writerows_final_write_with_empty_row_is_shot_stable")
def _writerows_final_write_with_empty_row_is_shot_stable() -> None:
    rows = [["one", 1], [], ["three", 3]]
    outcomes = _writerows_case(3, (0, 1, 0), rows)
    assert outcomes == [
        ("return", (None, ("one,1\n", "\n", "three,3\n"), 3)),
        ("return", (None, ("one,1\n", "\n", "three,3\n"), 3)),
        ("return", (None, ("one,1\n", "\n", "three,3\n"), 3)),
    ]


async def _async_outcome(call: Callable[[], Any]) -> Outcome:
    try:
        value = call()
        if inspect.isawaitable(value):
            value = await value
        return "return", value
    except Exception as exc:
        return "raise", (type(exc).__name__, str(exc))


@_case("error", "async_writerow_write_callback_isolated_against_fresh_execution")
def _async_writerow_write_callback_isolated_against_fresh_execution() -> None:
    row = ["async", "csv"]

    async def exercise() -> list[Outcome]:
        choose = effect("csv-async-writerow-write")
        handler = create_async_handler(choose)
        suspension_count = 0

        async def run() -> Any:
            return _writerow(lambda: _callback(choose(), "async writerow"), row)

        @handler.on(choose)
        async def resume(k: Any) -> list[Outcome]:
            nonlocal suspension_count
            suspension_count += 1
            outcomes: list[Outcome] = []
            for decision in ("raise", 2, "raise", 2):
                actual = await _async_outcome(lambda decision=decision: k(decision))
                expected = _outcome(
                    lambda decision=decision: _writerow(
                        lambda: _callback(decision, "async writerow"),
                        row,
                    )
                )
                assert actual == expected, f"decision {decision!r}: actual={actual!r}, expected={expected!r}"
                outcomes.append(actual)
            return outcomes

        result = await handler(run)
        assert suspension_count == 1
        return cast(list[Outcome], result)

    outcomes = asyncio.run(exercise())
    assert outcomes[0][0] == "raise"
    assert outcomes[1][0] == "return"
    assert outcomes[0] == outcomes[2]
    assert outcomes[1] == outcomes[3]


def _run_case(case_name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", str(Path(__file__).resolve()), "--case", case_name],
        text=True,
        capture_output=True,
        timeout=15,
        check=False,
    )


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "normal"])
def test_csv_continuation_normal(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "error"])
def test_csv_continuation_error_isolated_per_shot(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


@pytest.mark.parametrize("case_name", [name for name, (kind, _) in _CASES.items() if kind == "corner"])
def test_csv_continuation_corner(case_name: str) -> None:
    result = _run_case(case_name)
    assert result.returncode == 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--case":
        raise SystemExit("usage: test_c_continuation_csv.py --case CASE_NAME")
    _CASES[sys.argv[2]][1]()
