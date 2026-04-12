"""
Demo: Amb operator / Logic Puzzle Solver (multi-shot continuations)

Implement the classic Scheme ``amb`` (ambiguous) operator using multi-shot
algebraic effects.  ``amb(choices)`` non-deterministically picks one value;
``require(condition)`` prunes branches that violate a constraint (abort
returns empty list without calling k).

The logic puzzle (from SICP Exercise 4.42):
  Baker, Cooper, Fletcher, Miller, and Smith live on different floors
  of a five-story building.  Given a set of constraints, find which
  floor each person lives on.

Multi-shot pattern:
  - ``amb``: enumerates all choices via ``for v in values: results.extend(k(v))``
  - ``require``: prunes via abort (``return []`` without calling k)
"""

from typing import Any, Callable

from aleff import effect, Effect, Handler, Resume, create_handler, wind_range


# ---------------------------------------------------------------------------
# Effects
# ---------------------------------------------------------------------------

# amb(values) → non-deterministically pick one value from the list
amb: Effect[[list[int]], int] = effect("amb")

# require(condition) → prune this branch if condition is False
require: Effect[[bool], None] = effect("require")


# ---------------------------------------------------------------------------
# Handler: enumerate + prune
# ---------------------------------------------------------------------------


def run_amb(computation: Callable[[], list[Any]]) -> list[Any]:
    h: Handler[list[Any]] = create_handler(amb, require)

    @h.on(amb)
    def _amb(k: Resume[int, list[Any]], values: list[int]) -> list[Any]:
        results: list[Any] = []
        with wind_range(len(values)) as r:
            for i in r:
                results.extend(k(values[i]))
        return results

    @h.on(require)
    def _require(k: Resume[None, list[Any]], condition: bool) -> list[Any]:
        if condition:
            return k(None)
        return []  # prune: abort without calling k

    return h(computation, check=False)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Example 1: Simple constraint satisfaction
# ---------------------------------------------------------------------------


def pythagorean_triples() -> list[tuple[int, int, int]]:
    """Find all Pythagorean triples with components in 1..10."""
    candidates = list(range(1, 11))

    def body():
        a = amb(candidates)
        b = amb(candidates)
        require(a <= b)  # avoid duplicates
        c = amb(candidates)
        require(a * a + b * b == c * c)
        return [(a, b, c)]

    return run_amb(body)


# ---------------------------------------------------------------------------
# Example 2: SICP dwelling puzzle
# ---------------------------------------------------------------------------


def dwelling_puzzle() -> list[dict[str, int]]:
    """SICP Exercise 4.42 — five people on five floors.

    Constraints:
      - Baker does not live on the top floor.
      - Cooper does not live on the bottom floor.
      - Fletcher does not live on the top or the bottom floor.
      - Miller lives on a higher floor than does Cooper.
      - Smith does not live on a floor adjacent to Fletcher's.
      - Fletcher does not live on a floor adjacent to Cooper's.
      - All five people live on different floors.
    """
    floors = [1, 2, 3, 4, 5]

    def body():
        baker = amb(floors)
        cooper = amb(floors)
        fletcher = amb(floors)
        miller = amb(floors)
        smith = amb(floors)

        # All different
        people = (baker, cooper, fletcher, miller, smith)
        require(len(set(people)) == 5)

        # Constraints
        require(baker != 5)
        require(cooper != 1)
        require(fletcher != 5)
        require(fletcher != 1)
        require(miller > cooper)
        require(abs(smith - fletcher) != 1)
        require(abs(fletcher - cooper) != 1)

        return [{"Baker": baker, "Cooper": cooper, "Fletcher": fletcher, "Miller": miller, "Smith": smith}]

    return run_amb(body)


# ---------------------------------------------------------------------------
# Example 3: Map coloring
# ---------------------------------------------------------------------------


def map_coloring() -> list[dict[str, str]]:
    """Color a simple graph (Australia) with 3 colors.

    States: WA, NT, SA, Q, NSW, V, T
    Adjacencies: WA-NT, WA-SA, NT-SA, NT-Q, SA-Q, SA-NSW, SA-V, Q-NSW, NSW-V
    """
    colors = [1, 2, 3]  # use ints for amb, map to names at the end
    color_names = {1: "red", 2: "green", 3: "blue"}

    def body():
        wa = amb(colors)
        nt = amb(colors)
        sa = amb(colors)
        q = amb(colors)
        nsw = amb(colors)
        v = amb(colors)
        t = amb(colors)

        # Adjacency constraints
        require(wa != nt)
        require(wa != sa)
        require(nt != sa)
        require(nt != q)
        require(sa != q)
        require(sa != nsw)
        require(sa != v)
        require(q != nsw)
        require(nsw != v)

        return [
            {
                "WA": color_names[wa],
                "NT": color_names[nt],
                "SA": color_names[sa],
                "Q": color_names[q],
                "NSW": color_names[nsw],
                "V": color_names[v],
                "T": color_names[t],
            }
        ]

    return run_amb(body)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main() -> None:
    print("=== Pythagorean triples (1..10) ===")
    triples = pythagorean_triples()
    for t in triples:
        print(f"  {t[0]}^2 + {t[1]}^2 = {t[2]}^2  ({t[0] ** 2} + {t[1] ** 2} = {t[2] ** 2})")
    expected = [(3, 4, 5), (6, 8, 10)]
    assert triples == expected, f"expected {expected}, got {triples}"
    print("  [OK]\n")

    print("=== SICP dwelling puzzle ===")
    solutions = dwelling_puzzle()
    for sol in solutions:
        for name, floor in sorted(sol.items()):
            print(f"  {name:>10s}: floor {floor}")
        print()
    assert len(solutions) == 1
    sol = solutions[0]
    assert sol == {"Baker": 3, "Cooper": 2, "Fletcher": 4, "Miller": 5, "Smith": 1}
    print("  [OK]\n")

    print("=== Map coloring (Australia, 3 colors) ===")
    colorings = map_coloring()
    print(f"  {len(colorings)} valid colorings found")
    if colorings:
        print(f"  Example: {colorings[0]}")
    # With 3 colors and Tasmania unconstrained (3 choices),
    # the chromatic polynomial for the mainland gives 6 colorings,
    # times 3 for Tasmania = 18
    assert len(colorings) == 18, f"expected 18, got {len(colorings)}"
    print("  [OK]\n")

    print("All amb demos passed.")


if __name__ == "__main__":
    main()
