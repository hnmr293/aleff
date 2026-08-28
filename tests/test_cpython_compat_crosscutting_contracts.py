"""Cross-cutting CPython compatibility regression tests.

The snippets below are deliberately self-contained.  The compatibility helper
runs each one once in pristine CPython and once after importing ``aleff`` and
compares their deterministic output.
"""

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible


def _compatible(source: str) -> None:
    assert_cpython_compatible(dedent(source))


def test_builtin_argument_validation_matches_cpython() -> None:
    _compatible(
        """
        import operator

        def outcome(label, operation):
            try:
                value = operation()
            except BaseException as exc:
                print(label, "raise", type(exc).__name__, str(exc))
            else:
                print(label, "return", type(value).__name__, repr(value))

        outcome("print_bad_sep", lambda: print("x", sep=1, end=2))
        outcome("print_unknown", lambda: print("x", unexpected=1))
        outcome("list_sort_positional", lambda: [2, 1].sort(None))
        outcome("dict_get_keyword", lambda: {"x": 1}.get(key="x"))
        outcome("operator_call_no_object", lambda: operator.call())
        outcome("operator_call_keyword", lambda: operator.call(lambda **kw: kw, x=1))
        """
    )


def test_input_matches_cpython_for_custom_stream_lines() -> None:
    _compatible(
        """
        import sys

        class Stream:
            def __init__(self, value):
                self.value = value

            def readline(self):
                return self.value

        def read(value):
            old_stdin = sys.stdin
            sys.stdin = Stream(value)
            try:
                return ("return", repr(input()))
            except BaseException as exc:
                return ("raise", type(exc).__name__, str(exc))
            finally:
                sys.stdin = old_stdin

        print(read(b"bytes\\n"))
        print(read("carriage\\r\\n"))
        """
    )


def test_print_with_missing_stdout_matches_cpython() -> None:
    _compatible(
        """
        import sys

        old_stdout = sys.stdout
        sys.stdout = None
        try:
            try:
                print("hidden")
            except BaseException as exc:
                result = ("raise", type(exc).__name__, str(exc))
            else:
                result = ("return", None)
        finally:
            sys.stdout = old_stdout

        print(result)
        """
    )


def test_codec_error_handler_normal_path_matches_cpython() -> None:
    _compatible(
        """
        import codecs

        codecs.register_error("issue55", lambda exc: ("X\\x00Y", 1))
        try:
            value = b"\\xffA".decode("utf-8", "issue55")
        except BaseException as exc:
            print("raise", type(exc).__name__, str(exc))
        else:
            print("return", repr(value))
        """
    )


def test_dict_get_does_not_rehash_unrelated_existing_keys() -> None:
    _compatible(
        """
        class Key:
            fail = False

            def __hash__(self):
                if type(self).fail:
                    raise ValueError("unexpected rehash")
                return 0

        key = Key()
        values = {key: "value"}
        type(key).fail = True

        try:
            result = values.get("missing", "default")
        except BaseException as exc:
            print("raise", type(exc).__name__, str(exc))
        else:
            print("return", repr(result))
        """
    )


def test_list_sort_detects_callback_mutation_and_restores_list() -> None:
    _compatible(
        """
        values = [2, 1]

        def key(value):
            values.append(3)
            return value

        try:
            result = values.sort(key=key)
        except BaseException as exc:
            print("raise", type(exc).__name__, str(exc), repr(values))
        else:
            print("return", repr(result), repr(values))
        """
    )


def test_dict_update_preserves_partial_mutation_on_iterator_error() -> None:
    _compatible(
        """
        class Pairs:
            def __init__(self):
                self.index = 0

            def __iter__(self):
                return self

            def __next__(self):
                if self.index == 0:
                    self.index += 1
                    return ("first", 1)
                raise RuntimeError("iterator failed")

        values = {"existing": 0}
        try:
            values.update(Pairs())
        except BaseException as exc:
            print(type(exc).__name__, str(exc), sorted(values.items()))
        else:
            print("return", sorted(values.items()))
        """
    )


def test_takewhile_becomes_exhausted_after_first_false_result() -> None:
    _compatible(
        """
        import itertools

        calls = []

        def predicate(value):
            calls.append(value)
            return value < 2

        iterator = itertools.takewhile(predicate, [1, 2, 0])
        first = list(iterator)
        try:
            second = next(iterator)
        except BaseException as exc:
            second = (type(exc).__name__, str(exc))
        print(first, second, calls)
        """
    )


def test_frozenset_constructor_preserves_cpython_subclass_semantics() -> None:
    _compatible(
        """
        class FrozenSubclass(frozenset):
            def __iter__(self):
                return iter(("subclass-iterator",))

        exact_frozen = frozenset((1, 2))
        frozen_subclass = FrozenSubclass((1, 2))
        print(
            frozenset(exact_frozen) is exact_frozen,
            sorted(frozenset(frozen_subclass)),
        )
        """
    )


def test_operator_metadata_and_signatures_match_cpython() -> None:
    _compatible(
        """
        import inspect
        import operator

        for name in ("add", "call", "itemgetter"):
            function = getattr(operator, name)
            module = function.__module__
            module_info = (
                ("str", module)
                if isinstance(module, str)
                else (type(module).__name__, None)
            )
            try:
                signature = ("return", str(inspect.signature(function)))
            except BaseException as exc:
                signature = ("raise", type(exc).__name__)
            print(
                name,
                function.__name__,
                module_info,
                repr(getattr(function, "__text_signature__", None)),
                signature,
            )
        """
    )


def test_continuation_object_types_match_cpython() -> None:
    _compatible(
        """
        import functools
        import itertools

        tee_iterator = itertools.tee([1])[0]
        group_iterator = next(itertools.groupby([1]))[1]
        cache_decorator = functools.lru_cache(maxsize=1)
        key_factory = functools.cmp_to_key(int)

        def type_name(value):
            value_type = type(value)
            return value_type.__module__ + "." + value_type.__qualname__

        print(
            type_name(tee_iterator),
            type_name(group_iterator),
            type_name(cache_decorator),
            type_name(key_factory),
        )
        """
    )


def test_iterator_error_does_not_retain_consumed_item() -> None:
    _compatible(
        """
        import gc
        import weakref

        class Token:
            pass

        class FailingIterator:
            def __init__(self, item):
                self.item = item
                self.step = 0

            def __iter__(self):
                return self

            def __next__(self):
                if self.step == 0:
                    self.step = 1
                    return self.item
                raise RuntimeError("stop after one item")

        token = Token()
        reference = weakref.ref(token)
        source = FailingIterator(token)
        try:
            list(source)
        except RuntimeError:
            pass
        del token
        del source
        gc.collect()
        print(reference() is None)
        """
    )


def test_error_paths_leave_python_objects_usable() -> None:
    _compatible(
        """
        class Broken:
            def __iter__(self):
                return self

            def __next__(self):
                raise RuntimeError("broken iterator")

        try:
            bytes(Broken())
        except RuntimeError as exc:
            print(type(exc).__name__, str(exc))
        print(["after", {"error": True}], bytearray(b"ok"))
        """
    )
