#!/usr/bin/env bash
set -euo pipefail

targets=(
    "3.12.13"
    "3.13.12"
    "3.14.3"
    "3.14.3t"
)

for ver in "${targets[@]}"; do
    PYTHON="uv run --python $ver python" make debug
done

for ver in "${targets[@]}"; do
    echo "===== Python $ver ====="
    uv run --quiet --python "$ver" pytest tests/ -q
    echo
done

for ver in "${targets[@]}"; do
    echo "===== Examples Python $ver ====="
    all=0
    success=0
    skipped=0
    fail=0
    for f in examples/demo_*.py; do
        all=$((all + 1))
        name=$(basename "$f" .py)
        expected="examples/expected/${name}.txt"
        if [ ! -f "$expected" ]; then
            echo "  SKIP $name (no expected output)"
            skipped=$((skipped + 1))
            continue
        fi
        actual=$(uv run --quiet --python "$ver" python "$f" 2>&1)
        if diff -u "$expected" <(echo "$actual") > /dev/null 2>&1; then
            # OK
            success=$((success + 1))
        else
            echo "  FAIL $name"
            diff -u "$expected" <(echo "$actual") || true
            fail=1
        fi
    done
    if [ "$fail" -eq 1 ]; then
        echo "Examples regression test FAILED (Python $ver)"
        exit 1
    else
        echo "PASSED $success/$all examples (Python $ver)"
    fi
    echo
done
