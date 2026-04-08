#!/usr/bin/env bash
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <python-version>" >&2
    exit 1
fi

ver="$1"

echo "===== Python $ver ====="

# Build C extension
case "$(uname -s)" in
    MINGW*|MSYS*)
        # Windows: use setuptools (MSVC) via build_ext --inplace
        uv run --python "$ver" python setup.py build_ext --inplace
        ;;
    *)
        PYTHON="uv run --python $ver python" make debug
        ;;
esac

# Run tests
uv run --quiet --python "$ver" pytest tests/ -q

# Run example regression tests
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
