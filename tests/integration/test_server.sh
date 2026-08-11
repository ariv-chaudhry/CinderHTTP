#!/usr/bin/env bash
#
# Stage 2 integration checks against a live CinderHTTP process.
# Starts the server on a dedicated port, runs curl (and nc when available),
# then shuts the server down.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
port="${CINDERHTTP_TEST_PORT:-18080}"
bin="$repo_root/bin/cinderhttp"
pid=""

cleanup() {
    if [[ -n "${pid}" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

make -C "$repo_root" --silent all

"$bin" --port "$port" >/tmp/cinderhttp-integration.log 2>&1 &
pid=$!

# Wait for the listening socket rather than a fixed sleep.
ready=0
for _ in $(seq 1 50); do
    if ss -ltn 2>/dev/null | grep -q ":${port}"; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
    echo "Server failed to listen on port ${port}"
    cat /tmp/cinderhttp-integration.log || true
    exit 1
fi

fail=0

check() {
    local name="$1"
    shift
    if "$@"; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        fail=1
    fi
}

check "GET status 200" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/\" | grep -qx 200"

body="$(curl -sS "http://127.0.0.1:${port}/")"
check "GET body" bash -c "printf '%s' \"$body\" | grep -q 'CinderHTTP request parsed successfully'"

head_out="$(curl -sS -I "http://127.0.0.1:${port}/")"
check "HEAD status line" bash -c "printf '%s' \"$head_out\" | head -n1 | grep -q '200'"
check "HEAD Content-Length" bash -c "printf '%s' \"$head_out\" | grep -qi 'Content-Length:'"
# size_download is the response body size; HEAD must transfer 0 body bytes.
dl="$(curl -sS -o /dev/null -w '%{size_download}' --head "http://127.0.0.1:${port}/")"
check "HEAD no body bytes" bash -c "test \"$dl\" = 0"

check "POST status 200" bash -c "curl -sS -o /dev/null -w '%{http_code}' -X POST --data hello \"http://127.0.0.1:${port}/test\" | grep -qx 200"
post_body="$(curl -sS -X POST --data hello "http://127.0.0.1:${port}/test")"
check "POST body" bash -c "printf '%s' \"$post_body\" | grep -q 'POST request parsed successfully'"

check "DELETE status 405" bash -c "curl -sS -o /dev/null -w '%{http_code}' -X DELETE \"http://127.0.0.1:${port}/\" | grep -qx 405"

if command -v nc >/dev/null 2>&1; then
    malformed_out="$(printf 'GET / NotHTTP\r\n\r\n' | nc -w 2 127.0.0.1 "$port" || true)"
    check "malformed request -> 400" bash -c "printf '%s' \"$malformed_out\" | head -n1 | grep -q '400'"
else
    echo "SKIP: nc not available for malformed-request check"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "Integration tests failed. Server log:"
    cat /tmp/cinderhttp-integration.log || true
    exit 1
fi

echo "All Stage 2 integration checks passed."
