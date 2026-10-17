#ifndef GUFO_SERVER_DASHBOARD_RECORDER_HPP_
#define GUFO_SERVER_DASHBOARD_RECORDER_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/cli/serve/text_generation_backend.hpp"

namespace gufo::server::dashboard {

/// Metadata-only twin of the request log: status, token counts, timing and
/// cancellation state. It never stores prompt text, model paths, machine
/// identity or token ids (docs/SERVER.md telemetry policy).
struct RequestRecord {
  std::string id;
  std::string method;
  std::string path;
  std::string client;
  long long started_at{0};
  double duration_ms{0.0};
  int status{0};
  std::string outcome;
  bool stream{false};
  /// True once a text generation attached its per-request metrics.
  bool generation{false};
  std::size_t prompt_tokens{0};
  std::size_t completion_tokens{0};
  std::size_t cached_prompt_tokens{0};
  double ttft_ms{0.0};
  double queue_ms{0.0};
  double prefill_ms{0.0};
  double decode_ms{0.0};
  double prompt_tps{0.0};
  double gen_tps{0.0};
  std::string finish;
  std::string cache;
  std::size_t queue_depth{0};
  std::string plan;
  std::size_t draft_proposed{0};
  std::size_t draft_accepted{0};
};

/// Per-client token totals. A client is the transport-derived peer address
/// (gufo's `--sessions` pool is a separate, per-GPU-slot concept), so these
/// answer "what has each session consumed" without storing any content.
struct ClientMetrics {
  std::string client;
  std::uint64_t requests{0};
  std::uint64_t active{0};
  std::uint64_t generations{0};
  std::uint64_t prompt_tokens{0};
  std::uint64_t completion_tokens{0};
  std::uint64_t cached_prompt_tokens{0};
  /// Prefill work actually executed (cache hits excluded), so averages over
  /// `prefill_ms` stay honest.
  std::uint64_t prefill_tokens{0};
  std::uint64_t draft_proposed{0};
  std::uint64_t draft_accepted{0};
  /// Accumulated stage times, so the UI averages over the client's whole
  /// history instead of one request.
  double prefill_ms{0.0};
  double decode_ms{0.0};
  long long last_seen{0};
  std::string last_method;
  std::string last_path;
};

/// Bounded ring of finished requests plus the thread-local in-flight record
/// that `note_generation()` fills on the same connection thread.
class DashboardRecorder {
public:
  static constexpr std::size_t kMaxRecords = 100;
  static constexpr std::size_t kMaxClients = 32;

  /// Opens the thread-local record for one connection. `tracked` follows the
  /// request-log predicate: quiet health/metrics/dashboard polling must not
  /// inflate per-client totals.
  void begin(std::string id, std::string method, std::string path,
             std::string client, bool tracked = true);

  /// Attaches one generation `Result` to the calling thread's open record.
  void note_generation(const TextGenerationBackend::Result& result);

  /// Closes the thread-local record; commits it only when `record` holds.
  void finish(int status, double duration_ms, std::string_view outcome,
              bool stream, bool record);

  /// Newest first, capped at `kMaxRecords`.
  [[nodiscard]] std::deque<RequestRecord> list() const;

  /// Per-client totals, most recently active first. Bounded by
  /// `kMaxClients`: a new client retires the least recently active idle one.
  [[nodiscard]] std::vector<ClientMetrics> clients() const;

  [[nodiscard]] std::uint64_t active_count() const noexcept {
    return active_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t total_requests() const noexcept {
    return total_.load(std::memory_order_relaxed);
  }

private:
  struct ClientStats {
    ClientMetrics metrics;
    std::uint64_t touched{0};
  };

  /// Caller holds `mutex_`. Inserts the client when unknown, retiring the
  /// least recently touched idle entry once the table is full.
  ClientStats& Track(const std::string& client);

  /// Closes one open slot; quiet paths carry no per-client entry to release.
  void ReleaseActive(const std::string& client, bool tracked);

  mutable std::mutex mutex_;
  std::deque<RequestRecord> records_;
  std::unordered_map<std::string, ClientStats> clients_;
  std::uint64_t touch_clock_{0};
  std::atomic<std::uint64_t> active_{0};
  std::atomic<std::uint64_t> total_{0};
};

/// Process-wide recorder; same function-local-static pattern as the server
/// metric counters in http_server.hpp.
DashboardRecorder& Recorder();

}  // namespace gufo::server::dashboard

#endif  // GUFO_SERVER_DASHBOARD_RECORDER_HPP_
