import os
from pathlib import Path
import shutil
import subprocess


PROJECT_ROOT = Path(__file__).parents[1]
TARGETS = ("3.12.13", "3.13.12", "3.14.3", "3.14.3t")


def bash_executable() -> str:
    if os.name != "nt":
        return "bash"
    git_executable = shutil.which("git")
    if git_executable is not None:
        for parent in Path(git_executable).parents:
            candidate = parent / "bin" / "bash.exe"
            if candidate.is_file():
                return str(candidate)
    raise RuntimeError("Git Bash is required to test run_tests.sh on Windows")


def prepare_runner(tmp_path: Path) -> tuple[Path, Path]:
    runner_dir = tmp_path / "runner"
    runner_dir.mkdir()
    shutil.copy(PROJECT_ROOT / "run_tests.sh", runner_dir / "run_tests.sh")
    state_dir = tmp_path / "state"
    state_dir.mkdir()
    child = runner_dir / "run_tests_one.sh"
    child.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
touch "$RUNNER_STATE/$1.started"
for _ in $(seq 1 100); do
    count=$(find "$RUNNER_STATE" -name '*.started' | wc -l)
    if [ "$count" -eq 4 ]; then
        break
    fi
    sleep 0.02
done
if [ "$count" -ne 4 ]; then
    echo "targets did not start concurrently" >&2
    exit 70
fi
printf '%s\n' "$UV_PROJECT_ENVIRONMENT" > "$RUNNER_STATE/$1.environment"
echo "completed $1"
if [ "${FAIL_TARGET:-}" = "$1" ]; then
    exit 23
fi
"""
    )
    child.chmod(0o755)
    return runner_dir, state_dir


def run_parallel_runner(
    runner_dir: Path,
    state_dir: Path,
    *,
    fail_target: str | None = None,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["RUNNER_STATE"] = str(state_dir)
    if fail_target is not None:
        environment["FAIL_TARGET"] = fail_target
    return subprocess.run(
        [bash_executable(), "run_tests.sh"],
        cwd=runner_dir,
        env=environment,
        text=True,
        capture_output=True,
        timeout=5,
        check=False,
    )


def test_test_targets_run_concurrently_in_isolated_environments(tmp_path: Path) -> None:
    runner_dir, state_dir = prepare_runner(tmp_path)

    result = run_parallel_runner(runner_dir, state_dir)

    assert result.returncode == 0, result.stderr
    environments = {(state_dir / f"{target}.environment").read_text().strip() for target in TARGETS}
    assert len(environments) == len(TARGETS)
    assert all(not Path(environment).exists() for environment in environments)
    assert all(f"completed {target}" in result.stdout for target in TARGETS)


def test_test_runner_reports_child_failure_after_waiting_for_all_targets(tmp_path: Path) -> None:
    runner_dir, state_dir = prepare_runner(tmp_path)

    result = run_parallel_runner(
        runner_dir,
        state_dir,
        fail_target="3.13.12",
    )

    assert result.returncode == 1
    assert "completed 3.13.12" in result.stdout
    assert "Python 3.13.12 failed" in result.stderr
    assert all((state_dir / f"{target}.environment").exists() for target in TARGETS)
