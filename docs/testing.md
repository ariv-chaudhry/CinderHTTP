# Testing

CinderHTTP’s tests focus on observable contracts and safety invariants:
boundary conditions, untrusted input, resource ownership, concurrency, and
shutdown behavior — not internal implementation trivia.

## Test categories

| Category | What it covers |
| --- | --- |
| Unit | Parser, response, MIME, static files, router, stats, queue, reliability, HTTP reader |
| Integration | Live server via curl + `tests/integration/raw_http.py` |
| Reliability | SIGPIPE/`send_all`, queue drain, malformed regressions (Stage 6) |
| Fuzz-style | Deterministic random + mutated inputs to `http_parse_request` |
| Sanitizer | ASan + UBSan via `make debug test` / `make sanitize` |
| Valgrind | Optional leak/origin checks via `make valgrind` |
| Coverage | Optional gcov summary via `make coverage` |

## Commands

```bash
make test                 # unit suite (includes ~2000 fuzz iterations)
make integration          # live Stage 2–7 checks
make sanitize             # ASan/UBSan unit tests
make debug test           # same, explicit debug flags in one make run
make fuzz                 # deeper parser fuzz (default 50,000 iterations)
CINDERHTTP_FUZZ_ITERS=100000 make fuzz
make valgrind             # skipped cleanly if Valgrind is missing
make coverage             # clean rebuild with --coverage + gcov summary
```

## Fuzzing

`tests/test_parser_fuzz.c` uses a fixed xorshift seed (`0xC1D3`) so runs are
reproducible.

- **Random strategy:** arbitrary bytes (including `NUL`, CR, LF) at varied lengths
- **Mutation strategy:** start from valid seed requests and apply deterministic
  mutations (replace/delete/insert/truncate/corrupt digits)
- Every iteration calls `http_request_init` → `http_parse_request` →
  `http_request_destroy`, whether parse succeeds or fails
- Override iterations with `CINDERHTTP_FUZZ_ITERS` (validated; capped)

This is lightweight adversarial regression coverage, not a production fuzzer
or formal verification claim.

## Raw HTTP helper

`tests/integration/raw_http.py` (Python 3 stdlib only) sends exact request
bytes, optional write half-close, chunked sends with pauses, and a bounded
socket timeout so the integration suite cannot hang forever.

## Philosophy

Prefer tests that would catch real regressions: fragmented framing, limit±1
sizes, malformed metadata, filesystem confinement, HEAD length correctness,
queue/stats concurrency, and live-server survival after bad clients.
