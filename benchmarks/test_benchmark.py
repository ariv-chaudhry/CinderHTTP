#!/usr/bin/env python3
"""Unit tests for CinderHTTP benchmark helpers (no wrk/hey/ab required)."""

from __future__ import annotations

import unittest

from benchmark import (
    aggregate_runs,
    parse_ab_output,
    parse_bytes_per_sec,
    parse_hey_output,
    parse_latency_to_ms,
    parse_rate,
    parse_workers_list,
    parse_wrk_output,
    RunResult,
)


class LatencyParsingTests(unittest.TestCase):
    def test_ms_us_s(self) -> None:
        self.assertAlmostEqual(parse_latency_to_ms("4.12ms"), 4.12)
        self.assertAlmostEqual(parse_latency_to_ms("412us"), 0.412)
        self.assertAlmostEqual(parse_latency_to_ms("1.2s"), 1200.0)
        self.assertAlmostEqual(parse_latency_to_ms("500"), 500.0)

    def test_rejects_garbage(self) -> None:
        with self.assertRaises(ValueError):
            parse_latency_to_ms("")
        with self.assertRaises(ValueError):
            parse_latency_to_ms("fast")


class RateParsingTests(unittest.TestCase):
    def test_suffixes(self) -> None:
        self.assertAlmostEqual(parse_rate("12345.67"), 12345.67)
        self.assertAlmostEqual(parse_rate("12.3k"), 12300.0)
        self.assertAlmostEqual(parse_rate("1.2M"), 1_200_000.0)

    def test_transfer(self) -> None:
        self.assertAlmostEqual(parse_bytes_per_sec("10.5KB"), 10.5 * 1024)
        self.assertAlmostEqual(parse_bytes_per_sec("2MB"), 2 * 1024**2)


class WorkersValidationTests(unittest.TestCase):
    def test_ok(self) -> None:
        self.assertEqual(parse_workers_list("1,2,4,8"), [1, 2, 4, 8])

    def test_rejects_zero_negative_dup_bad(self) -> None:
        with self.assertRaises(ValueError):
            parse_workers_list("0")
        with self.assertRaises(ValueError):
            parse_workers_list("-1")
        with self.assertRaises(ValueError):
            parse_workers_list("1,1")
        with self.assertRaises(ValueError):
            parse_workers_list("abc")
        with self.assertRaises(ValueError):
            parse_workers_list("")


class WrkParserTests(unittest.TestCase):
    SAMPLE = """\
Running 10s test @ http://127.0.0.1:18081/api/health
  4 threads and 64 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.23ms  500.00us   9.00ms   80.00%
    Req/Sec     3.10k   200.00     4.00k    75.00%
  Latency Distribution
     50%    1.00ms
     75%    1.50ms
     90%    2.00ms
     99%    5.00ms
  123456 requests in 10.00s, 20.00MB read
Requests/sec:  12345.67
Transfer/sec:      2.00MB
CINDERHTTP_WRK_REQUESTS=123456
CINDERHTTP_WRK_DURATION_S=10.000000
CINDERHTTP_WRK_RPS=12345.600000
CINDERHTTP_WRK_AVG_MS=1.234000
CINDERHTTP_WRK_P95_MS=2.500000
CINDERHTTP_WRK_MAX_MS=9.000000
CINDERHTTP_WRK_BYTES_PER_S=2097152.000000
CINDERHTTP_WRK_ERRORS=0
"""

    def test_prefers_tagged_p95(self) -> None:
        parsed = parse_wrk_output(self.SAMPLE)
        self.assertAlmostEqual(parsed["requests_per_second"], 12345.6)
        self.assertAlmostEqual(parsed["average_latency_ms"], 1.234)
        self.assertAlmostEqual(parsed["p95_latency_ms"], 2.5)
        self.assertEqual(parsed["total_requests"], 123456)
        self.assertEqual(parsed["errors"], 0)

    def test_plain_wrk_no_fake_p95(self) -> None:
        plain = """\
Requests/sec:  1000.00
  Latency     2.00ms
  10000 requests in 10.00s, 1.00MB read
  Latency Distribution
     50%    1.00ms
     90%    3.00ms
     99%    8.00ms
"""
        parsed = parse_wrk_output(plain)
        self.assertAlmostEqual(parsed["requests_per_second"], 1000.0)
        self.assertIsNone(parsed["p95_latency_ms"])
        self.assertTrue(any("p95 unavailable" in n for n in parsed["notes"]))


class HeyParserTests(unittest.TestCase):
    SAMPLE = """\
Summary:
  Total:	1.0000 secs
  Slowest:	0.0200 secs
  Fastest:	0.0010 secs
  Average:	0.0040 secs
  Requests/sec:	2500.0000

Latency distribution:
  10% in 0.0010 secs
  50% in 0.0030 secs
  95% in 0.0100 secs
  99% in 0.0150 secs

Status code distribution:
  [200]	2500 responses
"""

    def test_hey(self) -> None:
        parsed = parse_hey_output(self.SAMPLE)
        self.assertAlmostEqual(parsed["requests_per_second"], 2500.0)
        self.assertAlmostEqual(parsed["average_latency_ms"], 4.0)
        self.assertAlmostEqual(parsed["p95_latency_ms"], 10.0)
        self.assertEqual(parsed["total_requests"], 2500)
        self.assertEqual(parsed["non_2xx"], 0)


class AbParserTests(unittest.TestCase):
    SAMPLE = """\
Complete requests:      10000
Failed requests:        0
Non-2xx responses:      0
Requests per second:    5000.12 [#/sec] (mean)
Time per request:       12.800 [ms] (mean)
Time per request:       0.200 [ms] (mean, across all concurrent requests)
Transfer rate:          800.00 [Kbytes/sec] received

Percentage of the requests served within a certain time (ms)
  50%      10
  66%      11
  75%      12
  80%      13
  90%      15
  95%      18
  98%      22
  99%      30
 100%      50 (longest request)
"""

    def test_ab_mean_and_p95(self) -> None:
        parsed = parse_ab_output(self.SAMPLE)
        self.assertAlmostEqual(parsed["requests_per_second"], 5000.12)
        self.assertAlmostEqual(parsed["average_latency_ms"], 12.8)
        self.assertAlmostEqual(parsed["p95_latency_ms"], 18.0)
        self.assertEqual(parsed["total_requests"], 10000)
        self.assertEqual(parsed["errors"], 0)


class AggregateTests(unittest.TestCase):
    def test_mean_stdev(self) -> None:
        runs = [
            RunResult(1, 1, requests_per_second=100.0, average_latency_ms=2.0, p95_latency_ms=3.0),
            RunResult(1, 2, requests_per_second=110.0, average_latency_ms=2.2, p95_latency_ms=3.2),
        ]
        agg = aggregate_runs(runs)
        self.assertAlmostEqual(agg["requests_per_second_mean"], 105.0)
        self.assertIsNotNone(agg["requests_per_second_stdev"])
        self.assertAlmostEqual(agg["p95_latency_ms_mean"], 3.1)


if __name__ == "__main__":
    unittest.main()
