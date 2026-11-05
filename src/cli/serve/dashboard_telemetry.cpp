#include "src/cli/serve/dashboard_telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <pdh.h>
#include <pdhmsg.h>  // PDH_MORE_DATA and PDH_CSTATUS_* are not in pdh.h
#include <unistd.h>  // gufo_resident_bytes via the compat/win32 shim
#include <windows.h>
#endif

#include "src/cli/serve/http_server.hpp"
#include "src/core/diagnostics/linux_sysfs.h"

#if defined(ENGINE_ENABLE_HIP)
#include "src/core/platform/device_memory.hpp"
#endif

namespace gufo::server::dashboard {
namespace {

constexpr std::size_t kThroughputWindowSamples = 5;
constexpr std::uint64_t kProbeEverySamples = 30;

double ClampPercent(double pct) {
  return std::clamp(pct, 0.0, 100.0);
}

#ifndef _WIN32
std::optional<double> SanePower(double watts) {
  if (!(watts >= 0.0 && watts <= 2000.0)) {
    return std::nullopt;
  }
  return watts;
}
#endif

const gufo::diagnostics::LinuxSysfs& Sys() {
  static const gufo::diagnostics::LinuxSysfs sysfs;
  return sysfs;
}

#ifndef _WIN32
std::optional<double> ParseDouble(std::string_view text) {
  const std::string value(text);
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || !std::isfinite(parsed)) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view text) {
  const auto value = ParseDouble(text);
  if (!value || *value < 0.0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(*value);
}

std::optional<std::uint64_t> ReadUnsignedFile(
    const gufo::diagnostics::LinuxSysfs& sysfs,
    const std::filesystem::path& path) {
  const auto raw = sysfs.ReadFile(path);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  return ParseUnsigned(*raw);
}

std::optional<double> ReadDoubleFile(const gufo::diagnostics::LinuxSysfs& sysfs,
                                     const std::filesystem::path& path) {
  const auto raw = sysfs.ReadFile(path);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  return ParseDouble(*raw);
}
#endif  // !_WIN32

std::optional<std::uint64_t> ReadProcessRss() {
#ifdef _WIN32
  const unsigned long long bytes = gufo_resident_bytes();
  if (bytes == 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(bytes);
#else
  std::ifstream input("/proc/self/status");
  for (std::string line; std::getline(input, line);) {
    if (!line.starts_with("VmRSS:")) {
      continue;
    }
    std::istringstream values(line.substr(6));
    std::uint64_t kib = 0;
    if (values >> kib) {
      return kib * 1024ULL;
    }
  }
  return std::nullopt;
#endif
}

#ifdef _WIN32
double FileTimeParts(const FILETIME& time) {
  ULARGE_INTEGER parts{};
  parts.LowPart = time.dwLowDateTime;
  parts.HighPart = time.dwHighDateTime;
  return static_cast<double>(parts.QuadPart);
}
#endif

}  // namespace

TelemetrySampler::TelemetrySampler(bool has_text_backend)
    : has_text_backend_(has_text_backend) {
#ifdef _WIN32
  platform_ = "windows";
#else
  platform_ = "linux";
#endif
}

TelemetrySampler::~TelemetrySampler() {
  stop();
}

void TelemetrySampler::start() {
  if (thread_.joinable()) {
    return;
  }
  thread_ =
      std::jthread([this](std::stop_token stop_token) { loop(stop_token); });
}

void TelemetrySampler::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
#ifdef _WIN32
  close_pdh();
#endif
}

void TelemetrySampler::loop(std::stop_token stop_token) {
  std::mutex wake_mutex;
  std::condition_variable_any wake;
  const std::stop_callback on_stop(stop_token, [&] { wake.notify_all(); });
  while (!stop_token.stop_requested()) {
    sample_once();
    wake.wait_for(wake_mutex, std::chrono::seconds(1),
                  [&] { return stop_token.stop_requested(); });
  }
}

void TelemetrySampler::probe_gpu() {
#ifndef _WIN32
  if (device_dir_.empty()) {
    for (const auto& device : Sys().QueryDrmGpuDevices()) {
      if (!device.driver.empty() && device.driver != "amdgpu") {
        continue;
      }
      device_dir_ =
          std::filesystem::path("/sys/class/drm") / device.card_name / "device";
      std::error_code ec;
      const auto hwmon_base = device_dir_ / "hwmon";
      for (const auto& entry :
           std::filesystem::directory_iterator(hwmon_base, ec)) {
        if (ec) {
          break;
        }
        if (Sys().ReadFile(entry.path() / "power1_average").has_value()) {
          hwmon_dir_ = entry.path();
          break;
        }
      }
      break;
    }
  }
  if (!gpu_name_.has_value() && !device_dir_.empty()) {
    if (const auto name = Sys().ReadFile(device_dir_ / "product_name");
        name && !name->empty()) {
      gpu_name_ = *name;
    }
  }
#endif
  if (!gpu_name_.has_value() && has_text_backend_) {
#if defined(ENGINE_ENABLE_HIP)
    // Same query `gufo diagnose` makes; only valid with a text backend so
    // the HIP runtime is already initialized.
    int device_count = 0;
    if (hipGetDeviceCount(&device_count) == hipSuccess && device_count > 0) {
      hipDeviceProp_t properties{};
      if (hipGetDeviceProperties(&properties, 0) == hipSuccess) {
        gpu_name_ = properties.name;
      }
    }
#endif
  }
}

void TelemetrySampler::sample_once() {
  try {
    const double sample_at =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const double epoch =
        std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    std::optional<double> cpu_busy;
    std::optional<double> cpu_total;
#ifdef _WIN32
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
      cpu_total = FileTimeParts(kernel) + FileTimeParts(user);
      cpu_busy = *cpu_total - FileTimeParts(idle);  // kernel includes idle
    }
#else
    if (const auto stat_text = Sys().ReadFile("/proc/stat")) {
      std::istringstream stream(*stat_text);
      std::string label;
      if (stream >> label && label == "cpu") {
        double values[10] = {};
        int count = 0;
        while (count < 10 && (stream >> values[count])) {
          ++count;
        }
        if (count >= 4) {
          double total = 0.0;
          for (int index = 0; index < count; ++index) {
            total += values[index];
          }
          cpu_busy = total - values[3] - (count > 4 ? values[4] : 0.0);
          cpu_total = total;
        }
      }
    }
#endif

    std::lock_guard<std::mutex> lock(mutex_);
    ++sample_count_;

    if (!statics_read_) {
      statics_read_ = true;
      if (const auto cpu = Sys().QueryCpuInfo()) {
        cpu_model_ = cpu->model_name;
        cpu_threads_ = cpu->logical_cores;
      }
    }
    if (sample_count_ == 1 ||
        (sample_count_ % kProbeEverySamples == 0 &&
         (!gpu_name_.has_value() || device_dir_.empty()))) {
      probe_gpu();
    }
    gpu_.name = gpu_name_;

    if (const auto mem = Sys().QueryMemInfo(); mem && mem->total_bytes > 0) {
      ram_total_ = mem->total_bytes;
      latest_.ram_used_bytes =
          mem->total_bytes - std::min(mem->available_bytes, mem->total_bytes);
    }
    process_rss_ = ReadProcessRss();

    std::optional<double> cpu_pct;
    if (cpu_busy && cpu_total && previous_cpu_total_ &&
        *cpu_total > *previous_cpu_total_) {
      cpu_pct = ClampPercent(100.0 * (*cpu_busy - *previous_cpu_busy_) /
                             (*cpu_total - *previous_cpu_total_));
    }
    if (cpu_busy && cpu_total) {
      previous_cpu_busy_ = *cpu_busy;
      previous_cpu_total_ = *cpu_total;
    }
    latest_.cpu_pct = cpu_pct;

#ifdef _WIN32
    gpu_.util_pct = sample_gpu_util();
#else
    if (!device_dir_.empty()) {
      if (const auto util =
              ReadDoubleFile(Sys(), device_dir_ / "gpu_busy_percent")) {
        gpu_.util_pct = ClampPercent(*util);
      } else {
        gpu_.util_pct = std::nullopt;
      }
      gpu_.vram_sysfs_used_bytes =
          ReadUnsignedFile(Sys(), device_dir_ / "mem_info_vram_used");
      gpu_.vram_sysfs_total_bytes =
          ReadUnsignedFile(Sys(), device_dir_ / "mem_info_vram_total");
      if (!hwmon_dir_.empty()) {
        if (const auto micro =
                ReadDoubleFile(Sys(), hwmon_dir_ / "power1_average")) {
          gpu_.power_w = SanePower(*micro / 1e6);
        } else {
          gpu_.power_w = std::nullopt;
        }
      }
    }
#endif

#if defined(ENGINE_ENABLE_HIP)
    if (has_text_backend_) {
      std::size_t free_bytes = 0;
      std::size_t total_bytes = 0;
      if (gufo::platform::DeviceMemoryInfo(&free_bytes, &total_bytes) ==
              hipSuccess &&
          total_bytes > 0) {
        gpu_.vram_used_bytes = total_bytes - std::min(free_bytes, total_bytes);
        gpu_.vram_total_bytes = total_bytes;
      }
    }
#endif

    const auto gen_tokens =
        detail::TotalGenTokens().load(std::memory_order_relaxed);
    const auto prompt_tokens =
        detail::TotalPromptTokens().load(std::memory_order_relaxed);
    std::optional<double> gen_rate;
    std::optional<double> prompt_rate;
    if (previous_sample_time_ && sample_at > *previous_sample_time_) {
      const double dt = sample_at - *previous_sample_time_;
      if (previous_gen_tokens_ && gen_tokens >= *previous_gen_tokens_) {
        gen_rate = static_cast<double>(gen_tokens - *previous_gen_tokens_) / dt;
      }
      if (previous_prompt_tokens_ &&
          prompt_tokens >= *previous_prompt_tokens_) {
        prompt_rate =
            static_cast<double>(prompt_tokens - *previous_prompt_tokens_) / dt;
      }
    }
    previous_gen_tokens_ = gen_tokens;
    previous_prompt_tokens_ = prompt_tokens;
    previous_sample_time_ = sample_at;
    if (gen_rate) {
      gen_window_.push_back(*gen_rate);
      while (gen_window_.size() > kThroughputWindowSamples) {
        gen_window_.pop_front();
      }
    }
    if (prompt_rate) {
      prompt_window_.push_back(*prompt_rate);
      while (prompt_window_.size() > kThroughputWindowSamples) {
        prompt_window_.pop_front();
      }
    }

    latest_.t = epoch;
    latest_.gpu_util_pct = gpu_.util_pct;
    latest_.vram_used_bytes = gpu_.vram_used_bytes;
    latest_.gen_tps = gen_rate;
    latest_.prompt_tps = prompt_rate;
    latest_.gpu_power_w = gpu_.power_w;
    history_.push_back(latest_);
    while (history_.size() > kHistoryCapacity) {
      history_.pop_front();
    }
  } catch (...) {
    // Telemetry must never take the server down: drop the sample and go on.
  }
}

TelemetrySnapshot TelemetrySampler::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  TelemetrySnapshot out;
  out.platform = platform_;
  out.cpu_model = cpu_model_;
  out.cpu_threads = cpu_threads_;
  out.uptime_s = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - started_)
          .count());
  out.cpu_pct = latest_.cpu_pct;
  out.ram_used_bytes = latest_.ram_used_bytes;
  out.ram_total_bytes = ram_total_;
  out.process_rss_bytes = process_rss_;
  out.gpu = gpu_;
  const auto mean = [](const std::deque<double>& window) {
    if (window.empty()) {
      return std::optional<double>();
    }
    double sum = 0.0;
    for (const double value : window) {
      sum += value;
    }
    return std::optional<double>(sum / static_cast<double>(window.size()));
  };
  out.gen_tps = mean(gen_window_);
  out.prompt_tps = mean(prompt_window_);
  out.history = history_;
  return out;
}

#ifdef _WIN32
std::optional<double> TelemetrySampler::sample_gpu_util() {
  if (pdh_broken_) {
    return std::nullopt;
  }
  if (pdh_query_ == nullptr) {
    PDH_HQUERY query{};
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
      pdh_broken_ = true;
      return std::nullopt;
    }
    pdh_query_ = query;
  }
  if (pdh_counter_ == nullptr) {
    PDH_HCOUNTER counter{};
    if (PdhAddEnglishCounterW(static_cast<PDH_HQUERY>(pdh_query_),
                              L"\\GPU Engine(*)\\Utilization Percentage", 0,
                              &counter) != ERROR_SUCCESS) {
      pdh_broken_ = true;
      return std::nullopt;
    }
    pdh_counter_ = counter;
    // The first collection only enumerates engine instances.
    (void)PdhCollectQueryData(static_cast<PDH_HQUERY>(pdh_query_));
    return std::nullopt;
  }
  if (PdhCollectQueryData(static_cast<PDH_HQUERY>(pdh_query_)) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  DWORD value_count = 0;
  DWORD buffer_size = 0;
  LONG status = PdhGetFormattedCounterArrayW(
      static_cast<PDH_HCOUNTER>(pdh_counter_), PDH_FMT_DOUBLE, &buffer_size,
      &value_count, nullptr);
  if (status != static_cast<LONG>(PDH_MORE_DATA) || buffer_size == 0) {
    return std::nullopt;  // PDH_NO_DATA while instances settle
  }
  std::vector<unsigned char> buffer(buffer_size);
  status = PdhGetFormattedCounterArrayW(
      static_cast<PDH_HCOUNTER>(pdh_counter_), PDH_FMT_DOUBLE, &buffer_size,
      &value_count,
      reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
  if (status != ERROR_SUCCESS) {
    return std::nullopt;
  }
  const auto* items =
      reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
  std::optional<double> max_util;
  for (DWORD index = 0; index < value_count; ++index) {
    if (items[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
        items[index].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
      continue;
    }
    const double util = items[index].FmtValue.doubleValue;
    if (!max_util || util > *max_util) {
      max_util = util;
    }
  }
  return max_util;
}

void TelemetrySampler::close_pdh() {
  if (pdh_query_ != nullptr) {
    PdhCloseQuery(static_cast<PDH_HQUERY>(pdh_query_));
    pdh_query_ = nullptr;
    pdh_counter_ = nullptr;
  }
}
#endif

}  // namespace gufo::server::dashboard
