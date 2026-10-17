#include "src/cli/serve/dashboard_recorder.hpp"

#include <algorithm>
#include <ctime>
#include <optional>
#include <utility>

#include "src/cli/serve/generation_metrics.hpp"

namespace gufo::server::dashboard {
namespace {

struct ActiveRecord {
  RequestRecord record;
  /// False for paths the request log keeps quiet, so polling cannot inflate
  /// per-client totals.
  bool tracked{true};
};

std::optional<ActiveRecord>& Current() {
  thread_local std::optional<ActiveRecord> record;
  return record;
}

}  // namespace

DashboardRecorder::ClientStats& DashboardRecorder::Track(
    const std::string& client) {
  const auto known = clients_.find(client);
  if (known != clients_.end()) {
    known->second.touched = ++touch_clock_;
    return known->second;
  }
  if (clients_.size() >= kMaxClients) {
    const auto retire = std::min_element(
        clients_.begin(), clients_.end(), [](const auto& lhs, const auto& rhs) {
          if (lhs.second.metrics.active != rhs.second.metrics.active) {
            return lhs.second.metrics.active < rhs.second.metrics.active;
          }
          return lhs.second.touched < rhs.second.touched;
        });
    // Active clients always win: with fewer active peers than the cap, an
    // idle entry is always available to retire.
    if (retire != clients_.end() && retire->second.metrics.active == 0) {
      clients_.erase(retire);
    }
  }
  const auto inserted = clients_.emplace(client, ClientStats{});
  inserted.first->second.metrics.client = inserted.first->first;
  inserted.first->second.touched = ++touch_clock_;
  return inserted.first->second;
}

void DashboardRecorder::ReleaseActive(const std::string& client, bool tracked) {
  active_.fetch_sub(1, std::memory_order_relaxed);
  if (!tracked) {
    return;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto entry = clients_.find(client);
  if (entry != clients_.end() && entry->second.metrics.active > 0) {
    entry->second.metrics.active -= 1;
    entry->second.touched = ++touch_clock_;
  }
}

void DashboardRecorder::begin(std::string id, std::string method,
                              std::string path, std::string client,
                              bool tracked) {
  auto& current = Current();
  if (current.has_value()) {
    // A connection thread that never reached finish() (process teardown races
    // only) must not leak its active slot into the next connection.
    ReleaseActive(current->record.client, current->tracked);
  }
  ActiveRecord entry;
  entry.tracked = tracked;
  entry.record.id = std::move(id);
  entry.record.method = std::move(method);
  entry.record.path = std::move(path);
  entry.record.client = std::move(client);
  entry.record.started_at = static_cast<long long>(std::time(nullptr));
  if (tracked) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ClientMetrics& totals = Track(entry.record.client).metrics;
    totals.requests += 1;
    totals.active += 1;
    totals.last_seen = entry.record.started_at;
    totals.last_method = entry.record.method;
    totals.last_path = entry.record.path;
  }
  current.emplace(std::move(entry));
  active_.fetch_add(1, std::memory_order_relaxed);
  total_.fetch_add(1, std::memory_order_relaxed);
}

void DashboardRecorder::note_generation(
    const TextGenerationBackend::Result& result) {
  auto& current = Current();
  if (!current.has_value()) {
    return;
  }
  RequestRecord& record = current->record;
  record.generation = true;
  record.prompt_tokens = result.prompt_tokens;
  record.completion_tokens = result.completion_tokens;
  record.cached_prompt_tokens = result.cached_prompt_tokens;
  record.ttft_ms = result.ttft_ms;
  record.queue_ms = result.queue_ms;
  record.prefill_ms = result.prefill_ms;
  record.decode_ms = result.decode_ms;
  record.prompt_tps = PrefillTokensPerSecond(result);
  record.gen_tps = result.decode_ms > 0.0
                       ? 1000.0 *
                             static_cast<double>(result.completion_tokens) /
                             result.decode_ms
                       : 0.0;
  record.finish =
      result.cancelled ? "cancelled"
      : result.finish_reason == TextGenerationBackend::FinishReason::kLength
          ? "length"
      : result.finish_reason ==
              TextGenerationBackend::FinishReason::kStopSequence
          ? "stop_sequence"
          : "stop";
  record.cache = result.cache_disk_hit ? "disk"
                 : result.cache_hit    ? "memory"
                                       : "miss";
  record.queue_depth = result.queue_depth_at_submit;
  record.plan = result.execution_plan;
  record.draft_proposed = result.draft_tokens;
  record.draft_accepted = result.draft_accepted_tokens;
  if (!current->tracked) {
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto client = clients_.find(record.client);
    if (client == clients_.end()) {
      return;
    }
    client->second.touched = ++touch_clock_;
    ClientMetrics& totals = client->second.metrics;
    totals.generations += 1;
    totals.prompt_tokens += result.prompt_tokens;
    totals.completion_tokens += result.completion_tokens;
    totals.cached_prompt_tokens += result.cached_prompt_tokens;
    totals.prefill_tokens += result.prefill_tokens;
    totals.draft_proposed += result.draft_tokens;
    totals.draft_accepted += result.draft_accepted_tokens;
    totals.prefill_ms += result.prefill_ms;
    totals.decode_ms += result.decode_ms;
  }
}

void DashboardRecorder::finish(int status, double duration_ms,
                               std::string_view outcome, bool stream,
                               bool record) {
  auto& current = Current();
  if (!current.has_value()) {
    return;
  }
  RequestRecord& entry = current->record;
  entry.status = status;
  entry.duration_ms = duration_ms;
  entry.outcome = std::string(outcome);
  entry.stream = stream;
  ReleaseActive(entry.client, current->tracked);
  if (record) {
    const std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(std::move(entry));
    while (records_.size() > kMaxRecords) {
      records_.pop_front();
    }
  }
  current.reset();
}

std::vector<ClientMetrics> DashboardRecorder::clients() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ClientMetrics> out;
  out.reserve(clients_.size());
  for (const auto& entry : clients_) {
    out.push_back(entry.second.metrics);
  }
  std::sort(out.begin(), out.end(),
            [](const ClientMetrics& lhs, const ClientMetrics& rhs) {
              if (lhs.last_seen != rhs.last_seen) {
                return lhs.last_seen > rhs.last_seen;
              }
              return lhs.requests > rhs.requests;
            });
  return out;
}

std::deque<RequestRecord> DashboardRecorder::list() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::deque<RequestRecord> out;
  for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
    out.push_back(*it);
  }
  return out;
}

DashboardRecorder& Recorder() {
  static DashboardRecorder recorder;
  return recorder;
}

}  // namespace gufo::server::dashboard
