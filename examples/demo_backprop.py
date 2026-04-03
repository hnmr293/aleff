"""
Demo: Reverse-mode Automatic Differentiation (backpropagation)

Implement backpropagation without building an explicit tape, by embedding
backward passes in the handler's post-resume continuation.

How it works:
  1. Handler calls resume(result) -> the rest of the forward computation runs to completion
  2. resume returns -> at this point result.grad contains the gradient propagated from downstream
  3. Apply chain rule to distribute gradients to input nodes
  The handler call stack naturally unwinds in reverse order, so no tape is needed.

Comparison:
  - evaluate:   normal computation with float
  - forward AD: propagate value and derivative simultaneously with Dual numbers
  - backprop:   reverse-mode gradient accumulation with Num (shared grad)
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any

from aleff1 import (
    effect,
    Effect,
    Resume,
    create_handler,
)


# ---------------------------------------------------------------------------
# Num: a node holding value and gradient
# ---------------------------------------------------------------------------


@dataclass
class Num:
    val: float
    grad: float = 0.0

    def __repr__(self) -> str:
        return f"Num({self.val:.6f}, grad={self.grad:.6f})"


# ---------------------------------------------------------------------------
# Effects
# ---------------------------------------------------------------------------

var: Effect[[float], Any] = effect("var")
const: Effect[[float], Any] = effect("const")
add: Effect[[Any, Any], Any] = effect("add")
mul: Effect[[Any, Any], Any] = effect("mul")
neg: Effect[[Any], Any] = effect("neg")
sin_: Effect[[Any], Any] = effect("sin")
cos_: Effect[[Any], Any] = effect("cos")
exp_: Effect[[Any], Any] = effect("exp")
log_: Effect[[Any], Any] = effect("log")
pow_: Effect[[Any, Any], Any] = effect("pow")

ALL_OPS = (var, const, add, mul, neg, sin_, cos_, exp_, log_, pow_)


# ---------------------------------------------------------------------------
# Math expressions (same as the forward AD demo)
# ---------------------------------------------------------------------------


def f1(x_val: float) -> Any:
    """f(x) = x² + sin(x),  f'(x) = 2x + cos(x)"""
    x = var(x_val)
    x_sq = mul(x, x)
    s = sin_(x)
    return add(x_sq, s)


def f2(x_val: float) -> Any:
    """f(x) = exp(x) * cos(x),  f'(x) = exp(x) * (cos(x) - sin(x))"""
    x = var(x_val)
    ex = exp_(x)
    cx = cos_(x)
    return mul(ex, cx)


def f3(x_val: float) -> Any:
    """f(x) = log(x² + 1),  f'(x) = 2x / (x² + 1)"""
    x = var(x_val)
    one = const(1.0)
    x_sq = mul(x, x)
    inner = add(x_sq, one)
    return log_(inner)


def f4(x_val: float) -> Any:
    """f(x) = x³ - 2x + 1,  f'(x) = 3x² - 2"""
    x = var(x_val)
    three = const(3.0)
    two = const(2.0)
    one = const(1.0)
    x_cubed = pow_(x, three)
    two_x = mul(two, x)
    return add(add(x_cubed, neg(two_x)), one)


def f5(x_val: float) -> Any:
    """f(x) = sin(x) * sin(x) + cos(x) * cos(x),  f'(x) = 0 (identity)"""
    x = var(x_val)
    s = sin_(x)
    c = cos_(x)
    s2 = mul(s, s)
    c2 = mul(c, c)
    return add(s2, c2)


def f6(x_val: float) -> Any:
    """f(x) = x * exp(-x²),  f'(x) = exp(-x²) * (1 - 2x²)

    Variable used in multiple places — demonstrates gradient accumulation unique to reverse-mode.
    """
    x = var(x_val)
    x_sq = mul(x, x)
    neg_x_sq = neg(x_sq)
    e = exp_(neg_x_sq)
    return mul(x, e)


# ---------------------------------------------------------------------------
# Handler: Normal evaluation
# ---------------------------------------------------------------------------


def evaluate(f, x: float) -> float:
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
# Handler: Reverse-mode AD (backpropagation)
# ---------------------------------------------------------------------------


def backprop(f, x: float) -> tuple[float, float]:
    """Compute f(x) and f'(x) using reverse-mode AD.

    Each handler:
      1. forward: create a result node and resume with k(result)
      2. after k() returns, result.grad contains the gradient from downstream
      3. backward: accumulate gradients to input nodes via chain rule
    """

    input_node: list[Num] = []  # holds the input node created by var

    h = create_handler(*ALL_OPS)

    @h.on(var)
    def _var(k: Resume[Any, Any], v: float):
        node = Num(v)
        input_node.append(node)
        return k(node)

    @h.on(const)
    def _const(k: Resume[Any, Any], v: float):
        return k(Num(v))

    @h.on(add)
    def _add(k: Resume[Any, Any], a: Num, b: Num):
        result = Num(a.val + b.val)
        v = k(result)
        # d(a+b)/da = 1, d(a+b)/db = 1
        a.grad += result.grad
        b.grad += result.grad
        return v

    @h.on(mul)
    def _mul(k: Resume[Any, Any], a: Num, b: Num):
        result = Num(a.val * b.val)
        v = k(result)
        # d(a*b)/da = b, d(a*b)/db = a
        a.grad += result.grad * b.val
        b.grad += result.grad * a.val
        return v

    @h.on(neg)
    def _neg(k: Resume[Any, Any], a: Num):
        result = Num(-a.val)
        v = k(result)
        a.grad += -result.grad
        return v

    @h.on(sin_)
    def _sin(k: Resume[Any, Any], a: Num):
        result = Num(math.sin(a.val))
        v = k(result)
        # d(sin a)/da = cos(a)
        a.grad += result.grad * math.cos(a.val)
        return v

    @h.on(cos_)
    def _cos(k: Resume[Any, Any], a: Num):
        result = Num(math.cos(a.val))
        v = k(result)
        # d(cos a)/da = -sin(a)
        a.grad += result.grad * (-math.sin(a.val))
        return v

    @h.on(exp_)
    def _exp(k: Resume[Any, Any], a: Num):
        ea = math.exp(a.val)
        result = Num(ea)
        v = k(result)
        # d(exp a)/da = exp(a)
        a.grad += result.grad * ea
        return v

    @h.on(log_)
    def _log(k: Resume[Any, Any], a: Num):
        result = Num(math.log(a.val))
        v = k(result)
        # d(log a)/da = 1/a
        a.grad += result.grad / a.val
        return v

    @h.on(pow_)
    def _pow(k: Resume[Any, Any], base: Num, exp: Num):
        val = base.val**exp.val
        result = Num(val)
        v = k(result)
        # d(a^b)/da = b * a^(b-1)
        if base.val != 0.0:
            base.grad += result.grad * exp.val * (base.val ** (exp.val - 1))
        # d(a^b)/db = a^b * ln(a)
        if base.val > 0.0:
            exp.grad += result.grad * val * math.log(base.val)
        return v

    def run():
        result: Num = f(x)
        # seed: df/df = 1
        result.grad = 1.0
        return result

    output: Num = h(run)

    return (output.val, input_node[0].grad)


# ---------------------------------------------------------------------------
# Numerical differentiation (for verification)
# ---------------------------------------------------------------------------


def numerical_diff(f, x: float, eps: float = 1e-7) -> float:
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
        ("f(x) = sin²(x) + cos²(x)  [= 1]", f5, [0.0, 1.0, math.pi, -2.5]),
        ("f(x) = x * exp(-x²)", f6, [0.0, 0.5, 1.0, -1.0, 2.0]),
    ]

    all_ok = True

    for name, f, xs in test_cases:
        print(f"=== {name} ===")

        for x in xs:
            val = evaluate(f, x)
            bp_val, bp_grad = backprop(f, x)
            num_grad = numerical_diff(f, x)

            ok = abs(bp_grad - num_grad) < 1e-5
            status = "OK" if ok else "MISMATCH"
            if not ok:
                all_ok = False

            print(f"  x={x:8.4f}  |  f(x)={val:12.6f}  |  f'(x) BP={bp_grad:12.6f}  num={num_grad:12.6f}  [{status}]")

            assert abs(val - bp_val) < 1e-10, f"value mismatch at x={x}"

        print()

    if all_ok:
        print("All backprop demos passed.")
    else:
        print("Some checks failed!")
        raise AssertionError("backprop mismatch")


if __name__ == "__main__":
    main()
