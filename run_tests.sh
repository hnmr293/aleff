#!/usr/bin/env bash
set -euo pipefail

targets=(
    "3.12.13"
    "3.13.12"
    "3.14.3"
    "3.14.3t"
)

for ver in "${targets[@]}"; do
    ./run_tests_one.sh "$ver"
done
