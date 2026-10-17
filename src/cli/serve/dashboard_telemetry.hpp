#ifndef GUFO_SERVER_DASHBOARD_TELEMETRY_HPP_
#define GUFO_SERVER_DASHBOARD_TELEMETRY_HPP_

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace gufo::server::dashboard {

inline constexpr std::size_t kHistoryCapacity = 60;

/// Unreadable readings are null on every platform, never guessed. On Windows
/// `power_w` and the `vram_sysfs_*` pair stay null (no public user-mode API);
/// `util_pct` there is the max across Pdh GPU Engine instances, which on an
/// iGPU driving the displays includes compositor activity.
struct GpuSnapshot {
  std::optional<std::string> name;
  std::optional<double> util_pct;
  std::optional<std::uint64_t> vram_used_bytes;
  std::optional<std::uint64_t> vram_total_bytes;
  std::optional<std::uint64_t> vram_sysfs_used_bytes;
  std::optional<std::uint64_t> vram_sysfs_total_bytes;
  std::optional<double> power_w;

  [[nodiscard]] bool any() const noexcept {
    return name.has_value() || util_pct.has_value() ||
           vram_used_bytes.has_value() || vram_total_bytes.has_value() ||
           vram_sysfs_used_bytes.has_value() ||
           vram_sysfs_total_bytes.has_value() || power_w.has_value();
  }
};

struct TelemetrySample {
  double t{0.0};
  std::optional<double> cpu_pct;
  std::optional<std::uint64_t> ram_used_bytes;
  std::optional<double> gpu_util_pct;
  std::optional<std::uint64_t> vram_used_bytes;
  std::optional<double> gen_tps;
  std::optional<double> prompt_tps;
  std::optional<double> gpu_power_w;
};

struct TelemetrySnapshot {
  std::string platform;  // "linux" or "windows"
  std::string cpu_model;
  std::uint32_t cpu_threads{0};
  std::uint64_t uptime_s{0};
  std::optional<double> cpu_pct;
  std::optional<std::uint64_t> ram_used_bytes;
  std::optional<std::uint64_t> ram_total_bytes;
  std::optional<std::uint64_t> process_rss_bytes;
  GpuSnapshot gpu;
  std::optional<double> gen_tps;     // rolling window
  std::optional<double> prompt_tps;  // rolling window
  std::deque<TelemetrySample> history;
};

/// 1 Hz system sampler owned by HttpServer. Every read is platform-branched
/// and exception-guarded: a failing sample yields nulls and the thread
/// continues. The Windows Pdh query lives and dies with the sampler.
class TelemetrySampler {
public:
  /// `has_text_backend` gates the HIP-runtime reads (VRAM, GPU name); the
  /// model is loaded before the server starts, so the runtime is up by then.
  explicit TelemetrySampler(bool has_text_backend);
  ~TelemetrySampler();

  TelemetrySampler(const TelemetrySampler&) = delete;
  TelemetrySampler& operator=(const TelemetrySampler&) = delete;
  TelemetrySampler(TelemetrySampler&&) = delete;
  TelemetrySampler& operator=(TelemetrySampler&&) = delete;

  void start();
  void stop();

  [[nodiscard]] TelemetrySnapshot snapshot() const;

private:
  void loop(std::stop_token stop_token);
  void sample_once();
  /// Discovers (or re-discovers) the GPU device paths and static name.
  void probe_gpu();

  bool has_text_backend_;
  std::chrono::steady_clock::time_point started_{
      std::chrono::steady_clock::now()};
  std::jthread thread_;

  mutable std::mutex mutex_;
  // Static reads, filled once.
  std::string platform_;
  std::string cpu_model_;
  std::uint32_t cpu_threads_{0};
  bool statics_read_{false};
  std::optional<std::string> gpu_name_;
  std::filesystem::path device_dir_;
  std::filesystem::path hwmon_dir_;
  GpuSnapshot gpu_;
  std::optional<std::uint64_t> ram_total_;
  std::optional<std::uint64_t> process_rss_;
  // Rolling state.
  std::optional<double> previous_cpu_busy_;
  std::optional<double> previous_cpu_total_;
  std::optional<std::uint64_t> previous_gen_tokens_;
  std::optional<std::uint64_t> previous_prompt_tokens_;
  std::optional<double> previous_sample_time_;
  TelemetrySample latest_;
  std::deque<TelemetrySample> history_;
  std::deque<double> gen_window_;
  std::deque<double> prompt_window_;
  std::uint64_t sample_count_{0};
#ifdef _WIN32
  void* pdh_query_{nullptr};
  void* pdh_counter_{nullptr};
  bool pdh_broken_{false};

  std::optional<double> sample_gpu_util();
  void close_pdh();
#endif
};

}  // namespace gufo::server::dashboard

#endif  // GUFO_SERVER_DASHBOARD_TELEMETRY_HPP_
