#ifndef GUFO_SERVER_PROMPT_TRACE_HPP_
#define GUFO_SERVER_PROMPT_TRACE_HPP_

#include <optional>
#include <string>
#include <string_view>

namespace gufo::server {

/// Opt-in content trace (`gufo serve llm --trace PATH`): one JSON object per
/// line for every chat request, holding the exact prompt the model reads
/// (special tokens spelled out). Off unless opened; the file holds client
/// content, so it is never enabled by default.
class PromptTrace {
public:
  /// Opens PATH for appending. Returns an error message on failure.
  [[nodiscard]] static std::optional<std::string> Open(const std::string& path);
  [[nodiscard]] static bool Enabled() noexcept;
  /// Appends one line (a complete JSON object without a newline) and flushes.
  static void Write(std::string_view json_line);
};

}  // namespace gufo::server

#endif  // GUFO_SERVER_PROMPT_TRACE_HPP_
