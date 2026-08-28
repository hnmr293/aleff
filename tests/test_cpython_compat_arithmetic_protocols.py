from cpython_compat_support import assert_cpython_compatible


def test_arithmetic_protocols_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
operations = (
    ("add", "+"), ("sub", "-"), ("mul", "*"), ("matmul", "@"),
    ("truediv", "/"), ("floordiv", "//"), ("mod", "%"), ("pow", "**"),
    ("lshift", "<<"), ("rshift", ">>"), ("and", "&"), ("xor", "^"),
    ("or", "|"),
)


def make_method(name):
    def method(self, other):
        return "%s:%s" % (name, type(other).__name__)
    return method


Target = type(
    "Target",
    (),
    {
        dunder: make_method(dunder)
        for name, _symbol in operations
        for dunder in ("__%s__" % name, "__r%s__" % name, "__i%s__" % name)
    },
)

rows = []
for name, symbol in operations:
    for kind, dunder, expression in (
        ("binary", "__%s__" % name, "target %s 7" % symbol),
        ("reflected", "__r%s__" % name, "7 %s target" % symbol),
    ):
        result = eval(expression, {"target": Target()})
        rows.append((kind, dunder, result, type(result).__name__))

    namespace = {"target": Target()}
    exec("target %s= 7" % symbol, namespace)
    result = namespace["target"]
    rows.append(("inplace", "__i%s__" % name, result, type(result).__name__))

print(rows)
"""
    )


def test_arithmetic_dispatch_corner_cases_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
operations = (
    ("add", "+"), ("sub", "-"), ("mul", "*"), ("matmul", "@"),
    ("truediv", "/"), ("floordiv", "//"), ("mod", "%"), ("pow", "**"),
    ("lshift", "<<"), ("rshift", ">>"), ("and", "&"), ("xor", "^"),
    ("or", "|"),
)


def returning(value):
    def method(self, other):
        return value
    return method


# A reflected method on a different type must run after the left method
# returns NotImplemented, for every binary operator.
fallback_rows = []
for name, symbol in operations:
    Left = type("Left", (), {"__%s__" % name: returning(NotImplemented)})
    Right = type(
        "Right",
        (),
        {"__r%s__" % name: returning("reflected:%s" % name)},
    )
    fallback_rows.append((name, eval("Left() %s Right()" % symbol)))


# If both sides decline, the language raises the normal unsupported-operand
# TypeError, including its operator-specific message.
error_rows = []
for name, symbol in operations:
    Left = type("Left", (), {"__%s__" % name: returning(NotImplemented)})
    Right = type(
        "Right",
        (),
        {"__r%s__" % name: returning(NotImplemented)},
    )
    try:
        eval("Left() %s Right()" % symbol)
    except Exception as exc:
        error_rows.append((name, type(exc).__name__, str(exc)))


# A more-derived right operand gets reflected precedence over an inherited
# left operation, even when the left operation could otherwise succeed.
precedence_rows = []
for name, symbol in operations:
    Base = type("Base", (), {"__%s__" % name: returning("base")})
    Derived = type(
        "Derived",
        (Base,),
        {"__r%s__" % name: returning("derived-reflected")},
    )
    precedence_rows.append((name, eval("Base() %s Derived()" % symbol)))


# In-place NotImplemented falls back to the ordinary binary slot and then
# rebinds the target to that result.
inplace_fallback_rows = []
for name, symbol in operations:
    Target = type("Target", (), {"__%s__" % name: returning("binary:%s" % name)})
    namespace = {"target": Target()}
    exec("target %s= 7" % symbol, namespace)
    inplace_fallback_rows.append((name, namespace["target"]))


# An in-place implementation may mutate and return itself; identity must be
# preserved rather than replaced by a binary fallback.
mutation_rows = []
for name, symbol in operations:
    def inplace(self, other, name=name):
        self.calls.append(name)
        return self

    Target = type("Target", (), {"__i%s__" % name: inplace})
    target = Target()
    target.calls = []
    namespace = {"target": target}
    exec("target %s= 7" % symbol, namespace)
    mutation_rows.append((name, namespace["target"] is target, target.calls))


class Marker:
    def __repr__(self):
        return "<marker>"


class ReturnValues:
    def __add__(self, other):
        return None

    def __rsub__(self, other):
        return Marker()

    def __imul__(self, other):
        return ["replacement"]


value = ReturnValues()
namespace = {"value": value}
reflected = 7 - value
exec("value *= 7", namespace)
return_rows = (value + 7, reflected, namespace["value"])

print("fallback", fallback_rows)
print("errors", error_rows)
print("precedence", precedence_rows)
print("inplace-fallback", inplace_fallback_rows)
print("mutation", mutation_rows)
print("returns", return_rows)
"""
    )


def test_arithmetic_errors_and_operand_corner_cases_match_cpython() -> None:
    assert_cpython_compatible(
        r"""
operations = (
    ("add", "+"), ("sub", "-"), ("mul", "*"), ("matmul", "@"),
    ("truediv", "/"), ("floordiv", "//"), ("mod", "%"), ("pow", "**"),
    ("lshift", "<<"), ("rshift", ">>"), ("and", "&"), ("xor", "^"),
    ("or", "|"),
)


def raising(name):
    def method(self, other):
        raise ValueError("raised:%s" % name)
    return method


def wrong_arity(self):
    return "unreachable"


wrong_arity_rows = []
for name, symbol in operations:
    for kind, dunder, statement in (
        ("binary", "__%s__" % name, "result = target %s 7" % symbol),
        ("reflected", "__r%s__" % name, "result = 7 %s target" % symbol),
        ("inplace", "__i%s__" % name, "target %s= 7" % symbol),
    ):
        Target = type("Target", (), {dunder: wrong_arity})
        namespace = {"target": Target()}
        try:
            exec(statement, namespace)
        except Exception as exc:
            wrong_arity_rows.append((kind, name, type(exc).__name__, str(exc)))


raised_rows = []
for name, symbol in operations:
    for kind, dunder, statement in (
        ("binary", "__%s__" % name, "result = target %s 7" % symbol),
        ("reflected", "__r%s__" % name, "result = 7 %s target" % symbol),
        ("inplace", "__i%s__" % name, "target %s= 7" % symbol),
    ):
        Target = type("Target", (), {dunder: raising("%s:%s" % (kind, name))})
        namespace = {"target": Target()}
        try:
            exec(statement, namespace)
        except Exception as exc:
            raised_rows.append((kind, name, type(exc).__name__, str(exc)))


# Operand evaluation order is left-to-right.
events = []


class Operand:
    def __add__(self, other):
        events.append("dunder")
        return "result"


def make_operand(label):
    events.append(label)
    return Operand()


result = make_operand("left") + make_operand("right")
evaluation_rows = (events, result)

print("wrong-arity", wrong_arity_rows)
print("raised", raised_rows)
print("evaluation", evaluation_rows)
"""
    )


def test_operator_arithmetic_surface_matches_cpython() -> None:
    assert_cpython_compatible(
        r"""
import inspect
import operator


operations = (
    ("add", "+"), ("sub", "-"), ("mul", "*"), ("matmul", "@"),
    ("truediv", "/"), ("floordiv", "//"), ("mod", "%"), ("pow", "**"),
    ("lshift", "<<"), ("rshift", ">>"), ("and_", "&"), ("xor", "^"),
    ("or_", "|"),
)


def make_method(name):
    def method(self, other):
        return "%s:%s" % (name, type(other).__name__)
    return method


Target = type(
    "Target",
    (),
    {
        "__%s__" % name.rstrip("_"): make_method("__%s__" % name.rstrip("_"))
        for name, _symbol in operations
    },
)

binary_rows = []
for name, _symbol in operations:
    target = Target()
    binary_rows.append((name, getattr(operator, name)(target, 7)))


inplace_rows = []
for name, _symbol in operations:
    dunder = "__i%s__" % name.rstrip("_")
    Target = type("Target", (), {dunder: make_method(dunder)})
    inplace_rows.append((name, getattr(operator, "i%s" % name.rstrip("_"))(Target(), 7)))


aliases = (
    ("__add__", "add"), ("__sub__", "sub"), ("__mul__", "mul"),
    ("__matmul__", "matmul"), ("__truediv__", "truediv"),
    ("__floordiv__", "floordiv"), ("__mod__", "mod"), ("__pow__", "pow"),
    ("__lshift__", "lshift"), ("__rshift__", "rshift"),
    ("__and__", "and_"), ("__xor__", "xor"), ("__or__", "or_"),
    ("__iadd__", "iadd"), ("__isub__", "isub"), ("__imul__", "imul"),
    ("__imatmul__", "imatmul"), ("__itruediv__", "itruediv"),
    ("__ifloordiv__", "ifloordiv"), ("__imod__", "imod"),
    ("__ipow__", "ipow"), ("__ilshift__", "ilshift"),
    ("__irshift__", "irshift"), ("__iand__", "iand"),
    ("__ixor__", "ixor"), ("__ior__", "ior"),
)
# Adapter installation replaces operator callables, so public alias identity
# and replacement-function metadata are part of the compatibility checks.
alias_rows = [
    (alias, canonical, getattr(operator, alias) is getattr(operator, canonical))
    for alias, canonical in aliases
]


try:
    signature = str(inspect.signature(operator.add))
except Exception as exc:
    # The full inspect error contains the address of the replacement object.
    signature = "%s:%s" % (type(exc).__name__, "no signature")

print("binary", binary_rows)
print("inplace", inplace_rows)
print("aliases", alias_rows)
print("metadata", repr(operator.add.__name__), repr(operator.add.__doc__), repr(signature))
"""
    )
