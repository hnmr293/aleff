#!/usr/bin/env bash
set -euo pipefail

targets=(
    "3.12.13"
    "3.13.12"
    "3.14.3"
    "3.14.3t"
)

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
run_root=$(mktemp -d "${TMPDIR:-/tmp}/aleff-tests.XXXXXX")
pids=()
logs=()

cleanup() {
    for pid in "${pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
    rm -rf -- "$run_root"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

for index in "${!targets[@]}"; do
    ver=${targets[$index]}
    log="$run_root/$index.log"
    environment="$run_root/$index.venv"
    logs[$index]=$log
    UV_PROJECT_ENVIRONMENT="$environment" \
        "$script_dir/run_tests_one.sh" "$ver" >"$log" 2>&1 &
    pids[$index]=$!
done

failed=0
for index in "${!targets[@]}"; do
    ver=${targets[$index]}
    if wait "${pids[$index]}"; then
        cat "${logs[$index]}"
    else
        status=$?
        cat "${logs[$index]}"
        echo "Python $ver failed with status $status" >&2
        failed=1
    fi
done

exit "$failed"
