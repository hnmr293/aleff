"""
Demo: Discrete Probability Distributions (multi-shot continuations)

Compute exact probability distributions by enumerating all branches.
The handler calls k(True) and k(False) with appropriate weights, then
merges the resulting distributions.  This demonstrates a non-list
combinator for multi-shot — results are weighted dictionaries, not flat
lists.

Multi-shot pattern:
  The handler calls k(True) weighted by p, and k(False) weighted by
  (1 - p).  Each call returns a dict mapping outcomes to probabilities.
  The handler merges the two dicts with appropriate weights.
"""

from typing import Callable

from aleff import effect, Effect, Handler, Resume, create_handler, wind_range


# ---------------------------------------------------------------------------
# Effects
# ---------------------------------------------------------------------------

# flip(p) → True with probability p, False with probability 1-p
flip: Effect[[float], bool] = effect("flip")


# ---------------------------------------------------------------------------
# Handler: enumerate branches with weights
# ---------------------------------------------------------------------------

type Dist = dict[str, float]


def run_prob(computation: Callable[[], Dist]) -> Dist:
    h: Handler[Dist] = create_handler(flip)

    @h.on(flip)
    def _flip(k: Resume[bool, Dist], p: float) -> Dist:
        dist_true = k(True)
        dist_false = k(False)
        merged: Dist = {}
        for outcome, prob in dist_true.items():
            merged[outcome] = merged.get(outcome, 0.0) + p * prob
        for outcome, prob in dist_false.items():
            merged[outcome] = merged.get(outcome, 0.0) + (1 - p) * prob
        return merged

    return h(computation)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------


def coin_flip() -> Dist:
    """Single fair coin flip."""
    result = flip(0.5)
    return {"heads": 1.0} if result else {"tails": 1.0}


def two_coins() -> Dist:
    """Two independent fair coin flips — count heads."""
    c1 = flip(0.5)
    c2 = flip(0.5)
    heads = (1 if c1 else 0) + (1 if c2 else 0)
    return {f"{heads}_heads": 1.0}


def three_coins_at_least_two() -> Dist:
    """Probability of getting at least 2 heads in 3 fair flips."""
    count = 0
    with wind_range(3) as r:
        for _ in r:
            if flip(0.5):
                count += 1
    return {"yes": 1.0} if count >= 2 else {"no": 1.0}


def biased_coin() -> Dist:
    """Two flips of a biased coin (p=0.7 heads)."""
    c1 = flip(0.7)
    c2 = flip(0.7)
    heads = (1 if c1 else 0) + (1 if c2 else 0)
    return {f"{heads}_heads": 1.0}


def bayesian_diagnosis() -> Dist:
    """Simple Bayesian reasoning.

    A disease has 1% prevalence.  A test has 90% sensitivity (true positive)
    and 95% specificity (true negative).  What is P(disease | positive test)?
    """
    has_disease = flip(0.01)
    if has_disease:
        test_positive = flip(0.90)  # sensitivity
    else:
        test_positive = flip(0.05)  # 1 - specificity
    if test_positive:
        label = "disease" if has_disease else "healthy"
        return {label: 1.0}
    else:
        return {"negative_test": 1.0}


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------


def print_dist(dist: Dist) -> None:
    total = sum(dist.values())
    for outcome in sorted(dist):
        p = dist[outcome]
        bar = "#" * int(p / total * 40)
        print(f"  {outcome:>20s}: {p:.4f}  {bar}")
    print()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main() -> None:
    print("=== Single fair coin ===")
    d = run_prob(coin_flip)
    print_dist(d)
    assert abs(d["heads"] - 0.5) < 1e-9
    assert abs(d["tails"] - 0.5) < 1e-9
    print("  [OK]")

    print("=== Two fair coins ===")
    d = run_prob(two_coins)
    print_dist(d)
    assert abs(d["0_heads"] - 0.25) < 1e-9
    assert abs(d["1_heads"] - 0.50) < 1e-9
    assert abs(d["2_heads"] - 0.25) < 1e-9
    print("  [OK]")

    print("=== Three coins, at least 2 heads ===")
    d = run_prob(three_coins_at_least_two)
    print_dist(d)
    assert abs(d["yes"] - 0.5) < 1e-9
    assert abs(d["no"] - 0.5) < 1e-9
    print("  [OK]")

    print("=== Biased coin (p=0.7), two flips ===")
    d = run_prob(biased_coin)
    print_dist(d)
    assert abs(d["0_heads"] - 0.09) < 1e-9
    assert abs(d["1_heads"] - 0.42) < 1e-9
    assert abs(d["2_heads"] - 0.49) < 1e-9
    print("  [OK]")

    print("=== Bayesian diagnosis (given positive test) ===")
    d = run_prob(bayesian_diagnosis)
    # Filter to only positive-test outcomes
    p_disease = d.get("disease", 0.0)
    p_healthy = d.get("healthy", 0.0)
    p_positive = p_disease + p_healthy
    print(f"  P(positive test) = {p_positive:.4f}")
    print(f"  P(disease | positive) = {p_disease / p_positive:.4f}")
    print(f"  P(healthy | positive) = {p_healthy / p_positive:.4f}")
    # P(disease | positive) = 0.01 * 0.90 / (0.01 * 0.90 + 0.99 * 0.05)
    expected = 0.009 / (0.009 + 0.0495)
    assert abs(p_disease / p_positive - expected) < 1e-6
    print("  [OK]")

    print()
    print("All probability demos passed.")


if __name__ == "__main__":
    main()
