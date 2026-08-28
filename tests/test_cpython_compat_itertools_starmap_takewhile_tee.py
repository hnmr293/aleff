"""CPython compatibility coverage for starmap, takewhile, and tee."""

from cpython_compat_support import assert_cpython_compatible


def test_starmap_matches_cpython() -> None:
    assert_cpython_compatible(
        """
import itertools

def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__)

print("basic", list(itertools.starmap(lambda left, right: left + right, [(1, 2), (3, 4)])))
print("empty", list(itertools.starmap(lambda value: value, [])))
print("iterable_args", list(itertools.starmap(lambda *values: sum(values), [[1, 2], (3, 4)])))
print("bad_args", outcome(lambda: next(itertools.starmap(lambda value: value, [1]))))

def fail(value):
    if value == 1:
        raise ValueError("callback failure")
    return value

iterator = itertools.starmap(fail, [(1,), (2,)])
print("callback_error", outcome(lambda: next(iterator)))
print("after_callback_error", outcome(lambda: next(iterator)))

class Subclass(itertools.starmap):
    pass

def subclass_result():
    mapped = Subclass(lambda left, right: left * right, [(2, 3)])
    return type(mapped) is Subclass, list(mapped)

print("subclass", outcome(subclass_result))
"""
    )


def test_takewhile_matches_cpython() -> None:
    assert_cpython_compatible(
        """
import itertools

def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__)

print("basic", list(itertools.takewhile(lambda value: value < 3, [1, 2, 3, 4])))

calls = 0
def stateful_predicate(value):
    global calls
    calls += 1
    return calls > 1

terminal = itertools.takewhile(stateful_predicate, [1, 2])
print("terminal_first", outcome(lambda: next(terminal)))
print("terminal_second", outcome(lambda: next(terminal)))
print("terminal_calls", calls)

holder = []
def reentrant_predicate(value):
    if value == 1:
        return next(holder[0]) == 2
    return True

holder.append(itertools.takewhile(reentrant_predicate, [1, 2, 3]))
print("reentry", outcome(lambda: next(holder[0])))

calls = 0
def failing_predicate(value):
    global calls
    calls += 1
    if calls == 1:
        raise ValueError("predicate failure")
    return True

after_error = itertools.takewhile(failing_predicate, [1, 2])
print("predicate_error", outcome(lambda: next(after_error)))
print("after_predicate_error", outcome(lambda: next(after_error)))

stopped = itertools.takewhile(lambda value: True, [1])
print("normal_continuation", outcome(lambda: next(stopped)))

class BadTruth:
    def __bool__(self):
        raise LookupError("truth failure")

print("truth_error", outcome(lambda: next(itertools.takewhile(lambda value: BadTruth(), [1]))))

class Subclass(itertools.takewhile):
    pass

def subclass_result():
    filtered = Subclass(lambda value: value < 2, [1, 2])
    return type(filtered) is Subclass, list(filtered)

print("subclass", outcome(subclass_result))
"""
    )


def test_tee_matches_cpython() -> None:
    assert_cpython_compatible(
        """
import itertools

def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__)

first, second = itertools.tee(iter([1, 2, 3]), 2)
print("split", next(first), list(second), list(first))
print("counts", [(n, tuple(list(iterator) for iterator in itertools.tee((1, 2), n))) for n in (0, 1, 3)])
print("zero_noniterable", outcome(lambda: itertools.tee(42, 0)))
print("negative", outcome(lambda: itertools.tee((1,), -1)))
print("noniterable", outcome(lambda: itertools.tee(42, 1)))
print("keyword_iterable", outcome(lambda: len(itertools.tee(iterable=(1,)))))
print("keyword_n", outcome(lambda: len(itertools.tee((1,), n=1))))
"""
    )


def test_tee_preserves_native_function_type_and_lifecycle_contracts() -> None:
    assert_cpython_compatible(
        r"""
import copy
import gc
import inspect
import itertools
import pickle
import warnings
import weakref

warnings.simplefilter("ignore", DeprecationWarning)

def shape(value):
    return (
        type(value).__module__,
        type(value).__name__,
        repr(value).split(" object at ", 1)[0],
    )

def outcome(operation):
    try:
        value = operation()
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))
    return ("return", shape(value))

def status(operation):
    try:
        operation()
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))
    return ("return",)

def signature():
    try:
        return ("return", str(inspect.signature(itertools.tee)))
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))

print(
    "tee_function",
    type(itertools.tee).__module__,
    type(itertools.tee).__name__,
    itertools.tee.__module__,
    itertools.tee.__name__,
    itertools.tee.__doc__.splitlines()[0],
    repr(getattr(itertools.tee, "__text_signature__", None)),
)
print("signature", signature())

tee_iterator = itertools.tee(iter((1, 2)), 1)[0]
print("native", type(tee_iterator) is itertools._tee, shape(tee_iterator))
print("self_iterator", iter(tee_iterator) is tee_iterator)
print("gc_tracked", gc.is_tracked(tee_iterator))
print("weakref", status(lambda: weakref.ref(tee_iterator)))
print("copy", outcome(lambda: copy.copy(tee_iterator)))
print("deepcopy", outcome(lambda: copy.deepcopy(tee_iterator)))
print(
    "pickle",
    outcome(lambda: pickle.loads(pickle.dumps(tee_iterator, protocol=4))),
)

holder = []
class ReenteringSource:
    def __iter__(self):
        return self

    def __next__(self):
        next(holder[0])
        return 1

holder.append(itertools.tee(ReenteringSource(), 1)[0])
print("reentry", outcome(lambda: next(holder[0])))

class CyclicSource:
    def __iter__(self):
        return self

    def __next__(self):
        raise StopIteration

source = CyclicSource()
source.iterator = itertools.tee(source, 1)[0]
source_reference = weakref.ref(source)
del source
gc.collect()
print("cycle_collected", source_reference() is None)
"""
    )


def test_zip_longest_matches_cpython() -> None:
    assert_cpython_compatible(
        """
import itertools

def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__)

print("basic", list(itertools.zip_longest([1, 2], ["a"], fillvalue="F")))
print("empty", list(itertools.zip_longest()))
print("keyword_fill", list(itertools.zip_longest([1], [], fillvalue=None)))
print("keyword_only_fill", list(itertools.zip_longest(fillvalue=99)))
print("bad_iterable", outcome(lambda: itertools.zip_longest(42)))

class FailingSource:
    def __iter__(self):
        return self

    def __next__(self):
        raise ValueError("source failure")

print("source_error", outcome(lambda: next(itertools.zip_longest(FailingSource()))))

changed = itertools.zip_longest([1], [2, 3], fillvalue=0)
print("first", next(changed))
print("after_first", list(changed))

class Subclass(itertools.zip_longest):
    pass

def subclass_result():
    zipped = Subclass([1], [2, 3], fillvalue="F")
    return type(zipped) is Subclass, list(zipped)

print("subclass", outcome(subclass_result))
"""
    )
