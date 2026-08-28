"""CPython compatibility regressions for dictionary operations (Issue #55)."""

from __future__ import annotations

from collections.abc import Callable
import subprocess
import sys
import textwrap
from typing import Any, cast

from aleff import create_handler, effect
from cpython_compat_support import assert_cpython_compatible


def _source(body: str) -> str:
    return textwrap.dedent(body).strip()


def _effect_outcomes(
    run: Callable[[Callable[[], Any]], Any],
    values: tuple[Any, ...] = (True, False),
) -> list[tuple[str, Any]]:
    choose = effect("choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle_choose(k: Any) -> list[tuple[str, Any]]:
        outcomes: list[tuple[str, Any]] = []
        for value in values:
            try:
                outcomes.append(("return", k(value)))
            except Exception as exc:
                outcomes.append(("raise", type(exc).__name__))
        return outcomes

    return cast(list[tuple[str, Any]], handler(lambda: run(choose)))


_EFFECT_CASES: dict[str, Callable[[], None]] = {}


def _effect_case(name: str) -> Callable[[Callable[[], None]], Callable[[], None]]:
    def register(case: Callable[[], None]) -> Callable[[], None]:
        _EFFECT_CASES[name] = case
        return case

    return register


def _run_effect_case(name: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-X", "faulthandler", __file__, "--effect-case", name],
        text=True,
        capture_output=True,
        timeout=15,
        check=False,
    )


def test_dict_protocol_methods_normal_behavior() -> None:
    assert_cpython_compatible(
        _source(
            """
            d = {"first": 1, 2: "two"}
            print(dict.__getitem__(d, "first"))
            print(dict.__contains__(d, 2))
            print(dict.__contains__(d, "missing"))
            print(dict.__eq__(d, {"first": 1, 2: "two"}))
            print(dict.__ne__(d, {"first": 1, 2: "two"}))
            dict.__setitem__(d, "third", 3)
            dict.__delitem__(d, 2)
            print(d)
            """
        )
    )


def test_dict_protocol_methods_errors_and_corner_cases() -> None:
    assert_cpython_compatible(
        _source(
            """
            def outcome(callback):
                try:
                    value = callback()
                except BaseException as exc:
                    print(("raise", type(exc).__name__))
                else:
                    print(("return", type(value).__name__, value is NotImplemented))

            outcome(lambda: dict.__getitem__({}, "missing"))
            outcome(lambda: dict.__delitem__({}, "missing"))
            outcome(lambda: dict.__contains__({}, []))
            outcome(lambda: dict.__setitem__({}, [], 1))
            outcome(lambda: dict.__getitem__([], "key"))
            outcome(lambda: dict.__setitem__({}, "key"))
            outcome(lambda: dict.__eq__({}))
            outcome(lambda: dict.__ne__({}))
            outcome(lambda: dict.__eq__({}, []))
            """
        )
    )


def test_dict_protocol_methods_require_stored_hash_match() -> None:
    assert_cpython_compatible(
        _source(
            """
            class Stored:
                def __hash__(self):
                    return 0

            class Lookup:
                def __hash__(self):
                    return 1

                def __eq__(self, other):
                    return True

            def get_case():
                return dict.__getitem__({Stored(): "value"}, Lookup())

            def contains_case():
                return dict.__contains__({Stored(): "value"}, Lookup())

            def set_case():
                d = {Stored(): "value"}
                dict.__setitem__(d, Lookup(), "new")
                return (len(d), sorted(type(key).__name__ for key in d), sorted(d.values()))

            def delete_case():
                d = {Stored(): "value"}
                dict.__delitem__(d, Lookup())
                return d

            def outcome(callback):
                try:
                    print(("return", callback()))
                except BaseException as exc:
                    print(("raise", type(exc).__name__))

            outcome(get_case)
            outcome(contains_case)
            outcome(set_case)
            outcome(delete_case)
            """
        )
    )


def test_dict_protocol_methods_use_python_hash_for_integer_keys() -> None:
    assert_cpython_compatible(
        _source(
            """
            class Lookup:
                def __hash__(self):
                    return -2

                def __eq__(self, other):
                    return other == -1

            def outcome(callback):
                try:
                    print(("return", callback()))
                except BaseException as exc:
                    print(("raise", type(exc).__name__))

            outcome(lambda: dict.__getitem__({-1: "value"}, Lookup()))
            outcome(lambda: dict.__contains__({-1: "value"}, Lookup()))

            d = {-1: "old"}
            dict.__setitem__(d, Lookup(), "new")
            print((len(d), d.get(-1), sorted(type(key).__name__ for key in d)))

            d = {-1: "value"}
            outcome(lambda: dict.__delitem__(d, Lookup()))
            print(d)
            """
        )
    )


def test_dict_protocol_methods_do_not_overflow_on_large_integer_keys() -> None:
    assert_cpython_compatible(
        _source(
            """
            class Lookup:
                def __hash__(self):
                    return 0

            try:
                dict.__getitem__({10**100: "value"}, Lookup())
            except BaseException as exc:
                print(type(exc).__name__)
            """
        )
    )


def test_dict_get_preserves_stored_hashes_and_defaults() -> None:
    assert_cpython_compatible(
        _source(
            """
            class Key:
                def __init__(self, value):
                    self.value = value

                def __hash__(self):
                    return self.value

                def __eq__(self, other):
                    return True

            stored = Key(1)
            d = {stored: "value"}
            stored.value = 2
            print(d.get(Key(2), "missing"))

            print({}.get("missing"))
            print({}.get("missing", "default"))
            """
        )
    )


def test_dict_pop_normal_behavior_errors_and_defaults() -> None:
    assert_cpython_compatible(
        _source(
            """
            d = {"key": "value"}
            print(d.pop("key"))
            print(d)

            try:
                {}.pop("missing")
            except BaseException as exc:
                print(type(exc).__name__)
            print({}.pop("missing", "default"))
            try:
                {}.pop()
            except BaseException as exc:
                print(type(exc).__name__)
            """
        )
    )


def test_dict_equality_uses_stored_key_hashes() -> None:
    assert_cpython_compatible(
        _source(
            """
            class Key:
                def __init__(self):
                    self.value = 1

                def __hash__(self):
                    return self.value

                def __eq__(self, other):
                    return True

            key = Key()
            left = {key: "value"}
            right = {key: "value"}
            key.value = 2
            print(left == right)
            print(left != right)
            """
        )
    )


def test_dict_fromkeys_normal_behavior_and_errors() -> None:
    assert_cpython_compatible(
        _source(
            """
            print(dict.fromkeys(["a", "a", "b"], 7))

            def outcome(callback):
                try:
                    print(("return", callback()))
                except BaseException as exc:
                    print(("raise", type(exc).__name__))

            outcome(lambda: dict.fromkeys([[]]))
            outcome(lambda: dict.fromkeys(1))
            outcome(lambda: dict.fromkeys([1], 2, 3))
            """
        )
    )


def test_dict_subclass_fromkeys_returns_subclass() -> None:
    assert_cpython_compatible(
        _source(
            """
            class D(dict):
                pass

            print(type(D.fromkeys(["a"], 7)).__name__)
            """
        )
    )


def test_dict_update_normal_behavior_errors_and_input_shapes() -> None:
    assert_cpython_compatible(
        _source(
            """
            d = {"old": 0}
            d.update({"mapping": 1}, sequence=2)
            print(d)

            for item in ((("too",),), (("too", "many", "items"),)):
                try:
                    {}.update(item)
                except BaseException as exc:
                    print(type(exc).__name__)

            try:
                {}.update(("a", 1), ("b", 2))
            except BaseException as exc:
                print(type(exc).__name__)
            """
        )
    )


def test_dict_update_recognizes_keys_only_objects_as_mappings() -> None:
    assert_cpython_compatible(
        _source(
            """
            class KeysOnly:
                def keys(self):
                    return ("a",)

                def __iter__(self):
                    return iter((("wrong", 2),))

            target = {}
            try:
                target.update(KeysOnly())
            except BaseException as exc:
                print(("raise", type(exc).__name__, target))
            else:
                print(("return", target))
            """
        )
    )


def test_dict_update_inserts_each_sequence_item_before_requesting_next() -> None:
    assert_cpython_compatible(
        _source(
            """
            target = {}

            class Items:
                def __iter__(self):
                    yield ("a", 1)
                    if target != {"a": 1}:
                        raise RuntimeError("ordering")
                    yield ("b", 2)

            try:
                target.update(Items())
            except BaseException as exc:
                print((type(exc).__name__, target))
            else:
                print(("ok", target))
            """
        )
    )


@_effect_case("dict_item_equality_resume")
def _dict_item_equality_resume() -> None:
    def run(choose: Callable[[], Any]) -> str:
        class StoredKey:
            def __hash__(self) -> int:
                return 1

            def __eq__(self, _other: Any) -> bool:
                return cast(bool, choose())

        class LookupKey:
            def __hash__(self) -> int:
                return 1

        try:
            return dict.__getitem__({StoredKey(): "value"}, LookupKey())
        except KeyError:
            return "KeyError"

    assert _effect_outcomes(run) == [("return", "value"), ("return", "KeyError")]


@_effect_case("dict_equality_continues_after_resume")
def _dict_equality_continues_after_resume() -> None:
    def run(choose: Callable[[], Any]) -> bool:
        class Value:
            def __init__(self, key: str) -> None:
                self.key = key

            def __eq__(self, _other: Any) -> bool:
                return bool(choose()) if self.key == "first" else False

        return {"first": Value("first"), "second": Value("second")} == {
            "first": object(),
            "second": object(),
        }

    assert _effect_outcomes(run) == [("return", False), ("return", False)]


@_effect_case("dict_fromkeys_hash_resume")
def _dict_fromkeys_hash_resume() -> None:
    def run(choose: Callable[[], Any]) -> int:
        class Key:
            def __hash__(self) -> int:
                return cast(int, choose())

        return len(dict.fromkeys([Key()]))

    assert _effect_outcomes(run, (1, 10)) == [("return", 1), ("return", 1)]


@_effect_case("dict_update_hash_resume")
def _dict_update_hash_resume() -> None:
    def run(choose: Callable[[], Any]) -> int:
        class Key:
            def __hash__(self) -> int:
                return cast(int, choose())

        result: dict[Any, str] = {}
        result.update([(Key(), "value")])
        return len(result)

    assert _effect_outcomes(run, (1, 10)) == [("return", 1), ("return", 1)]


@_effect_case("dict_update_mapping_hash_resume")
def _dict_update_mapping_hash_resume() -> None:
    def run(choose: Callable[[], Any]) -> str:
        class Key:
            def __hash__(self) -> int:
                return cast(int, choose())

        class Mapping:
            def keys(self) -> list[Key]:
                return [Key()]

            def __getitem__(self, _key: Key) -> str:
                return "value"

        result: dict[Any, str] = {}
        result.update(Mapping())
        return next(iter(result.values()))

    assert _effect_outcomes(run, (1, 10)) == [("return", "value"), ("return", "value")]


def test_effectful_dict_cases_in_isolated_processes() -> None:
    failures: list[str] = []
    for name in _EFFECT_CASES:
        try:
            result = _run_effect_case(name)
        except subprocess.TimeoutExpired:
            failures.append(f"{name}: timed out after 15 seconds")
            continue
        if result.returncode != 0:
            failures.append(f"{name}:\n{result.stdout}{result.stderr}")
    assert not failures, "\n\n".join(failures)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--effect-case":
        raise SystemExit("usage: test_cpython_compat_dict_methods.py --effect-case NAME")
    _EFFECT_CASES[sys.argv[2]]()
