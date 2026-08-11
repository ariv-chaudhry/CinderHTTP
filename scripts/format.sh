#!/usr/bin/env bash
#
# Formats project C sources in place with clang-format, using the .clang-format
# style file at the repository root. Formatting is a convenience, not a
# correctness tool, so a missing clang-format is reported and skipped rather
# than treated as a build failure.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not found; skipping formatting." >&2
    exit 0
fi

mapfile -t files < <(find "$repo_root/src" "$repo_root/include" "$repo_root/tests" \
    -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null)

if [ "${#files[@]}" -eq 0 ]; then
    echo "No C sources found to format yet."
    exit 0
fi

echo "Formatting ${#files[@]} file(s) with clang-format..."
clang-format -i "${files[@]}"
