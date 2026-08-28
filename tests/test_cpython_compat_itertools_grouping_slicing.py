"""CPython compatibility regression tests for issue #55 itertools adapters."""

from cpython_compat_support import assert_cpython_compatible


def test_groupby_normal_group_lifecycle_and_iterator_protocol() -> None:
    assert_cpython_compatible(
        r"""
import itertools


class Source:
    def __init__(self, values):
        self.values = iter(values)
        self.iter_calls = 0

    def __iter__(self):
        self.iter_calls += 1
        return self

    def __next__(self):
        return next(self.values)


source = Source([1, 1, 2, 2, 3])
groups = itertools.groupby(source)
print("iter_calls_after_constructor", source.iter_calls)
print("groups", [(key, list(group)) for key, group in groups])
print("iter_calls_after_iteration", source.iter_calls)
print("empty", [(key, list(group)) for key, group in itertools.groupby([])])
print("key_none", [(key, list(group)) for key, group in itertools.groupby([1, 1, 2], key=None)])
""".strip()
    )


def test_groupby_advancing_parent_invalidates_previous_grouper() -> None:
    assert_cpython_compatible(
        r"""
import itertools


groups = itertools.groupby([1, 1, 2, 2, 3])
first_key, first_group = next(groups)
second_key, second_group = next(groups)
print("parent_advance", first_key, second_key, list(first_group), list(second_group))
print("remaining", [(key, list(group)) for key, group in groups])
""".strip()
    )


def test_groupby_uses_cpython_key_comparison_direction_and_grouper_type() -> None:
    assert_cpython_compatible(
        r"""
import itertools


class Key:
    def __init__(self, name):
        self.name = name

    def __eq__(self, other):
        return isinstance(other, Key) and self.name == "A" and other.name == "B"


keys = (Key("A"), Key("B"))
groups = itertools.groupby([0, 1], lambda value: keys[value])
print("asymmetric", [(key.name, list(group)) for key, group in groups])

_, grouper = next(itertools.groupby([10]))
print("grouper_type", type(grouper).__module__, type(grouper).__name__)
""".strip()
    )


def test_groupby_keyword_construction_and_errors_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import itertools


def outcome(label, callback):
    try:
        value = callback()
    except BaseException as exc:
        print(label, "error", type(exc).__name__)
    else:
        print(label, "ok", value)


outcome("keyword", lambda: [(key, list(group)) for key, group in itertools.groupby(iterable=[1, 1, 2])])
outcome("missing", lambda: itertools.groupby())
outcome("too_many", lambda: itertools.groupby([], None, object()))
outcome("unexpected_keyword", lambda: itertools.groupby([], unknown=True))


class Bad:
    def __iter__(self):
        raise ValueError("iterated")


outcome("validation_precedes_iteration", lambda: itertools.groupby(Bad(), object(), object()))
""".strip()
    )


def test_islice_all_argument_forms_and_consumption_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import itertools


class Source:
    def __init__(self):
        self.next_value = 0
        self.iter_calls = 0

    def __iter__(self):
        self.iter_calls += 1
        return self

    def __next__(self):
        if self.next_value == 10:
            raise StopIteration
        value = self.next_value
        self.next_value += 1
        return value


source = Source()
print("iter_calls_after_constructor", source.iter_calls)
print("slice", list(itertools.islice(source, 2, 9, 3)))
print("source_position", source.next_value)
print("stop_only", list(itertools.islice(range(10), 4)))
print("start_stop", list(itertools.islice(range(10), 2, 5)))
print("start_stop_step", list(itertools.islice(range(10), 1, 9, 3)))
print("stop_none", list(itertools.islice(range(6), 2, None)))
print("zero", list(itertools.islice(range(6), 0)))
""".strip()
    )


def test_islice_invalid_bounds_and_argument_validation_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import itertools


def outcome(label, callback):
    try:
        value = callback()
    except BaseException as exc:
        print(label, "error", type(exc).__name__)
    else:
        print(label, "ok", type(value).__module__, type(value).__name__)


outcome("negative_start", lambda: itertools.islice(range(5), -1, 4))
outcome("negative_stop", lambda: itertools.islice(range(5), 1, -1))
outcome("zero_step", lambda: itertools.islice(range(5), 0, 4, 0))
outcome("negative_step", lambda: itertools.islice(range(5), 0, 4, -1))
outcome("none_stop_only", lambda: itertools.islice(range(5), None))
outcome("non_index_bound", lambda: itertools.islice(range(5), 1, "4"))
outcome("missing", lambda: itertools.islice())
outcome("too_many", lambda: itertools.islice([], 1, 2, 3, 4))


class Bad:
    def __iter__(self):
        raise ValueError("iterated")


outcome("validation_precedes_iteration", lambda: itertools.islice(Bad(), 1, 2, 3, 4))
""".strip()
    )


def test_pairwise_normal_empty_singleton_and_iterator_protocol_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import itertools


class Source:
    def __init__(self, values):
        self.values = iter(values)
        self.iter_calls = 0

    def __iter__(self):
        self.iter_calls += 1
        return self

    def __next__(self):
        return next(self.values)


source = Source([1, 2, 3])
pairs = itertools.pairwise(source)
print("iter_calls_after_constructor", source.iter_calls)
print("pairs", list(pairs))
print("iter_calls_after_iteration", source.iter_calls)
print("empty", list(itertools.pairwise([])))
print("singleton", list(itertools.pairwise([1])))
print("normal", list(itertools.pairwise([1, 2, 3, 4])))
""".strip()
    )


def test_pairwise_subclasses_and_argument_validation_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import itertools


def outcome(label, callback):
    try:
        value = callback()
    except BaseException as exc:
        print(label, "error", type(exc).__name__)
    else:
        print(label, "ok", value)


class PairwiseSubclass(itertools.pairwise):
    pass


outcome("subclass", lambda: list(PairwiseSubclass([1, 2, 3])))
outcome("missing", lambda: itertools.pairwise())
outcome("too_many", lambda: itertools.pairwise([], object()))


class Bad:
    def __iter__(self):
        raise ValueError("iterated")


outcome("validation_precedes_iteration", lambda: itertools.pairwise(Bad(), object()))
""".strip()
    )


def test_repeat_normal_identity_bounds_and_length_hint_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import itertools
import operator


element = []
values = list(itertools.repeat(element, 3))
element.append("changed")
print("identity", len(values), all(value is element for value in values), values)
print("zero", list(itertools.repeat("x", 0)))
print("negative", list(itertools.repeat("x", -2)))
print("keyword", list(itertools.repeat("x", times=2)))

finite = itertools.repeat("x", 3)
print("hint_initial", operator.length_hint(finite))
print("next", next(finite))
print("hint_after_next", operator.length_hint(finite))
print("rest", list(finite))
print("hint_exhausted", operator.length_hint(finite))
print("infinite_limited", list(itertools.islice(itertools.repeat("x"), 3)))
""".strip()
    )


def test_repeat_index_conversion_errors_and_subclasses_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
import itertools


def outcome(label, callback):
    try:
        value = callback()
    except BaseException as exc:
        print(label, "error", type(exc).__name__)
    else:
        print(label, "ok", value)


class InvalidIndex:
    def __index__(self):
        return "not an int"


class RepeatSubclass(itertools.repeat):
    pass


outcome("invalid_index", lambda: itertools.repeat("x", InvalidIndex()))
outcome("float", lambda: itertools.repeat("x", 1.5))
outcome("none", lambda: itertools.repeat("x", None))
outcome("overflow", lambda: itertools.repeat("x", 10**100))
outcome("subclass", lambda: next(RepeatSubclass("x", 1)))
outcome("unexpected_keyword", lambda: itertools.repeat("x", count=1))
""".strip()
    )
