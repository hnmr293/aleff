"""
Demo: N-Queens (backtracking search via multi-shot continuations)

Place N queens on an NxN board so that no two queens threaten each other.
The body code reads as a simple forward computation: "for each row, pick a
column."  The handler explores all column choices via multi-shot, and
pruning happens naturally when the body rejects an invalid placement.

Multi-shot pattern:
  The handler calls k(col) for each column 0..N-1 and concatenates the
  results.  Each resumption runs from the same suspension point with
  independent local state.
"""

from aleff import effect, Effect, Handler, Resume, create_handler, wind_range


# ---------------------------------------------------------------------------
# Effects
# ---------------------------------------------------------------------------

choose_col: Effect[[int], int] = effect("choose_col")


# ---------------------------------------------------------------------------
# Solver (pure logic -- no awareness of search strategy)
# ---------------------------------------------------------------------------


def is_safe(queens: tuple[int, ...], row: int, col: int) -> bool:
    """Check whether placing a queen at (row, col) conflicts with existing queens."""
    for r, c in enumerate(queens):
        if c == col or abs(c - col) == abs(r - row):
            return False
    return True


def solve(n: int) -> list[list[int]]:
    """Find all solutions to the N-Queens problem.

    Returns a list of solutions.  Each solution is a list of column indices,
    one per row (e.g. [1, 3, 0, 2] means row0->col1, row1->col3, ...).

    Uses ``wind_range`` instead of ``range()`` because the range iterator
    is a mutable object shared across multi-shot continuations.
    ``wind_range`` saves and restores the iterator position via the wind
    snapshot/restore mechanism.
    """
    queens: tuple[int, ...] = ()

    with wind_range(n) as rows:
        for row in rows:
            col = choose_col(n)
            if not is_safe(queens, row, col):
                return []  # dead end -- prune this branch
            queens = (*queens, col)

    return [list(queens)]


# ---------------------------------------------------------------------------
# Handler: enumerate all columns via multi-shot
# ---------------------------------------------------------------------------


def run_nqueens(n: int) -> list[list[int]]:
    h: Handler[list[list[int]]] = create_handler(choose_col)

    @h.on(choose_col)
    def _choose(k: Resume[int, list[list[int]]], n: int) -> list[list[int]]:
        results: list[list[int]] = []
        for col in range(n):
            results.extend(k(col))
        return results

    return h(lambda: solve(n))


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------


def print_board(queens: list[int]) -> None:
    n = len(queens)
    for row in range(n):
        line = ""
        for col in range(n):
            line += " Q" if queens[row] == col else " ."
        print(f"  {line}")
    print()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


EXPECTED_COUNTS = {1: 1, 4: 2, 5: 10, 6: 4, 8: 92}


def main() -> None:
    for n in [1, 4, 5, 6, 8]:
        solutions = run_nqueens(n)
        print(f"N={n}: {len(solutions)} solutions")

        if n <= 5:
            for i, sol in enumerate(solutions):
                print(f"  --- solution {i + 1} ---")
                print_board(sol)

        assert len(solutions) == EXPECTED_COUNTS[n], (
            f"expected {EXPECTED_COUNTS[n]} solutions for N={n}, got {len(solutions)}"
        )
        print("  [OK]")
        print()

    print("All N-Queens demos passed.")


if __name__ == "__main__":
    main()
