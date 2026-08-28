"""Differential regression tests for dynamic-execution built-ins."""

from cpython_compat_support import assert_cpython_compatible


def test_eval_normal_error_and_namespace_behavior() -> None:
    assert_cpython_compatible(
        r"""
def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


globals_ns = {"x": 4, "__builtins__": {"len": len}}
locals_ns = {"y": 6}
records = [
    outcome(lambda: eval("x + y", globals_ns, locals_ns)),
    outcome(lambda: eval("len([1, 2, 3])", globals_ns, locals_ns)),
    outcome(lambda: eval(compile("x * y", "<compat>", "eval"), globals_ns, locals_ns)),
    outcome(lambda: eval("x +", globals_ns, locals_ns)),
    outcome(lambda: eval(42)),
]
print(repr((records, globals_ns, locals_ns)))
""",
    )


def test_exec_normal_error_and_namespace_behavior() -> None:
    assert_cpython_compatible(
        r"""
def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


globals_ns = {
    "seed": 4,
    "__builtins__": {"RuntimeError": RuntimeError, "LookupError": LookupError},
}
locals_ns = {"offset": 6}
records = [
    outcome(lambda: exec("result = seed + offset", globals_ns, locals_ns)),
    ("state", globals_ns, locals_ns),
    outcome(lambda: exec("before = 1\nraise LookupError('boom')\nafter = 2", globals_ns, locals_ns)),
    ("after-error", globals_ns, locals_ns),
    outcome(lambda: exec(42, globals_ns, locals_ns)),
]
print(repr(records))
""",
    )


def test_breakpoint_hook_arguments_and_errors() -> None:
    assert_cpython_compatible(
        r"""
import sys


def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


original_hook = sys.breakpointhook
calls = []


def hook(*args, **kwargs):
    calls.append((args, tuple(sorted(kwargs.items()))))
    return {"called": True, "count": len(calls)}


try:
    sys.breakpointhook = hook
    records = [
        outcome(lambda: breakpoint(1, label="two")),
        outcome(lambda: breakpoint()),
    ]

    def raising_hook(*args, **kwargs):
        raise RuntimeError((args, kwargs))

    sys.breakpointhook = raising_hook
    records.append(outcome(lambda: breakpoint("value", level=3)))
finally:
    sys.breakpointhook = original_hook

print(repr((records, calls)))
""",
    )


def test_import_normal_errors_and_fromlist_behavior() -> None:
    assert_cpython_compatible(
        r"""
import sys


def outcome(call):
    try:
        value = call()
        return ("return", value)
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


math_module = __import__("math")
math_fromlist = __import__("math", fromlist=("sqrt",))
records = [
    (math_module.__name__, math_fromlist.__name__, math_module is math_fromlist),
    outcome(lambda: __import__("_aleff_issue_55_missing_module")),
    outcome(lambda: __import__(None)),
    outcome(lambda: __import__("math", {}, {}, ("sqrt",), 1)),
]
print(repr(records))
""",
    )


def test_builtin_metadata_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import builtins


print(repr((builtins.__import__.__doc__, builtins.__build_class__.__doc__)))
""",
    )


def test_build_class_normal_metaclass_mro_entries_and_errors() -> None:
    assert_cpython_compatible(
        r"""
def outcome(call):
    try:
        value = call()
        return ("return", value)
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class Base:
    def __init_subclass__(cls, **kwargs):
        cls.flag = kwargs.pop("flag", None)
        super().__init_subclass__(**kwargs)


class Alias:
    def __mro_entries__(self, bases):
        return (Base,)


class Meta(type):
    @classmethod
    def __prepare__(mcls, name, bases, **kwargs):
        namespace = dict()
        namespace["prepared"] = (name, len(bases), tuple(sorted(kwargs.items())))
        return namespace


class Prepared(Alias(), metaclass=Meta, flag=9):
    value = 12


class LeftMeta(type):
    pass


class RightMeta(type):
    pass


class Left(metaclass=LeftMeta):
    pass


class Right(metaclass=RightMeta):
    pass


def conflicting_class():
    class Impossible(Left, Right):
        pass

    return Impossible


records = [
    (Prepared.__bases__[0] is Base, Prepared.__orig_bases__[0].__class__.__name__),
    (Prepared.prepared, Prepared.value),
    outcome(conflicting_class),
]
print(repr(records))
""",
    )


def test_build_class_continuations_preserve_cpython_construction() -> None:
    assert_cpython_compatible(
        r"""
import sys


def resumed(body, values=(7, 11)):
    def shot(value):
        try:
            return ("return", body(lambda: value))
        except BaseException as exc:
            return ("raise", type(exc).__name__, str(exc))

    if "aleff" not in sys.modules:
        return [shot(value) for value in values]

    from aleff import create_handler, effect

    choose = effect("cpython_compat_07_choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle(k):
        results = []
        for value in values:
            try:
                results.append(("return", k(value)))
            except BaseException as exc:
                results.append(("raise", type(exc).__name__, str(exc)))
        return results

    return handler(lambda: body(choose))


class Base:
    pass


class Alias:
    def __mro_entries__(self, bases):
        return (Base,)


def mro_entries_body(choose):
    class Target(Alias()):
        value = choose()

    return (Target.__bases__[0] is Base, Target.__orig_bases__[0].__class__.__name__, Target.value)


class MetaClass(type):
    def __call__(mcls, name, bases, namespace, **kwargs):
        result = type.__call__(mcls, name, bases, namespace, **kwargs)
        result.selected = mcls.__name__
        return result


class FirstMeta(type, metaclass=MetaClass):
    pass


class WinningMeta(FirstMeta):
    pass


class FirstBase(metaclass=FirstMeta):
    pass


class WinningBase(metaclass=WinningMeta):
    pass


def metaclass_body(choose):
    class Target(FirstBase, WinningBase):
        value = choose()

    return (type(Target).__name__, Target.selected, Target.value)


class PrepareMeta(type):
    @classmethod
    def __prepare__(mcls, name, bases, **kwargs):
        choose_for_prepare()
        return {}


def prepare_body(choose):
    global choose_for_prepare
    choose_for_prepare = choose

    class Target(metaclass=PrepareMeta):
        pass

    return Target.__name__


class KeywordBase:
    def __init_subclass__(cls, *, flag):
        cls.flag = flag


class Descriptor:
    def __set_name__(self, owner, name):
        owner.marker = choose_for_descriptor()


def keyword_body(choose):
    global choose_for_descriptor
    choose_for_descriptor = choose

    class Target(KeywordBase, flag=42):
        value = Descriptor()

    return Target.flag


class RealSetName:
    def __set_name__(self, owner, name):
        owner.events.append("real")

    def __getattribute__(self, name):
        if name == "__set_name__":
            return lambda owner, ignored: owner.events.append("fake")
        return object.__getattribute__(self, name)


class SuspendingSetName:
    def __set_name__(self, owner, name):
        choose_for_set_name()


def special_lookup_body(choose):
    global choose_for_set_name
    choose_for_set_name = choose

    class Target:
        events = []
        first = SuspendingSetName()
        second = RealSetName()

    return Target.events


class CallableSetName:
    def __call__(self, owner, name):
        choose_for_callable()


class CallableDescriptor:
    __set_name__ = CallableSetName()


def callable_set_name_body(choose):
    global choose_for_callable
    choose_for_callable = choose

    class Target:
        value = CallableDescriptor()

    return Target.__name__


class ReplacementMeta(type):
    def __new__(mcls, name, bases, namespace, **kwargs):
        return type.__new__(mcls, "Replacement", (), {})


def classcell_body(choose):
    class Target(metaclass=ReplacementMeta):
        def method(self):
            return __class__

        marker = choose()

    return Target.__name__


records = [
    resumed(mro_entries_body),
    resumed(metaclass_body),
    resumed(prepare_body),
    resumed(keyword_body),
    resumed(special_lookup_body),
    resumed(callable_set_name_body),
    resumed(classcell_body),
]
print(repr(records))
""",
    )


_BUILD_CLASS_MULTISHOT_PREAMBLE = r"""
import sys


def resumed(body, values=(7, 11)):
    def shot(value):
        try:
            return ("return", body(lambda: value))
        except BaseException as exc:
            return ("raise", type(exc).__name__, str(exc))

    if "aleff" not in sys.modules:
        return [shot(value) for value in values]

    from aleff import create_handler, effect

    choose = effect("build-class-isolated-choice")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle(k):
        results = []
        for value in values:
            try:
                results.append(("return", k(value)))
            except BaseException as exc:
                results.append(("raise", type(exc).__name__, str(exc)))
        return results

    return handler(lambda: body(choose))
"""


def test_build_class_set_name_runs_exactly_once_per_shot() -> None:
    assert_cpython_compatible(
        _BUILD_CLASS_MULTISHOT_PREAMBLE
        + r"""
class SuspendingDescriptor:
    def __set_name__(self, owner, name):
        choose_for_set_name()


class ObservedDescriptor:
    def __set_name__(self, owner, name):
        owner.events.append(("set_name", name))


def body(choose):
    global choose_for_set_name
    choose_for_set_name = choose

    class Target:
        events = []
        suspending = SuspendingDescriptor()
        observed = ObservedDescriptor()

    return tuple(Target.events)


print(repr(resumed(body)))
"""
    )


def test_build_class_init_subclass_runs_exactly_once_per_shot() -> None:
    assert_cpython_compatible(
        _BUILD_CLASS_MULTISHOT_PREAMBLE
        + r"""
class Base:
    def __init_subclass__(cls, **kwargs):
        cls.events.append("init_subclass")
        super().__init_subclass__(**kwargs)


class SuspendingDescriptor:
    def __set_name__(self, owner, name):
        choose_for_set_name()


def body(choose):
    global choose_for_set_name
    choose_for_set_name = choose

    class Target(Base):
        events = []
        suspending = SuspendingDescriptor()

    return tuple(Target.events)


print(repr(resumed(body)))
"""
    )


def test_build_class_classcell_postcondition_is_checked_per_shot() -> None:
    assert_cpython_compatible(
        _BUILD_CLASS_MULTISHOT_PREAMBLE
        + r"""
class ReplacementMeta(type):
    def __new__(mcls, name, bases, namespace, **kwargs):
        return type.__new__(mcls, "Replacement", (), {})


def body(choose):
    class Target(metaclass=ReplacementMeta):
        def method(self):
            return __class__

        marker = choose()

    return Target.__name__


print(repr(resumed(body)))
"""
    )


def test_import_continuation_preserves_package_state() -> None:
    assert_cpython_compatible(
        r"""
import importlib.abc
import importlib.util
import sys


def resumed(body, values=(7, 11)):
    def shot(value):
        try:
            return ("return", body(lambda: value))
        except BaseException as exc:
            return ("raise", type(exc).__name__, str(exc))

    if "aleff" not in sys.modules:
        return [shot(value) for value in values]

    from aleff import create_handler, effect

    choose = effect("cpython_compat_07_import_choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle(k):
        results = []
        for value in values:
            try:
                results.append(("return", k(value)))
            except BaseException as exc:
                results.append(("raise", type(exc).__name__, str(exc)))
        return results

    return handler(lambda: body(choose))


package_name = "_aleff_compat_07_package"
child_name = package_name + ".child"


class Loader(importlib.abc.Loader):
    def __init__(self, choose):
        self.choose = choose

    def create_module(self, spec):
        return None

    def exec_module(self, module):
        if module.__name__ == child_name:
            module.value = self.choose()


class Finder(importlib.abc.MetaPathFinder):
    def __init__(self, choose):
        self.choose = choose

    def find_spec(self, fullname, path, target=None):
        if fullname == package_name:
            return importlib.util.spec_from_loader(
                fullname, Loader(self.choose), is_package=True
            )
        if fullname == child_name:
            return importlib.util.spec_from_loader(fullname, Loader(self.choose))
        return None


def import_body(choose):
    finder = Finder(choose)
    sys.meta_path.insert(0, finder)
    try:
        module = __import__(child_name, fromlist=("value",))
        package = sys.modules[package_name]
        return (module.__name__, module.value, getattr(package, "child") is module)
    finally:
        if finder in sys.meta_path:
            sys.meta_path.remove(finder)
        sys.modules.pop(child_name, None)
        sys.modules.pop(package_name, None)


print(repr(resumed(import_body)))
""",
    )


def test_three_argument_type_normal_errors_and_metaclass_behavior() -> None:
    assert_cpython_compatible(
        r"""
def outcome(call):
    try:
        return ("return", call())
    except BaseException as exc:
        return ("raise", type(exc).__name__, str(exc))


class Base:
    def __init_subclass__(cls, **kwargs):
        cls.received = tuple(sorted(kwargs.items()))
        kwargs.pop("flag", None)
        super().__init_subclass__(**kwargs)


class Descriptor:
    def __set_name__(self, owner, name):
        owner.descriptor_name = name


normal = type(
    "Normal",
    (Base,),
    {"value": 12, "descriptor": Descriptor()},
    flag=9,
)
records = [
    (normal.__name__, normal.__bases__[0].__name__, normal.value, normal.descriptor_name, normal.received),
    outcome(lambda: type("TooFew", ())),
    outcome(lambda: type("BadBases", (42,), {})),
    outcome(lambda: type("BadNamespace", (), [])),
]
print(repr(records))
""",
    )


def test_three_argument_type_continuations_preserve_cpython_construction() -> None:
    assert_cpython_compatible(
        r"""
import sys


def resumed(body, values=(7, 11)):
    def shot(value):
        try:
            return ("return", body(lambda: value))
        except BaseException as exc:
            return ("raise", type(exc).__name__, str(exc))

    if "aleff" not in sys.modules:
        return [shot(value) for value in values]

    from aleff import create_handler, effect

    choose = effect("cpython_compat_07_type_choose")
    handler = create_handler(choose)

    @handler.on(choose)
    def handle(k):
        results = []
        for value in values:
            try:
                results.append(("return", k(value)))
            except BaseException as exc:
                results.append(("raise", type(exc).__name__, str(exc)))
        return results

    return handler(lambda: body(choose))


class KeywordBase:
    def __init_subclass__(cls, *, flag):
        cls.flag = flag


class Descriptor:
    def __set_name__(self, owner, name):
        owner.marker = choose_for_descriptor()


def keyword_body(choose):
    global choose_for_descriptor
    choose_for_descriptor = choose
    target = type(
        "Target",
        (KeywordBase,),
        {"value": Descriptor()},
        flag=42,
    )
    return (target.flag, target.marker)


class CallableSetName:
    def __call__(self, owner, name):
        choose_for_callable()


class CallableDescriptor:
    __set_name__ = CallableSetName()


def callable_body(choose):
    global choose_for_callable
    choose_for_callable = choose
    target = type("Target", (), {"value": CallableDescriptor()})
    return target.__name__


print(repr((resumed(keyword_body), resumed(callable_body))))
""",
    )
