#!/usr/bin/env bash
#
# Integration checks for Stages 2–3 against a live CinderHTTP process.
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

"$bin" --port "$port" --root "$repo_root/public" >/tmp/cinderhttp-integration.log 2>&1 &
pid=$!

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

check "GET / status 200" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/\" | grep -qx 200"
body="$(curl -sS "http://127.0.0.1:${port}/")"
check "GET / is HTML" bash -c "printf '%s' \"$body\" | grep -q 'CinderHTTP'"
ctype="$(curl -sS -D - -o /dev/null "http://127.0.0.1:${port}/" | tr -d '\r' | grep -i '^Content-Type:' || true)"
check "GET / Content-Type html" bash -c "printf '%s' \"$ctype\" | grep -qi 'text/html'"

check "GET CSS 200" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/css/style.css\" | grep -qx 200"
css_ctype="$(curl -sS -D - -o /dev/null "http://127.0.0.1:${port}/css/style.css" | tr -d '\r' | grep -i '^Content-Type:' || true)"
check "GET CSS Content-Type" bash -c "printf '%s' \"$css_ctype\" | grep -qi 'text/css'"

head_out="$(curl -sS -I "http://127.0.0.1:${port}/")"
check "HEAD status 200" bash -c "printf '%s' \"$head_out\" | head -n1 | grep -q '200'"
dl="$(curl -sS -o /dev/null -w '%{size_download}' --head "http://127.0.0.1:${port}/")"
check "HEAD no body bytes" bash -c "test \"$dl\" = 0"

check "404 missing" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/does-not-exist\" | grep -qx 404"
not_found_body="$(curl -sS "http://127.0.0.1:${port}/does-not-exist")"
check "custom 404 body" bash -c "printf '%s' \"$not_found_body\" | grep -q '404 Not Found'"

check "traversal blocked" bash -c "curl -sS --path-as-is -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/../../outside.txt\" | grep -qx 403"
check "encoded traversal blocked" bash -c "curl -sS --path-as-is -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/%2e%2e/etc/passwd\" | grep -qx 403"

check "POST still works" bash -c "curl -sS -o /dev/null -w '%{http_code}' -X POST --data hello \"http://127.0.0.1:${port}/test\" | grep -qx 200"
check "DELETE still 405" bash -c "curl -sS -o /dev/null -w '%{http_code}' -X DELETE \"http://127.0.0.1:${port}/\" | grep -qx 405"

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

echo "All Stage 3 integration checks passed."
