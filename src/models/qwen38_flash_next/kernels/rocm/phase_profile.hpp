#ifndef GUFO_MODELS_QWEN38_FLASH_NEXT_KERNELS_ROCM_PHASE_PROFILE_HPP_
#define GUFO_MODELS_QWEN38_FLASH_NEXT_KERNELS_ROCM_PHASE_PROFILE_HPP_

// GUFO_PHASE_PROFILE=1 (EXPERIMENT, diagnostics only): per-phase GPU time
// inside the real verify body graphs. Every Mark records a one-thread kernel
// that writes the GPU wall clock into coherent pinned memory; it is captured
// into the graph, so every replay stamps its phase boundaries. After each
// verify the stamps are folded per phase and width; a report every ~10 s
// gives ms per pass per phase. Each stamp costs ~0.8 us of GPU time, charged
// to the phase it ends.

#include <hip/hip_runtime.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "src/models/qwen38_flash_next/kernels/rocm/kernels.hpp"

namespace gufo::models::qwen38_flash_next::rocm {

class PhaseStamps {
public:
  enum Phase : int {
    kSetup,
    kPle,
    kMixNorm,    // F32 grouped norm (unnormed mixers only)
    kMixDown,    // quantize + down GEMV (+ fused SiLU)
    kMixUp,      // quantize + up GEMV
    kMixEpi,     // epilogue (mixed + inject)
    kGdnIn,      // quantize + qkv + gate GEMVs
    kGdnAb,      // alpha/beta F16 GEMV
    kGdnCore,    // conv, prep, recurrence, norm/gate
    kGdnOut,     // quantize + output GEMV
    kAttnProj,   // qkv GEMV + norm/rope/store
    kAttnCore,   // indexer, attention, sigmoid gate
    kAttnOut,    // output projection
    kCombine,
    kRouter,     // F32 router GEMV + top-k
    kShared,     // shared expert gate/up + down
    kRouted,     // routed gate/up (quantize + gated GEMV)
    kHeadMix,
    kHead,       // LM head
    kCand,       // candidate selection + copies
    kRoutedDown,  // routed down (quantize + GEMV)
    kRoutedEpi,   // MoE epilogue
    kMtpIn,       // draft step: embedding, norms, fc projections
    kCount
  };
  static PhaseStamps& Get() {
    static PhaseStamps stamps;
    return stamps;
  }
  [[nodiscard]] bool Enabled() const noexcept { return enabled_; }
  void BeginBody(std::uint64_t key, hipStream_t stream) {
    if (!enabled_) {
      return;
    }
    auto& block = blocks_[key];
    if (block.slots == nullptr) {
      void* slots = nullptr;
      if (hipHostMalloc(&slots, kSlots * sizeof(std::uint64_t),
                        hipHostMallocCoherent) != hipSuccess) {
        current_ = nullptr;
        return;
      }
      block.slots = static_cast<std::uint64_t*>(slots);
    }
    block.phases.clear();
    current_ = &block;
    Mark(stream, kSetup);
  }
  void EndBody(hipStream_t stream) {
    Mark(stream, kCount);
    current_ = nullptr;
  }
  void Mark(hipStream_t stream, int phase) {
    if (current_ == nullptr || current_->phases.size() >= kSlots) {
      return;
    }
    GpuStamp(current_->slots + current_->phases.size(), stream);
    current_->phases.push_back(phase);
  }
  /// The body of `key` (width `w`) completed.
  void Collect(std::uint64_t key, std::uint32_t w, bool draft = false) {
    if (draft) {
      w = 0;  // draft steps report in slot 0
    } else if (!enabled_ || w == 0 || w > 8) {
      return;
    }
    const auto it = blocks_.find(key);
    if (!enabled_ || it == blocks_.end() || it->second.phases.size() < 2) {
      return;
    }
    const Block& b = it->second;
    auto& s = stats_[w];
    for (std::size_t i = 0; i + 1 < b.phases.size(); ++i) {
      const std::uint64_t a = b.slots[i], z = b.slots[i + 1];
      if (z > a) {
        s.ms[b.phases[i]] += static_cast<double>(z - a) * ns_per_tick_ * 1e-6;
      }
    }
    ++s.passes;
    s.stamps = b.phases.size();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - since_).count() >= 10.0) {
      since_ = now;
      Report();
    }
  }

private:
  static constexpr std::size_t kSlots = 4096;
  struct Block {
    std::uint64_t* slots{nullptr};
    std::vector<int> phases;
  };
  struct Stats {
    std::array<double, kCount + 1> ms{};
    std::uint64_t passes{0};
    std::uint64_t stamps{0};
  };
  PhaseStamps() {
    const char* v = std::getenv("GUFO_PHASE_PROFILE");
    enabled_ = v != nullptr && v[0] != '\0' && v[0] != '0';
    int device = 0, khz = 0;
    if (enabled_ && hipGetDevice(&device) == hipSuccess &&
        hipDeviceGetAttribute(&khz, hipDeviceAttributeWallClockRate,
                              device) == hipSuccess &&
        khz > 0) {
      ns_per_tick_ = 1e6 / khz;
    }
    since_ = std::chrono::steady_clock::now();
  }
  void Report() {
    static constexpr const char* kNames[kCount] = {
        "setup",     "ple",       "mix-norm", "mix-down", "mix-up",
        "mix-epi",   "gdn-in",    "gdn-ab",   "gdn-core", "gdn-out",
        "attn-proj", "attn-core", "attn-out", "combine",  "router",
        "shared",    "routed",    "head-mix", "head",     "cand",
        "r-down",    "r-epi",     "mtp-in"};
    for (std::uint32_t w = 0; w <= 8; ++w) {
      Stats& s = stats_[w];
      if (s.passes == 0) {
        continue;
      }
      double total = 0;
      for (int p = 0; p < kCount; ++p) {
        total += s.ms[p];
      }
      const double per = 1.0 / static_cast<double>(s.passes);
      std::string line;
      char item[96];
      for (int p = 0; p < kCount; ++p) {
        if (s.ms[p] <= 0) {
          continue;
        }
        std::snprintf(item, sizeof(item), " %s %.3f", kNames[p], s.ms[p] * per);
        line += item;
      }
      std::fprintf(stderr,
                   "qwen38_flash_next: phases %s%u: %llu passes, %.2f ms/pass "
                   "(%llu stamps):%s\n",
                   w == 0 ? "draft" : "w", w,
                   static_cast<unsigned long long>(s.passes), total * per,
                   static_cast<unsigned long long>(s.stamps), line.c_str());
      s = Stats{};
    }
  }
  bool enabled_{false};
  double ns_per_tick_{10.0};
  std::map<std::uint64_t, Block> blocks_;
  Block* current_{nullptr};
  std::array<Stats, 9> stats_{};
  std::chrono::steady_clock::time_point since_;
};

inline PhaseStamps& Ps() { return PhaseStamps::Get(); }

}  // namespace gufo::models::qwen38_flash_next::rocm

#endif  // GUFO_MODELS_QWEN38_FLASH_NEXT_KERNELS_ROCM_PHASE_PROFILE_HPP_
