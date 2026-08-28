"""CPython compatibility tests for list, tuple, and dict constructors."""

from textwrap import dedent

from cpython_compat_support import assert_cpython_compatible


def _assert_cpython_compatible(source: str) -> None:
    assert_cpython_compatible(dedent(source))


def test_list_tuple_dict_constructors_keep_normal_behavior() -> None:
    _assert_cpython_compatible(
        """
        class Pairs:
            def __iter__(self):
                return iter((("first", 1), ("second", 2)))

        print(list(range(3)))
        print(tuple(value * 2 for value in range(3)))
        print(dict(Pairs()))
        """
    )


def test_list_constructor_calls_len() -> None:
    _assert_cpython_compatible(
        """
        class Sized:
            def __init__(self):
                self.events = []

            def __iter__(self):
                return iter((1, 2))

            def __len__(self):
                self.events.append("len")
                return 2

        source = Sized()
        print(list(source), source.events)
        """
    )


def test_list_constructor_calls_length_hint() -> None:
    _assert_cpython_compatible(
        """
        class HintIterator:
            def __init__(self):
                self.index = 0
                self.events = []

            def __iter__(self):
                return self

            def __next__(self):
                if self.index == 2:
                    raise StopIteration
                value = self.index + 1
                self.index += 1
                return value

            def __length_hint__(self):
                self.events.append("length_hint")
                return 2

        source = HintIterator()
        print(list(source), source.events)
        """
    )


def test_list_length_hint_errors_are_reported() -> None:
    _assert_cpython_compatible(
        """
        class BrokenIterator:
            def __init__(self):
                self.index = 0

            def __iter__(self):
                return self

            def __next__(self):
                if self.index:
                    raise StopIteration
                self.index = 1
                return 1

            def __length_hint__(self):
                raise RuntimeError("length hint failed")

        try:
            list(BrokenIterator())
        except BaseException as exc:
            print(type(exc).__name__)
        else:
            print("returned")
        """
    )


def test_dict_constructor_uses_keys_attribute_for_mapping_detection() -> None:
    _assert_cpython_compatible(
        """
        class KeysOnly:
            def keys(self):
                return ("mapping_key",)

            def __iter__(self):
                return iter((("iterable_key", 1),))

        try:
            print("constructor", dict(KeysOnly()))
        except BaseException as exc:
            print("constructor", type(exc).__name__)

        target = {}
        try:
            dict.__init__(target, KeysOnly())
        except BaseException as exc:
            print("init", type(exc).__name__)
        else:
            print("init", target)
        """
    )


def test_dict_constructor_validates_each_pair_before_next_item() -> None:
    _assert_cpython_compatible(
        """
        events = []

        class Source:
            def __iter__(self):
                yield ("malformed",)
                events.append("consumed-after-error")
                yield ("later", 1)

        try:
            dict(Source())
        except BaseException as exc:
            print(type(exc).__name__, events)
        """
    )


def test_list_init_mutates_receiver_incrementally_on_failure() -> None:
    _assert_cpython_compatible(
        """
        target = [0]

        class Source:
            def __iter__(self):
                yield 1
                raise ValueError("boom")

        try:
            list.__init__(target, Source())
        except BaseException as exc:
            print(type(exc).__name__, target)
        """
    )


def test_dict_init_updates_existing_receiver_instead_of_clearing() -> None:
    _assert_cpython_compatible(
        """
        target = {"old": 0}
        result = dict.__init__(target, (("new", 1),))
        print(result, target)
        """
    )


def test_dict_init_mutates_receiver_incrementally_on_pair_error() -> None:
    _assert_cpython_compatible(
        """
        target = {"old": 0}

        class Source:
            def __iter__(self):
                yield ("new", 1)
                yield ("malformed",)

        try:
            dict.__init__(target, Source())
        except BaseException as exc:
            print(type(exc).__name__, target)
        """
    )


def test_init_validates_receiver_before_consuming_source() -> None:
    _assert_cpython_compatible(
        """
        events = []

        class Source:
            def __iter__(self):
                events.append("iterated")
                return iter(())

        for initializer in (list.__init__, dict.__init__):
            try:
                initializer(object(), Source())
            except BaseException as exc:
                print(initializer.__qualname__, type(exc).__name__, events)
            events.clear()
        """
    )


def test_constructor_errors_remain_standard() -> None:
    _assert_cpython_compatible(
        """
        class Exploding:
            def __iter__(self):
                raise LookupError("iteration failed")

        for constructor in (list, tuple, dict):
            try:
                constructor(Exploding())
            except BaseException as exc:
                print(constructor.__name__, type(exc).__name__)
        """
    )


def test_constructor_fast_paths_preserve_identity_contracts() -> None:
    _assert_cpython_compatible(
        """
        source_list = [1, 2]
        source_tuple = (1, 2)
        print(list(source_list) is source_list)
        print(tuple(source_tuple) is source_tuple)
        """
    )
