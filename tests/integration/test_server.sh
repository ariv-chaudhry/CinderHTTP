#!/usr/bin/env bash
#
# Integration checks for Stages 2–6 against a live CinderHTTP process.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
port="${CINDERHTTP_TEST_PORT:-18080}"
bin="$repo_root/bin/cinderhttp"
pid=""
log="/tmp/cinderhttp-integration.log"

cleanup() {
    if [[ -n "${pid}" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
        # Bounded wait so a hung shutdown fails the suite instead of hanging forever.
        for _ in $(seq 1 50); do
            if ! kill -0 "$pid" 2>/dev/null; then
                wait "$pid" 2>/dev/null || true
                pid=""
                return
            fi
            sleep 0.1
        done
        echo "WARN: server did not exit after SIGINT; sending SIGKILL"
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        pid=""
    fi
}
trap cleanup EXIT

wait_for_ready() {
    local ready=0
    for _ in $(seq 1 50); do
        if curl -fsS "http://127.0.0.1:${port}/api/health" >/dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "Server failed to become ready on port ${port}"
        cat "$log" || true
        exit 1
    fi
}

start_server() {
    local extra_args=("$@")
    cleanup
    : >"$log"
    "$bin" --port "$port" --root "$repo_root/public" "${extra_args[@]}" >"$log" 2>&1 &
    pid=$!
    wait_for_ready
}

stop_server_signal() {
    local sig="$1"
    if [[ -z "${pid}" ]]; then
        return
    fi
    kill "-${sig}" "$pid" 2>/dev/null || true
    local exited=0
    for _ in $(seq 1 50); do
        if ! kill -0 "$pid" 2>/dev/null; then
            exited=1
            break
        fi
        sleep 0.1
    done
    wait "$pid" 2>/dev/null || true
    pid=""
    [[ "$exited" -eq 1 ]]
}

make -C "$repo_root" --silent all

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

# Body/haystack checks without nested bash -c (JSON quotes break that pattern).
check_contains() {
    local name="$1"
    local haystack="$2"
    local needle="$3"
    if printf '%s' "$haystack" | grep -q -- "$needle"; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        fail=1
    fi
}

check_regex() {
    local name="$1"
    local haystack="$2"
    local regex="$3"
    if printf '%s' "$haystack" | grep -Eq -- "$regex"; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        fail=1
    fi
}

# --- Stage 3 baseline with Stage 4 concurrency defaults ---
start_server --workers 4 --queue-size 16

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

check "POST /test is 405" bash -c "curl -sS -o /dev/null -w '%{http_code}' -X POST --data hello \"http://127.0.0.1:${port}/test\" | grep -qx 405"
check "DELETE still 405" bash -c "curl -sS -o /dev/null -w '%{http_code}' -X DELETE \"http://127.0.0.1:${port}/\" | grep -qx 405"

if command -v nc >/dev/null 2>&1; then
    malformed_out="$(printf 'GET / NotHTTP\r\n\r\n' | nc -w 2 127.0.0.1 "$port" || true)"
    check "malformed request -> 400" bash -c "printf '%s' \"$malformed_out\" | head -n1 | grep -q '400'"
else
    echo "SKIP: nc not available for malformed-request check"
fi

# --- Stage 5 API endpoints ---
health="$(curl -sS "http://127.0.0.1:${port}/api/health")"
check "GET /api/health 200" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/api/health\" | grep -qx 200"
check_contains "GET /api/health JSON body" "$health" '"status":"ok"'
health_ctype="$(curl -sS -D - -o /dev/null "http://127.0.0.1:${port}/api/health" | tr -d '\r' | grep -i '^Content-Type:' || true)"
check_contains "GET /api/health Content-Type" "$health_ctype" "application/json"
check "GET /api/health?query" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/api/health?test=1\" | grep -qx 200"

health_head_dl="$(curl -sS -o /dev/null -w '%{size_download}' --head "http://127.0.0.1:${port}/api/health")"
check "HEAD /api/health no body" bash -c "test \"$health_head_dl\" = 0"

echo_body="$(curl -sS -X POST -H 'Content-Type: text/plain' --data-binary 'hello cinder' "http://127.0.0.1:${port}/api/echo")"
check "POST /api/echo body" bash -c "test \"$echo_body\" = 'hello cinder'"
echo_ctype="$(curl -sS -D - -o /dev/null -X POST -H 'Content-Type: text/plain' --data-binary 'hello cinder' "http://127.0.0.1:${port}/api/echo" | tr -d '\r' | grep -i '^Content-Type:' || true)"
check_contains "POST /api/echo Content-Type" "$echo_ctype" "text/plain"

json_echo="$(curl -sS -X POST -H 'Content-Type: application/json' --data-binary '{"message":"hello"}' "http://127.0.0.1:${port}/api/echo")"
check "POST /api/echo JSON bytes" test "$json_echo" = '{"message":"hello"}'
json_echo_ct="$(curl -sS -D - -o /dev/null -X POST -H 'Content-Type: application/json' --data-binary '{"message":"hello"}' "http://127.0.0.1:${port}/api/echo" | tr -d '\r' | grep -i '^Content-Type:' || true)"
check_contains "POST /api/echo JSON Content-Type" "$json_echo_ct" "application/json"

echo_get_headers="$(curl -sS -D - -o /dev/null "http://127.0.0.1:${port}/api/echo" | tr -d '\r')"
check_contains "GET /api/echo 405" "$echo_get_headers" "405"
check_contains "GET /api/echo Allow POST" "$(printf '%s\n' "$echo_get_headers" | grep -i '^Allow:' || true)" "POST"

check "GET /api/does-not-exist 404" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/api/does-not-exist\" | grep -qx 404"
api_404="$(curl -sS "http://127.0.0.1:${port}/api/does-not-exist")"
check_contains "API 404 is JSON" "$api_404" "not found"

stats_json="$(curl -sS "http://127.0.0.1:${port}/api/stats")"
check "GET /api/stats 200" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/api/stats\" | grep -qx 200"
check_contains "stats has connections_accepted" "$stats_json" "connections_accepted"
check_contains "stats has requests_total" "$stats_json" "requests_total"
check_contains "stats has responses_2xx" "$stats_json" "responses_2xx"
check_contains "stats has responses_4xx" "$stats_json" "responses_4xx"
check_contains "stats has responses_5xx" "$stats_json" "responses_5xx"
check_contains "stats has active_connections" "$stats_json" "active_connections"
check_regex "stats requests_total nonzero" "$stats_json" '"requests_total":[1-9][0-9]*'

stats_head_dl="$(curl -sS -o /dev/null -w '%{size_download}' --head "http://127.0.0.1:${port}/api/stats")"
check "HEAD /api/stats no body" bash -c "test \"$stats_head_dl\" = 0"

# Concurrent health + stats counter growth
seq 1 40 | xargs -P 8 -I{} curl -fsS "http://127.0.0.1:${port}/api/health" >/dev/null
stats_after="$(curl -sS "http://127.0.0.1:${port}/api/stats")"
check_regex "concurrent health then stats alive" "$stats_after" '"requests_total":[1-9][0-9]*'
check "static homepage still works" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/\" | grep -qx 200"

# Concurrent requests against the 4-worker pool.
tmp_codes="$(mktemp)"
seq 1 40 | xargs -P 8 -I{} bash -c "
    path='/'
    case \$(( {} % 3 )) in
        0) path='/' ;;
        1) path='/css/style.css' ;;
        2) path='/does-not-exist' ;;
    esac
    curl -sS -o /dev/null -w '%{http_code}\n' \"http://127.0.0.1:${port}\${path}\"
" >"$tmp_codes"
ok_count="$(grep -E '^(200|404)$' "$tmp_codes" | wc -l | tr -d ' ')"
line_count="$(wc -l <"$tmp_codes" | tr -d ' ')"
check "concurrent 40 requests all answered" bash -c "test \"$line_count\" = 40"
check "concurrent statuses only 200/404" bash -c "test \"$ok_count\" = 40"
rm -f "$tmp_codes"

# Startup banner reflects concurrency settings.
check "startup shows workers=4" bash -c "grep -q 'workers=4' \"$log\""
check "startup shows queue=16" bash -c "grep -q 'queue=16' \"$log\""

# --- Single worker mode ---
start_server --workers 1 --queue-size 16
check "workers=1 GET /" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/\" | grep -qx 200"
check "workers=1 /api/health" bash -c "curl -sS -o /dev/null -w '%{http_code}' \"http://127.0.0.1:${port}/api/health\" | grep -qx 200"
check "workers=1 startup banner" bash -c "grep -q 'workers=1' \"$log\""

# --- Small queue backpressure ---
start_server --workers 1 --queue-size 1
tmp_small="$(mktemp)"
seq 1 20 | xargs -P 10 -I{} curl -sS -o /dev/null -w '%{http_code}\n' "http://127.0.0.1:${port}/" >"$tmp_small"
small_ok="$(grep -c '^200$' "$tmp_small" || true)"
check "queue-size=1 concurrent survives" bash -c "test \"$small_ok\" = 20"
check "queue-size=1 server still alive" bash -c "kill -0 \"$pid\""
rm -f "$tmp_small"

# --- Shutdown under concurrency (SIGINT) ---
start_server --workers 4 --queue-size 8
seq 1 30 | xargs -P 10 -I{} curl -sS -o /dev/null "http://127.0.0.1:${port}/api/health" >/dev/null 2>&1 &
load_pid=$!
sleep 0.2
check "SIGINT under load exits" stop_server_signal INT
wait "$load_pid" 2>/dev/null || true

# --- Stage 6: SIGTERM clean exit ---
start_server --workers 2 --queue-size 8
check "SIGTERM preflight health" bash -c "curl -fsS \"http://127.0.0.1:${port}/api/health\" | grep -q status"
check "SIGTERM clean exit" stop_server_signal TERM

# --- Stage 6: repeated start/stop ---
restart_ok=1
for i in $(seq 1 5); do
    start_server --workers 2 --queue-size 8
    if ! curl -fsS "http://127.0.0.1:${port}/api/health" >/dev/null; then
        restart_ok=0
        break
    fi
    if ! stop_server_signal TERM; then
        restart_ok=0
        break
    fi
done
check "repeated start/stop x5" bash -c "test \"$restart_ok\" = 1"

# --- Stage 6: client disconnect during response must not kill server ---
start_server --workers 2 --queue-size 8
# Open a connection and abort before finishing the response read.
(
  exec 3<>"/dev/tcp/127.0.0.1/${port}"
  printf 'GET /api/health HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
  exec 3<&-
  exec 3>&-
) 2>/dev/null || true
check "server alive after client abort" bash -c "curl -fsS \"http://127.0.0.1:${port}/api/health\" | grep -q status"
check "server still up after abort" bash -c "kill -0 \"$pid\""

# --- Stage 7: raw malformed / fragmented clients ---
raw_http="$script_dir/raw_http.py"
if command -v python3 >/dev/null 2>&1; then
    raw_status() {
        local name="$1"
        local expect="$2"
        shift 2
        local out
        out="$(python3 "$raw_http" 127.0.0.1 "$port" --timeout 2 --half-close "$@" 2>/dev/null || true)"
        local line
        line="$(printf '%s' "$out" | head -n1 | tr -d '\r')"
        if printf '%s' "$line" | grep -q "HTTP/1.1 ${expect}"; then
            echo "PASS: $name"
        else
            echo "FAIL: $name (expected status ${expect}, got: ${line:-<empty>})"
            fail=1
        fi
    }

    raw_status "raw invalid version -> 505" 505 \
        'GET / HTTP/2.0\r\nHost: localhost\r\n\r\n'
    raw_status "raw unsupported method -> 405" 405 \
        'PUT / HTTP/1.1\r\nHost: localhost\r\n\r\n'
    raw_status "raw malformed request line -> 400" 400 \
        'GET /\r\nHost: localhost\r\n\r\n'
    raw_status "raw header without colon -> 400" 400 \
        'GET / HTTP/1.1\r\nBadHeader\r\n\r\n'
    raw_status "raw invalid Content-Length -> 400" 400 \
        'POST /api/echo HTTP/1.1\r\nContent-Length: abc\r\n\r\n'
    raw_status "raw duplicate Content-Length -> 400" 400 \
        'POST /api/echo HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n'
    raw_status "raw Transfer-Encoding chunked -> 501" 501 \
        'POST /api/echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n'

    # Truncated headers / body: server must stay alive (response optional).
    python3 "$raw_http" 127.0.0.1 "$port" --timeout 1 --half-close \
        'GET / HTTP/1.1\r\nHost:' >/dev/null 2>&1 || true
    python3 "$raw_http" 127.0.0.1 "$port" --timeout 1 --half-close \
        'POST /api/echo HTTP/1.1\r\nContent-Length: 100\r\n\r\nshort' >/dev/null 2>&1 || true
    check "alive after truncated raw requests" bash -c "curl -fsS \"http://127.0.0.1:${port}/api/health\" | grep -q status"

    # Malformed burst then health.
    for _ in $(seq 1 12); do
        python3 "$raw_http" 127.0.0.1 "$port" --timeout 1 --half-close \
            'GET / NotHTTP\r\n\r\n' >/dev/null 2>&1 || true
        python3 "$raw_http" 127.0.0.1 "$port" --timeout 1 --half-close \
            'POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n' >/dev/null 2>&1 || true
    done
    check "health after malformed sequence" bash -c "curl -fsS \"http://127.0.0.1:${port}/api/health\" | grep -q status"

    # Connect-and-drop without sending.
    for _ in $(seq 1 8); do
        python3 - "$port" <<'PY' >/dev/null 2>&1 || true
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1.0)
s.close()
PY
    done
    check "health after empty connects" bash -c "curl -fsS \"http://127.0.0.1:${port}/api/health\" | grep -q status"

    # Fragmented valid GET across pauses.
    frag_out="$(python3 "$raw_http" 127.0.0.1 "$port" --timeout 3 --half-close --chunk-pause 0.05 \
        --chunk 'GET /api/health' \
        --chunk ' HTTP/1.1\r\nHost:' \
        --chunk ' localhost\r\n\r\n' 2>/dev/null || true)"
    check_contains "fragmented GET health status" "$(printf '%s' "$frag_out" | head -n1 | tr -d '\r')" "200"
    check_contains "fragmented GET health body" "$frag_out" '"status":"ok"'

    # Fragmented POST echo body.
    echo_out="$(python3 "$raw_http" 127.0.0.1 "$port" --timeout 3 --half-close --chunk-pause 0.02 \
        --chunk 'POST /api/echo HTTP/1.1\r\nContent-Length: 11\r\n\r\n' \
        --chunk 'hello ' \
        --chunk 'world' 2>/dev/null || true)"
    echo_body="$(printf '%s' "$echo_out" | python3 -c 'import sys; d=sys.stdin.buffer.read(); i=d.find(b"\r\n\r\n"); sys.stdout.buffer.write(d[i+4:] if i>=0 else b"")')"
    check "fragmented POST echo body" bash -c "test \"$echo_body\" = 'hello world'"

    # Concurrent malformed burst (bounded parallelism; each client has a timeout).
    python3 - "$port" <<'PY'
import socket, sys, concurrent.futures
port = int(sys.argv[1])
payload = b"GET / HTTP/9.9\r\nHost: x\r\n\r\n"

def one(_):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=1.0)
        s.settimeout(1.0)
        try:
            s.sendall(payload)
            s.shutdown(socket.SHUT_WR)
            while s.recv(4096):
                pass
        finally:
            s.close()
    except OSError:
        pass

with concurrent.futures.ThreadPoolExecutor(max_workers=16) as ex:
    list(ex.map(one, range(80)))
PY
    check "alive after malformed burst" bash -c "curl -fsS --max-time 2 \"http://127.0.0.1:${port}/api/health\" | grep -q status"
    burst_stats="$(curl -sS --max-time 2 "http://127.0.0.1:${port}/api/stats")"
    check_contains "stats after malformed burst" "$burst_stats" "requests_total"
else
    echo "SKIP: python3 not available for Stage 7 raw HTTP checks"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "Integration tests failed. Server log:"
    cat "$log" || true
    exit 1
fi

echo "All Stage 2–7 integration checks passed."
