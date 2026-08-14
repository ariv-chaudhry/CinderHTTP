-- Machine-parseable summary for CinderHTTP benchmarks (wrk --script).
-- Emits CINDERHTTP_WRK_* lines; does not replace wrk's normal console output.
done = function(summary, latency, requests)
  local duration_s = summary.duration / 1000000
  local rps = 0
  if duration_s > 0 then
    rps = summary.requests / duration_s
  end
  local avg_ms = latency.mean / 1000
  local p95_ms = latency:percentile(95.0) / 1000
  local max_ms = latency.max / 1000
  local err = summary.errors.connect + summary.errors.read + summary.errors.write
               + summary.errors.status + summary.errors.timeout
  local bytes_per_s = 0
  if duration_s > 0 then
    bytes_per_s = summary.bytes / duration_s
  end

  io.write(string.format("CINDERHTTP_WRK_REQUESTS=%d\n", summary.requests))
  io.write(string.format("CINDERHTTP_WRK_DURATION_S=%.6f\n", duration_s))
  io.write(string.format("CINDERHTTP_WRK_RPS=%.6f\n", rps))
  io.write(string.format("CINDERHTTP_WRK_AVG_MS=%.6f\n", avg_ms))
  io.write(string.format("CINDERHTTP_WRK_P95_MS=%.6f\n", p95_ms))
  io.write(string.format("CINDERHTTP_WRK_MAX_MS=%.6f\n", max_ms))
  io.write(string.format("CINDERHTTP_WRK_BYTES_PER_S=%.6f\n", bytes_per_s))
  io.write(string.format("CINDERHTTP_WRK_ERRORS=%d\n", err))
  io.write(string.format("CINDERHTTP_WRK_CONNECT_ERRORS=%d\n", summary.errors.connect))
  io.write(string.format("CINDERHTTP_WRK_READ_ERRORS=%d\n", summary.errors.read))
  io.write(string.format("CINDERHTTP_WRK_WRITE_ERRORS=%d\n", summary.errors.write))
  io.write(string.format("CINDERHTTP_WRK_STATUS_ERRORS=%d\n", summary.errors.status))
  io.write(string.format("CINDERHTTP_WRK_TIMEOUT_ERRORS=%d\n", summary.errors.timeout))
end
