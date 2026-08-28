"""CPython compatibility regression tests for string join and encoding."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any, cast

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def test_str_join_normal_values_and_iterables() -> None:
    assert_cpython_compatible(
        """
def report(name, operation):
    try:
        print(name, "ok", repr(operation()))
    except Exception as exc:
        print(name, "error", type(exc).__name__)


class Separator(str):
    pass


class Item(str):
    def __str__(self):
        raise AssertionError("join must not call __str__")


report("empty-list", lambda: ",".join([]))
report("empty-tuple", lambda: "".join(()))
report("singleton", lambda: "|".join(("only",)))
report("unicode", lambda: " / ".join(("左", "middle", "右")))
report("generator", lambda: ":".join(str(value) for value in range(4)))
report("separator-subclass", lambda: Separator("::").join(("a", "b", "c")))
report("item-subclass", lambda: "-".join((Item("a"), Item("b"))))
"""
    )


def test_str_join_errors_and_argument_validation() -> None:
    assert_cpython_compatible(
        """
def report(name, operation):
    try:
        print(name, "ok", repr(operation()))
    except Exception as exc:
        print(name, "error", type(exc).__name__)


class BadIterable:
    def __iter__(self):
        raise RuntimeError("iteration failed")


class FailingIterator:
    def __iter__(self):
        return self

    def __next__(self):
        raise ValueError("next failed")


report("bad-first-item", lambda: ",".join((1, "ok")))
report("bad-second-item", lambda: ",".join(("ok", object())))
report("iter-error", lambda: ",".join(BadIterable()))
report("next-error", lambda: ",".join(FailingIterator()))
report("missing-argument", lambda: str.join())
report("too-many-arguments", lambda: ",".join(("a",), ("b",)))
report("keyword-argument", lambda: ",".join(iterable=("a",)))
report("non-iterable", lambda: ",".join(None))
"""
    )


def test_str_encode_normal_values_and_error_handlers() -> None:
    assert_cpython_compatible(
        """
import codecs


def report(name, operation):
    try:
        print(name, "ok", repr(operation()))
    except Exception as exc:
        print(name, "error", type(exc).__name__)


def replacement(exc):
    return ("?", exc.end)


def bytes_replacement(exc):
    return (b"?", exc.end)


codecs.register_error("issue55_str_replacement", replacement)
codecs.register_error("issue55_bytes_replacement", bytes_replacement)
report("default", lambda: "café".encode())
report("utf8", lambda: "é".encode("utf-8"))
report("latin1", lambda: "é".encode(encoding="latin-1", errors="strict"))
report("empty", lambda: "".encode("ascii"))
report("ascii-replace", lambda: "é".encode("ascii", "replace"))
report("ascii-ignore", lambda: "é".encode("ascii", "ignore"))
report("ascii-backslash", lambda: "é".encode("ascii", "backslashreplace"))
report("ascii-name", lambda: "é".encode("ascii", "namereplace"))
report("ascii-xml", lambda: "é".encode("ascii", "xmlcharrefreplace"))
report("custom-str-replacement", lambda: "éX".encode("ascii", "issue55_str_replacement"))
report("custom-bytes-replacement", lambda: "éX".encode("ascii", "issue55_bytes_replacement"))
"""
    )


def test_str_encode_errors_and_argument_validation() -> None:
    assert_cpython_compatible(
        """
def report(name, operation):
    try:
        print(name, "ok", repr(operation()))
    except Exception as exc:
        print(name, "error", type(exc).__name__)


report("strict-error", lambda: "é".encode("ascii", "strict"))
report("none-encoding", lambda: "é".encode(None))
report("none-errors", lambda: "A".encode(errors=None))
report("integer-encoding", lambda: "A".encode(1))
report("integer-errors", lambda: "A".encode(errors=1))
report("unknown-encoding", lambda: "A".encode("issue55-no-such-encoding"))
report("unknown-errors", lambda: "é".encode("ascii", "issue55-no-such-errors"))
report("too-many-arguments", lambda: "A".encode("ascii", "strict", "extra"))
report("unexpected-keyword", lambda: "A".encode(codec="ascii"))
"""
    )


Choose = Callable[[], Any]


def _resume_outcomes(run: Callable[[Choose], Any], values: tuple[Any, ...]) -> list[Any]:
    choose = effect("issue55_choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle(k: Any) -> list[Any]:
        outcomes: list[Any] = []
        for value in values:
            try:
                outcomes.append(k(value))
            except Exception as exc:
                outcomes.append(type(exc).__name__)
        return outcomes

    return cast(list[Any], handler(lambda: run(choose)))


def test_str_join_resumes_effectful_iterable() -> None:
    def run(choose: Choose) -> str:
        class Items:
            def __iter__(self):
                return iter((choose(), "tail"))

        return ",".join(Items())

    assert _resume_outcomes(run, ("left", "right")) == ["left,tail", "right,tail"]


def test_str_encode_resume_continues_after_replacement() -> None:
    import codecs

    def run(choose: Choose) -> bytes:
        codecs.register_error(
            "issue55_encode_resume",
            lambda exc: (choose(), exc.end),  # pyright: ignore[reportAttributeAccessIssue, reportUnknownLambdaType, reportUnknownMemberType]
        )
        return "éX".encode("ascii", "issue55_encode_resume")

    assert _resume_outcomes(run, ("?", "!")) == [b"?X", b"!X"]


def test_str_encode_resume_accepts_bytes_replacement() -> None:
    import codecs

    def run(choose: Choose) -> bytes:
        codecs.register_error(
            "issue55_encode_bytes_resume",
            lambda exc: (choose(), exc.end),  # pyright: ignore[reportAttributeAccessIssue, reportUnknownLambdaType, reportUnknownMemberType]
        )
        return "éX".encode("ascii", "issue55_encode_bytes_resume")

    assert _resume_outcomes(run, (b"?", b"!")) == [b"?X", b"!X"]


def test_str_encode_resume_rejects_out_of_range_position() -> None:
    import codecs

    def run(choose: Choose) -> bytes:
        codecs.register_error(
            "issue55_encode_bad_position",
            lambda _exc: choose(),
        )
        return "éX".encode("ascii", "issue55_encode_bad_position")

    outcomes = _resume_outcomes(run, (("?", 10),))
    assert outcomes == ["IndexError"]
