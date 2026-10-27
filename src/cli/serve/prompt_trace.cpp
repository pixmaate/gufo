#include "src/cli/serve/prompt_trace.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace gufo::server {
namespace {

std::mutex g_trace_mutex;
std::FILE* g_trace_file = nullptr;
std::atomic<bool> g_trace_enabled{false};

}  // namespace

std::optional<std::string> PromptTrace::Open(const std::string& path) {
  const std::lock_guard lock(g_trace_mutex);
  if (g_trace_file != nullptr)
    return std::nullopt;
  g_trace_file = std::fopen(path.c_str(), "ab");
  if (g_trace_file == nullptr)
    return std::string(std::strerror(errno));
  g_trace_enabled.store(true, std::memory_order_release);
  return std::nullopt;
}

bool PromptTrace::Enabled() noexcept {
  return g_trace_enabled.load(std::memory_order_acquire);
}

void PromptTrace::Write(std::string_view json_line) {
  const std::lock_guard lock(g_trace_mutex);
  if (g_trace_file == nullptr)
    return;
  std::fwrite(json_line.data(), 1, json_line.size(), g_trace_file);
  std::fputc('\n', g_trace_file);
  std::fflush(g_trace_file);
}

}  // namespace gufo::server
