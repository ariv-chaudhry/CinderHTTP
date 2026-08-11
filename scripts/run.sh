#!/usr/bin/env bash
#
# Convenience wrapper: builds CinderHTTP if needed, then runs it, forwarding
# any arguments straight to the binary.
#
#   scripts/run.sh
#   scripts/run.sh --port 9000 --verbose
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(dirname "$script_dir")"

make -C "$repo_root" --silent all
exec "$repo_root/bin/cinderhttp" "$@"
