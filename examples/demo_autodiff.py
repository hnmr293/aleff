"""
Demo: Automatic Differentiation (forward-mode)

Swap handlers on the same math expression to switch between:
  - Normal evaluation (float)
  - Automatic differentiation (Dual number)

The math expressions don't know what "numbers" are — they just invoke
primitive operations via effects. If the handler passes floats, it's normal
evaluation. If it passes Dual numbers, derivatives propagate automatically.
"""

import math
from dataclasses import dataclass
from typing import Any, Callable

from aleff import (
    effect,
    Effect,
    Resume,
    create_handler,
)


# ---------------------------------------------------------------------------
# Dual number (core data structure for forward-mode AD)
# ---------------------------------------------------------------------------


@dataclass
class Dual:
    """val + dot * ε (ε² = 0)"""

    val: float
    dot: float  # derivative

    def __repr__(self) -> str:
        return f"Dual({self.val:.6f}, d={self.dot:.6f})"


# ---------------------------------------------------------------------------
# Effects: arithmetic primitives
# ---------------------------------------------------------------------------

var: Effect[[float], Any] = effect("var")  # wrap input variable
const: Effect[[float], Any] = effect("const")  # wrap constant
add: Effect[[Any, Any], Any] = effect("add")
mul: Effect[[Any, Any], Any] = effect("mul")
neg: Effect[[Any], Any] = effect("neg")
sin_: Effect[[Any], Any] = effect("sin")
cos_: Effect[[Any], Any] = effect("cos")
exp_: Effect[[Any], Any] = effect("exp")
log_: Effect[[Any], Any] = effect("log")
pow_: Effect[[Any, Any], Any] = effect("pow")  # base, exponent

ALL_OPS = (var, const, add, mul, neg, sin_, cos_, exp_, log_, pow_)


# ---------------------------------------------------------------------------
# Math expressions (pure — type-agnostic)
# ---------------------------------------------------------------------------


def f1(x_val: float) -> Any:
    """f(x) = x² + sin(x)
    f'(x) = 2x + cos(x)
    """
    x = var(x_val)
    x_sq = mul(x, x)
    s = sin_(x)
    return add(x_sq, s)


def f2(x_val: float) -> Any:
    """f(x) = exp(x) * cos(x)
    f'(x) = exp(x) * cos(x) - exp(x) * sin(x) = exp(x) * (cos(x) - sin(x))
    """
    x = var(x_val)
    ex = exp_(x)
    cx = cos_(x)
    return mul(ex, cx)


def f3(x_val: float) -> Any:
    """f(x) = log(x² + 1)
    f'(x) = 2x / (x² + 1)
    """
    x = var(x_val)
    one = const(1.0)
    x_sq = mul(x, x)
    inner = add(x_sq, one)
    return log_(inner)


def f4(x_val: float) -> Any:
    """f(x) = x^3 - 2x + 1
    f'(x) = 3x² - 2
    """
    x = var(x_val)
    three = const(3.0)
    two = const(2.0)
    one = const(1.0)
    x_cubed = pow_(x, three)
    two_x = mul(two, x)
    return add(add(x_cubed, neg(two_x)), one)


# ---------------------------------------------------------------------------
# Handler 1: Normal evaluation (float)
# ---------------------------------------------------------------------------


def evaluate(f: Callable[[float], Any], x: float) -> float:
    h = create_handler(*ALL_OPS)

    @h.on(var)
    def _var(k: Resume[Any, Any], v: float):
        return k(v)

    @h.on(const)
    def _const(k: Resume[Any, Any], v: float):
        return k(v)

    @h.on(add)
    def _add(k: Resume[Any, Any], a: float, b: float):
        return k(a + b)

    @h.on(mul)
    def _mul(k: Resume[Any, Any], a: float, b: float):
        return k(a * b)

    @h.on(neg)
    def _neg(k: Resume[Any, Any], a: float):
        return k(-a)

    @h.on(sin_)
    def _sin(k: Resume[Any, Any], a: float):
        return k(math.sin(a))

    @h.on(cos_)
    def _cos(k: Resume[Any, Any], a: float):
        return k(math.cos(a))

    @h.on(exp_)
    def _exp(k: Resume[Any, Any], a: float):
        return k(math.exp(a))

    @h.on(log_)
    def _log(k: Resume[Any, Any], a: float):
        return k(math.log(a))

    @h.on(pow_)
    def _pow(k: Resume[Any, Any], base: float, exp: float):
        return k(base**exp)

    return h(lambda: f(x))


# ---------------------------------------------------------------------------
# Handler 2: Automatic differentiation (Dual number, forward-mode)
# ---------------------------------------------------------------------------


def differentiate(f: Callable[[float], Any], x: float) -> tuple[float, float]:
    """Compute f(x) and f'(x) simultaneously."""

    h = create_handler(*ALL_OPS)

    @h.on(var)
    def _var(k: Resume[Any, Any], v: float):
        return k(Dual(v, 1.0))  # dx/dx = 1

    @h.on(const)
    def _const(k: Resume[Any, Any], v: float):
        return k(Dual(v, 0.0))  # dc/dx = 0

    @h.on(add)
    def _add(k: Resume[Any, Any], a: Dual, b: Dual):
        # d(a+b) = da + db
        return k(Dual(a.val + b.val, a.dot + b.dot))

    @h.on(mul)
    def _mul(k: Resume[Any, Any], a: Dual, b: Dual):
        # d(a*b) = da*b + a*db  (product rule)
        return k(Dual(a.val * b.val, a.dot * b.val + a.val * b.dot))

    @h.on(neg)
    def _neg(k: Resume[Any, Any], a: Dual):
        return k(Dual(-a.val, -a.dot))

    @h.on(sin_)
    def _sin(k: Resume[Any, Any], a: Dual):
        # d(sin a) = cos(a) * da  (chain rule)
        return k(Dual(math.sin(a.val), math.cos(a.val) * a.dot))

    @h.on(cos_)
    def _cos(k: Resume[Any, Any], a: Dual):
        # d(cos a) = -sin(a) * da
        return k(Dual(math.cos(a.val), -math.sin(a.val) * a.dot))

    @h.on(exp_)
    def _exp(k: Resume[Any, Any], a: Dual):
        # d(exp a) = exp(a) * da
        ea = math.exp(a.val)
        return k(Dual(ea, ea * a.dot))

    @h.on(log_)
    def _log(k: Resume[Any, Any], a: Dual):
        # d(log a) = da / a
        return k(Dual(math.log(a.val), a.dot / a.val))

    @h.on(pow_)
    def _pow(k: Resume[Any, Any], base: Dual, exp: Dual):
        val = base.val**exp.val
        # d(a^b):
        #   exponent constant (b'=0): b * a^(b-1) * a'
        #   base constant (a'=0):     a^b * ln(a) * b'
        #   general:                   a^b * (b' * ln(a) + b * a'/a)
        if exp.dot == 0.0:
            dot = exp.val * (base.val ** (exp.val - 1)) * base.dot
        elif base.dot == 0.0:
            dot = val * math.log(base.val) * exp.dot
        else:
            dot = val * (exp.dot * math.log(base.val) + exp.val * base.dot / base.val)
        return k(Dual(val, dot))

    result: Dual = h(lambda: f(x))
    return (result.val, result.dot)


# ---------------------------------------------------------------------------
# Numerical differentiation (for verification)
# ---------------------------------------------------------------------------


def numerical_diff(f: Callable[[float], Any], x: float, eps: float = 1e-7) -> float:
    return (evaluate(f, x + eps) - evaluate(f, x - eps)) / (2 * eps)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main():
    test_cases: list[tuple[str, Any, list[float]]] = [
        ("f(x) = x² + sin(x)", f1, [0.0, 1.0, 2.0, -1.5, math.pi]),
        ("f(x) = exp(x) * cos(x)", f2, [0.0, 1.0, -0.5, math.pi / 4]),
        ("f(x) = log(x² + 1)", f3, [0.0, 1.0, 2.0, -3.0]),
        ("f(x) = x³ - 2x + 1", f4, [0.0, 1.0, -1.0, 2.0, 0.5]),
    ]

    all_ok = True

    for name, f, xs in test_cases:
        print(f"=== {name} ===")

        for x in xs:
            val = evaluate(f, x)
            ad_val, ad_deriv = differentiate(f, x)
            num_deriv = numerical_diff(f, x)

            ok = abs(ad_deriv - num_deriv) < 1e-5
            status = "OK" if ok else "MISMATCH"
            if not ok:
                all_ok = False

            print(f"  x={x:8.4f}  |  f(x)={val:12.6f}  |  f'(x) AD={ad_deriv:12.6f}  num={num_deriv:12.6f}  [{status}]")

            assert abs(val - ad_val) < 1e-10, f"value mismatch at x={x}"

        print()

    if all_ok:
        print("All autodiff demos passed.")
    else:
        print("Some checks failed!")
        raise AssertionError("autodiff mismatch")


if __name__ == "__main__":
    main()
