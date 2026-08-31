"""Helpers for comparing an isolated interpreter with and without aleff."""

from __future__ import annotations

import os
import subprocess
import sys
from typing import Final


_IMPORT_ALEFF: Final = "import aleff\n"


def _run_isolated(source: str, timeout: float) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["PYTHONIOENCODING"] = "utf-8"
    return subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
        env=environment,
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


def assert_cpython_compatible_after_prelude(
    prelude: str,
    source: str,
    timeout: float = 10,
) -> None:
    """Compare importing aleff after *prelude* with a pristine execution."""

    pristine = _run_isolated(prelude + "\n" + source, timeout)
    with_aleff = _run_isolated(prelude + "\n" + _IMPORT_ALEFF + source, timeout)
    pristine_output = (pristine.returncode, pristine.stdout, pristine.stderr)
    aleff_output = (with_aleff.returncode, with_aleff.stdout, with_aleff.stderr)
    assert aleff_output == pristine_output, (
        "interpreter output changed when aleff was imported after the prelude\n"
        f"pristine: {pristine_output!r}\n"
        f"with aleff: {aleff_output!r}\n"
        f"prelude:\n{prelude}\n"
        f"source:\n{source}"
    )
