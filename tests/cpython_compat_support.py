"""Helpers for comparing an isolated interpreter with and without aleff."""

from __future__ import annotations

import subprocess
import sys
from typing import Final


_IMPORT_ALEFF: Final = "import aleff\n"


def _run_isolated(source: str, timeout: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def assert_cpython_compatible(source: str, timeout: float = 10) -> None:
    """Assert that *source* has identical process output before and after importing aleff."""

    pristine = _run_isolated(source, timeout)
    with_aleff = _run_isolated(_IMPORT_ALEFF + source, timeout)

    pristine_output = (pristine.returncode, pristine.stdout, pristine.stderr)
    aleff_output = (with_aleff.returncode, with_aleff.stdout, with_aleff.stderr)
    assert aleff_output == pristine_output, (
        "interpreter output changed after importing aleff\n"
        f"pristine: {pristine_output!r}\n"
        f"with aleff: {aleff_output!r}\n"
        f"source:\n{source}"
    )
