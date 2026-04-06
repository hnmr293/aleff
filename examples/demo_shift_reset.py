"""
Demo: shift/reset and shift0/reset0 via Algebraic Effects

Delimited continuations (shift/reset) have a direct correspondence
with algebraic effect handlers:

- shift/reset  = deep handler    (handler stays installed across k)
- shift0/reset0 = shallow handler (handler removed before k resumes)

This demo implements both pairs and shows how their behavior differs.
"""

from typing import Any, Callable, cast

from aleff import (
    effect,
    Effect,
    Resume,
    Handler,
    create_handler,
)


# ---------------------------------------------------------------------------
# The "shift" effect
#
# The return type of shift/shift0 depends on what the handler passes
# to the continuation at runtime, so static typing uses Any here.
# ---------------------------------------------------------------------------

_shift_eff: Effect[[Callable[[Callable[..., Any]], Any]], Any] = effect("shift")


# ---------------------------------------------------------------------------
# shift/reset (deep handler)
# ---------------------------------------------------------------------------


def reset(f: Callable[[], Any]) -> Any:
    """Delimit a computation.  shift() calls inside *f* are captured."""
    h: Handler[Any] = create_handler(_shift_eff)

    @h.on(_shift_eff)
    def _handle(
        k: Resume[Any, Any],
        g: Callable[[Callable[..., Any]], Any],
    ) -> Any:
        # k includes the handler (deep), so calling k(v) runs under reset
        return g(k)

    return h(f, check=False)


def shift(g: Callable[[Callable[..., Any]], Any]) -> Any:
    """Capture the current continuation up to the nearest reset."""
    return _shift_eff(g)


# ---------------------------------------------------------------------------
# shift0/reset0 (shallow handler)
# ---------------------------------------------------------------------------


def reset0(f: Callable[[], Any]) -> Any:
    """Delimit a computation (shallow).  shift0() calls inside *f*
    are captured, but k does NOT include the delimiter."""
    h: Handler[Any] = create_handler(_shift_eff, shallow=True)

    @h.on(_shift_eff)
    def _handle(
        k: Resume[Any, Any],
        g: Callable[[Callable[..., Any]], Any],
    ) -> Any:
        # k does NOT include the handler (shallow)
        # calling k(v) resumes without any delimiter installed
        return g(k)

    return h(f, check=False)


def shift0(g: Callable[[Callable[..., Any]], Any]) -> Any:
    """Capture the continuation up to the nearest reset0 (shallow)."""
    return _shift_eff(g)


# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------


def example_deep_basic() -> None:
    """shift/reset: basic usage."""
    # reset(fun () -> 1 + shift(fun k -> k(k(10))))
    # k = fun x -> 1 + x  (with reset re-installed)
    # k(k(10)) = k(1 + 10) = k(11) = 1 + 11 = 12
    result: int = reset(lambda: 1 + shift(lambda k: k(k(10))))
    print(f"  reset(1 + shift(k -> k(k(10))))  = {result}")
    assert result == 12


def example_shallow_basic() -> None:
    """shift0/reset0: basic usage."""
    # reset0(fun () -> 1 + shift0(fun k -> k(10)))
    # k = fun x -> 1 + x  (WITHOUT reset0 re-installed)
    # k(10) = 1 + 10 = 11
    result: int = reset0(lambda: 1 + shift0(lambda k: k(10)))
    print(f"  reset0(1 + shift0(k -> k(10)))   = {result}")
    assert result == 11


def example_deep_multiple_shifts() -> None:
    """shift/reset: multiple shifts.  The handler stays installed,
    so the second shift is caught by the same reset."""

    def body() -> int:
        a: int = shift(lambda k: k(1) + k(2))  # first shift: fork
        b: int = shift(lambda k: k(a * 10))  # second shift: transform
        return b

    result: int = reset(body)
    # First shift forks: k(1) and k(2)
    # k(1): a=1, second shift: k2(1*10) = k2(10) -> b=10 -> return 10
    # k(2): a=2, second shift: k2(2*10) = k2(20) -> b=20 -> return 20
    # Total: 10 + 20 = 30
    print(f"  reset(shift(k->k(1)+k(2)); shift(k->k(a*10)))  = {result}")
    assert result == 30


def example_shallow_needs_re_delimit() -> None:
    """shift0/reset0: after the first shift0, the handler is removed.
    A second shift0 would be unhandled unless we re-install reset0."""

    def body() -> int:
        a: int = shift0(lambda k: reset0(lambda: k(1)) + reset0(lambda: k(2)))
        b: int = shift0(lambda k: reset0(lambda: k(a * 10)))
        return b

    result: int = reset0(body)
    # First shift0 captures k (does NOT include reset0).
    # We must wrap k(1) and k(2) in reset0() to catch the second shift0.
    # reset0(k(1)): a=1, second shift0 captured, k2(1*10) = 10
    # reset0(k(2)): a=2, second shift0 captured, k2(2*10) = 20
    # Total: 10 + 20 = 30
    print(f"  reset0(shift0(k->reset0(k(1))+reset0(k(2))); shift0(...))  = {result}")
    assert result == 30


def example_abort() -> None:
    """shift/shift0 can abort: if g does not call k, the computation
    is discarded.  Both deep and shallow behave identically here."""
    result_deep: int = reset(lambda: 1 + shift(lambda _k: 42))
    result_shallow: int = reset0(lambda: 1 + shift0(lambda _k: 42))
    print(f"  reset(1 + shift(_ -> 42))   = {result_deep}")
    print(f"  reset0(1 + shift0(_ -> 42)) = {result_shallow}")
    assert result_deep == 42
    assert result_shallow == 42


def example_generator() -> None:
    """Generators via shift0/reset0.

    yield suspends the computation; the consumer decides whether
    to continue by calling k inside a fresh reset0.
    """

    def yield_val(x: int) -> None:
        shift0(lambda k: (x, k))

    def produce() -> None:
        yield_val(1)
        yield_val(2)
        yield_val(3)

    # Collect all yielded values
    values: list[int] = []
    result: Any = reset0(produce)
    while isinstance(result, tuple):
        val = cast(int, result[0])
        k = cast(Callable[[None], Any], result[1])
        values.append(val)
        result = reset0(lambda: k(None))

    print(f"  generator yields = {values}")
    assert values == [1, 2, 3]


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


if __name__ == "__main__":
    print("=== shift/reset (deep handler) ===")
    example_deep_basic()
    example_deep_multiple_shifts()

    print()
    print("=== shift0/reset0 (shallow handler) ===")
    example_shallow_basic()
    example_shallow_needs_re_delimit()

    print()
    print("=== Abort (both) ===")
    example_abort()

    print()
    print("=== Generator via shift0/reset0 ===")
    example_generator()

    print()
    print("All demos passed.")
