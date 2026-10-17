# Gufo `/dashboard` — live monitoring, chat, and model configuration UI

## Goal

Add a self-contained dashboard page served by the running `gufo serve` process at `GET /dashboard`, with supporting JSON endpoints on the same server/port. The page has three tabs:

1. **Overview** — CPU %, RAM, GPU util %, VRAM used/total, token throughput (tok/s), model state, active requests, 60-second sparkline history, and a recent-requests list with per-request timing detail.
2. **Chat** — talk directly to the loaded model by streaming `POST /v1/chat/completions` (SSE) from the page.
3. **Models** — the model(s) this process serves (`/v1/models`) plus the loaded model's configuration (context, sessions, speculative backend, sampling defaults, reasoning defaults).

**Platform requirement: the dashboard must work fully on Strix Halo (gfx1151, UMA APU) on both Linux (production) and Windows (ROCm/TheRock dev builds).** Gufo is a dual-target codebase (CMakeLists.txt:3-5; `compat/win32` POSIX shim; Windows CI via the `cpu-test`/hosted presets), and the dashboard follows the same `#ifdef _WIN32` discipline gufo already applies: every reading has a Linux path and a Windows path, and anything a platform cannot read is `null` — never a guess, never a crash.

## Design constraints (gufo reality, verified in code)

- **No runtime model load/unload** exists (`docs/SERVER.md:645`) — the dashboard has no load/unload controls.
- **One model per process**, loaded at startup (`src/cli/serve/serve.cpp:1167`); "available models" = what this process serves: text + any of video/tts/asr/image services (`HttpServer` holds up to five, `http_server.hpp:94-100`).
- **Metadata-only request records** (user decision), matching the documented telemetry policy at `docs/SERVER.md:255-257`: "limited to status, token counts, timing, and cancellation state… must not contain prompt text, model paths, machine identity, request IDs, or token IDs."
  - The `rN` request id stays — already exposed to the client via the existing `X-Request-ID` header (`http_server.cpp:1505`).
  - `client` (peer IP) is kept: transport-derived via `getpeername` (`http_server.cpp:1361-1364`), never read from client headers so it cannot be spoofed; a serving dashboard needs to show which client issued a request. It is not the server host's identity.
  - `gpu.name` (device string) is kept: same auth gate as every route, needed to label the VRAM tile. No hostname, MAC, or other server-host identifier is ever read or sent.

### Per-platform data availability (Strix Halo)

| Data point | Linux | Windows |
|---|---|---|
| CPU model + thread counts | `LinuxSysfs::QueryCpuInfo()` from `/proc/cpuinfo` | **same call** — built-in `WindowsCpuInfo()` branch (CPUID brand string + `sysconf`, linux_sysfs.cpp:75-112) |
| CPU utilization % | `/proc/stat` aggregate-line delta between 1 Hz samples | `GetSystemTimes` (kernel32) delta |
| Host RAM used/total | `LinuxSysfs::QueryMemInfo()` from `/proc/meminfo` | **same call** — psapi helpers from the `compat/win32` shim (linux_sysfs.cpp:158-164, gufo_posix.h:321-324) |
| Process RSS | `/proc/self/status` VmRSS (same read as `Logger::MemoryStatus`, logging.cpp:91-103) | `gufo_resident_bytes()` (compat shim, same read as `Logger::MemoryStatus`) |
| VRAM used/total | `gufo::platform::DeviceMemoryInfo` = `hipMemGetInfo` (framebuffer = BIOS carve-out) | **same call** — `DeviceMemoryInfo` switches to DXGI `QueryVideoMemoryInfo` WDDM budget, the number gufo's loader already sizes from (device_memory.hpp, device_memory.cpp) |
| GPU busy % | amdgpu sysfs `gpu_busy_percent` via `LinuxSysfs::ReadFile` | **New** — Win32 Pdh counter `\GPU Engine(*)\Utilization Percentage` (`pdh.h`/`pdh.lib`, see below) |
| GPU edge temperature | amdgpu hwmon `temp1_input` (same convention `QueryDrmGpuDevices` already walks, linux_sysfs.cpp:244-261) | `null` — not exposed by public Win32 APIs without a vendor DLL |
| GPU power draw | amdgpu hwmon `power1_average` (µW) | `null` — same reason |
| GPU name | sysfs `product_name` (same as `gufo diagnose`) | `hipGetDeviceProperties().name` behind `ENGINE_ENABLE_HIP` — exactly what `gufo diagnose` does (system_inventory.cpp:97-108) |
| VRAM sysfs cross-check | `mem_info_vram_used/_total` via `LinuxSysfs::ReadFile` (useful on UMA where it differs from the framebuffer) | `null` (fields present, null-valued) |
| Everything else (model state, request records, tok/s series, chat, models tab) | platform-independent | platform-independent |

Windows GPU-busy note: on Strix Halo the iGPU also drives the displays, so the Pdh engine counters include display-engine activity; the UI shows the max across engines with a footnote in the tooltip. That is the honest number without a vendor SDK (ADL/`atiadlxx.dll` is out: not a system library, unstable ABI, not vendored by gufo).

## Reuse map — every dashboard data point vs. existing gufo code

The dashboard is built by **exposing what the server already collects**, plus a small set of new readings taken through gufo's own existing readers and system-library conventions. Nothing is measured twice where gufo already measures it.

| Dashboard data point | Source (existing gufo code) | Status |
|---|---|---|
| Model id, ready state | `TextGenerationBackend::model_id()` / `ready()` — the same virtuals `/ready` and `/v1/models` use (`http_server.cpp:1282, 584`) | Reuse |
| Sampling defaults | `TextGenerationBackend::sampling_defaults()` (text_generation_backend.hpp:214) | Reuse |
| Reasoning defaults | `TextGenerationBackend::reasoning_defaults()` (text_generation_backend.hpp:217) | Reuse |
| Context / sessions / speculative backend | `InferenceBackend::max_context()` (inference_backend.hpp:124) + new `model_info()` virtual (only new backend surface; sessions/speculative are already passed into `load()`, serve.cpp:1167-1192, just not stored for readback) | Reuse + tiny addition |
| Total prompt / generated tokens | `detail::TotalPromptTokens()` / `TotalGenTokens()` atomics (http_server.hpp:146-153) — already incremented by `RecordServerMetrics` on every generation (http_server.hpp:164) | Reuse |
| Last prompt/decode speed (tok/s) | `detail::LastPromptSpeed()` / `LastGenSpeed()` atomics (http_server.hpp:154-161) — the same values `/metrics` exposes as `llamacpp:*_tokens_seconds` (http_server.cpp:917-944) | Reuse |
| Per-request tokens + timings | `TextGenerationBackend::Result` (text_generation_backend.hpp:134-188) — the same struct already serialized into response `timings` (`GenerationTimings`), `usage.gufo` (`UsageJson`, http_server.cpp:348-370), `Server-Timing` header (http_server.cpp:372-384), and log lines (`GenerationLogDetails`, generation_metrics.hpp:19-65). Dashboard record mirrors those field names exactly. | Reuse |
| Per-request method/path/status/duration/outcome/client | The exact values `handle_connection` already assembles for `Logger::LogRequest` (http_server.cpp:1557-1560, logging.cpp:105-121). Recording uses the **same predicate** the server already uses for logging (`log_request \|\| status >= 400 \|\| !connected`, http_server.cpp:1557) — the dashboard list is the in-memory twin of the request log, and quiet polling paths (`/health`, `/metrics`, `/v1/models`, …, http_server.cpp:1481-1484) stay out automatically. | Reuse |
| Host RAM used/total (both OSes) | `gufo::diagnostics::LinuxSysfs::QueryMemInfo()` (linux_sysfs.h:55) — already cross-platform: `/proc/meminfo` on Linux, `gufo_total_physical_bytes()`/`gufo_available_physical_bytes()` (compat shim) on Windows | Reuse |
| CPU model + threads (both OSes) | `LinuxSysfs::QueryCpuInfo()` (linux_sysfs.h:54) — already cross-platform | Reuse |
| Process RSS (both OSes) | Same read `Logger::MemoryStatus()` performs per request (logging.cpp:91-103), 4 lines in the sampler | Reuse (pattern) |
| VRAM free/total (both OSes) | `gufo::platform::DeviceMemoryInfo` (device_memory.hpp) — `hipMemGetInfo` on Linux, DXGI WDDM budget on Windows; the identical call `ModelLoadLog` makes at load time (serve.cpp:72-78). Polled live only when a text backend exists (model loads before the server starts, so the HIP runtime is up). | Reuse |
| GPU identity + edge temperature | Linux: `LinuxSysfs::QueryDrmGpuDevices()` (linux_sysfs.h:56) — discovers `card*` devices, parses `uevent` driver/PCI, amdgpu hwmon `temp1_input` → `hwmon_sensors["edge_temperature_c"]` (linux_sysfs.cpp:197-267), `product_name` for the name. Windows: `hipGetDeviceProperties` (system_inventory.cpp:97-108, `ENGINE_ENABLE_HIP`-guarded, backend-gated). | Reuse |
| JSON encoding | `gufo::json::Value` (src/core/json.hpp) — the serializer every endpoint uses (`Ok()`, http_server.cpp:344-346) | Reuse |
| Auth | Existing Bearer gate `IsAuthorized` (http_server.cpp:1253, 252-278) — dashboard JSON routes sit behind it like every route; page HTML is the single unauthenticated route (contains no secrets; browsers can't send Bearer on initial load). | Reuse |
| Request id convention | Existing `rN` counter + `X-Request-ID` (http_server.cpp:1357-1365, 1505) | Reuse |

### New readings (collected the gufo way)

Gufo has no existing reader for these; they are added in `dashboard_telemetry.cpp` using the **same standards** the codebase applies to system data — `gufo::diagnostics::LinuxSysfs::ReadFile()` (public sysfs/proc reader, linux_sysfs.h:58), the same `/sys/class/drm/card*/device/` layout `QueryDrmGpuDevices()` walks, `#ifdef _WIN32` branches, `std::optional`/null on any failure, nothing may throw into the server:

| New data point | Linux | Windows |
|---|---|---|
| CPU utilization % | Delta of the aggregate `/proc/stat` line between 1 Hz samples (same /proc convention as `QueryMemInfo`) | Delta of `GetSystemTimes` (kernel32) — same family of system calls gufo's shim already uses |
| GPU busy % | `LinuxSysfs::ReadFile("<card>/device/gpu_busy_percent")` — the key `gufo diagnose` does not read today, so it is read in the sampler, **not** added to `QueryDrmGpuDevices()` (that function's output is a contract for the diagnose report) | Pdh (Performance Data Helper) API: open one query with counter `\GPU Engine(*)\Utilization Percentage`, collect each second, take the max across engine instances. Pure Win32 system API (`pdh.h`, link `pdh`) — same "OS system library" category as the `ws2_32`/`psapi`/`dxgi` gufo already links. Query created once in the sampler, closed in `stop()`; any Pdh failure → null. |
| GPU power draw | `LinuxSysfs::ReadFile("<card>/device/hwmon/*/power1_average")` (µW); null when absent | `null` (no public API without vendor DLL) |
| VRAM via sysfs (UMA cross-check) | `LinuxSysfs::ReadFile("<card>/device/mem_info_vram_used")` / `mem_info_vram_total`; `DeviceMemoryInfo` remains the primary VRAM source | `null` (fields present, null-valued) |
| Rolling throughput (gen/prompt tok/s) | Sampler computes Δ`TotalGenTokens()` per 1 s interval (reusing the existing counter); prompt tps from `detail::LastPromptSpeed()` | same |

**Build dependencies:**
- `gufo_http` currently links `gufo_logging gufo_core gufo_qwen3_tts gufo_qwen3_asr gufo_qwen_image_21` (CMakeLists.txt:232) but not `gufo_diagnostics` (CMakeLists.txt:108-119, defined earlier, no cycle). Add `gufo_diagnostics`; only `linux_sysfs.o` is pulled in (its Windows branch already depends on the globally linked `gufo_posix_compat` shim, CMakeLists.txt:30-34 — proven by `gufo_diagnostics` compiling on Windows today).
- Windows: add `pdh` to `gufo_http`'s link libraries, following the existing per-target system-lib pattern `if (WIN32) target_link_libraries(gufo_core PRIVATE dxgi)` (CMakeLists.txt:193-196).

## Architecture

All new server code lives in `gufo_http` (`src/cli/serve/`). The page is embedded in the binary at build time (no static-file route, no install-path handling, no traversal risk).

```
Browser ──GET /dashboard──────────────────────► dashboard.html (embedded bytes, no auth)
         ──GET /dashboard/status (2 s poll)──► Telemetry snapshot + server/model state (bearer-gated)
         ──GET /dashboard/requests (2 s poll)► RequestRecorder ring buffer (bearer-gated)
         ──POST /v1/chat/completions (SSE)───► existing OpenAI chat handler (no change)
         ──GET /v1/models────────────────────► existing ListModels (no change)

handle_connection (per-connection thread)
  begin() ─► DashboardRecorder (thread-local current record)
  handler ─► WithTiming(result) ─► Recorder::note_generation() (same thread)
  finish() ─► status/duration/outcome  (predicate: same as request logging)

TelemetrySampler (1 Hz std::jthread, member of HttpServer; both OSes)
  RAM + CPU model/threads: LinuxSysfs::QueryMemInfo / QueryCpuInfo (cross-platform)
  VRAM: gufo::platform::DeviceMemoryInfo (ENGINE_ENABLE_HIP, backend present;
        hipMemGetInfo on Linux, DXGI WDDM budget on Windows)
  GPU name: sysfs product_name (Linux) / hipGetDeviceProperties (Windows)
  GPU busy: /sys amdgpu gpu_busy_percent (Linux) / Pdh GPU Engine counter (Windows)
  GPU temp/power: amdgpu hwmon (Linux) / null (Windows)
  CPU %: /proc/stat delta (Linux) / GetSystemTimes (Windows)
  RSS: same read as Logger::MemoryStatus (both OSes)
  gen_tps: Δ detail::TotalGenTokens() per interval
  keeps last 60 samples per series under a mutex
```

- **Thread-local linkage** (mirrors the existing function-local-static pattern of the `detail::` atomics, http_server.hpp:146-162): `DashboardRecorder::begin(req)` sets the thread-local current record; `WithTiming` (called on the same connection thread, for both streamed and non-streamed completions) attaches the `Result` fields; `finish()` closes the record. No new threading beyond the sampler.
- **Sampler lifetime**: constructed with `HttpServer` (works for all modality servers — llm/video/tts/asr/image), stopped in `stop()`/destructor; VRAM + GPU-name polling gated on `backend_ != nullptr` (HIP runtime initialized by then); the Windows Pdh query is created/destroyed with the sampler.
- **Exception safety**: every sampler read is `std::optional`; a throwing/failing sample yields nulls and the thread continues — telemetry must never take the server down (same bar as the existing `/proc` reads in `Logger::MemoryStatus`).

### Auth model

- `GET /dashboard` served **before** the `IsAuthorized` gate (no secrets in the page).
- `GET /dashboard/status` and `GET /dashboard/requests` served **after** it, inheriting `--api-key` protection.
- The page stores an optional API key in `sessionStorage`; on any 401 it reveals a key input. With no `--api-key` set (default 127.0.0.1 bind) everything works without one.

## API contracts

### `GET /dashboard` → 200 `text/html; charset=utf-8`

Embedded `src/cli/serve/web/dashboard.html` (single file, inline CSS + JS, no external assets, no CDNs, vanilla JS only).

### `GET /dashboard/status` → 200 JSON

```json
{
  "server": {
    "model": "<backend->model_id() or null>",
    "ready": true,
    "context": 32768, "sessions": 2, "speculative": "off",
    "sampling": {"max_tokens": 0, "temperature": 0, "top_k": 0, "top_p": 1, "min_p": 0,
                 "min_keep": 0, "seed": -1, "repeat_penalty": 1, "repeat_last_n": 64},
    "reasoning": {"enabled": null, "effort": null, "preserve_thinking": null},
    "services": {"text": true, "video": false, "image": false, "tts": false, "asr": false},
    "uptime_s": 1234, "active_requests": 1, "total_requests": 42,
    "total_prompt_tokens": 1000, "total_generated_tokens": 500
  },
  "hardware": {
    "platform": "linux",
    "cpu": {"model": "AMD Ryzen AI Max+ 395", "logical_threads": 16, "pct": 12.3},
    "ram_used_bytes": 8589934592, "ram_total_bytes": 128849018880,
    "process_rss_bytes": 2147483648,
    "gpu": {"name": "AMD Radeon 890M", "util_pct": 45,
            "vram_used_bytes": 17179869184, "vram_total_bytes": 68719476736,
            "vram_sysfs_used_bytes": 25769803776, "vram_sysfs_total_bytes": 68719476736,
            "temp_c": 52.0, "power_w": 18.2}
  },
  "throughput": {"gen_tps": 24.5, "prompt_tps": 310.0,
                 "last_prompt_tps": 310.0, "last_decode_tps": 24.5},
  "history": {"t": [ ...60 epoch-s... ], "cpu": [...], "ram_used": [...],
              "gpu_util": [...], "vram_used": [...], "gen_tps": [...]}
}
```

- `hardware.platform` is `"linux"` or `"windows"` — the UI uses it to footnote Windows specifics (GPU util includes display-engine activity; temp/power not available).
- `hardware.gpu` is `null` when no GPU readings are available (non-llm server, unsupported host); individual unreadable fields are `null`, never a guess. On Windows, `temp_c`, `power_w`, and `vram_sysfs_*` are always `null`; on Linux they are populated when amdgpu exposes them.
- `throughput.last_prompt_tps` / `last_decode_tps` are the existing `detail::LastPromptSpeed()` / `LastGenSpeed()` atomics; `gen_tps`/`prompt_tps` are the sampler's rolling window.
- `server.context/sessions/speculative` from the new `model_info()` virtual; `sampling`/`reasoning` from existing virtuals.

### `GET /dashboard/requests` → 200 JSON

```json
{"requests": [
  {"id": "r12", "method": "POST", "path": "/v1/chat/completions",
   "client": "127.0.0.1", "started_at": 1760000000, "duration_ms": 1234.5,
   "status": 200, "outcome": "completed", "stream": true,
   "prompt_tokens": 100, "completion_tokens": 42, "cached_prompt_tokens": 0,
   "ttft_ms": 180.2, "queue_ms": 0.0, "prefill_ms": 150.1, "decode_ms": 980.3,
   "prompt_tps": 310.0, "gen_tps": 42.8, "finish": "stop",
   "cache": "memory", "queue_depth": 0, "plan": "serial-c1",
   "draft_proposed": 0, "draft_accepted": 0}
]}
```

- Field names match `GenerationLogDetails`/`GenerationTimings` (generation_metrics.hpp:19-99) so the dashboard, the API `timings`, and the server log describe a request identically. Text-generation fields are `null` for non-text endpoints; `finish` maps `Result::finish_reason`/`cancelled` exactly as `GenerationLogDetails` does (generation_metrics.hpp:26-32).
- Newest first, capped at 100 (bounded memory). Recorded only when the existing request-log predicate holds (`log_request || status >= 400 || !connected`, http_server.cpp:1557) — dashboard/health/metrics/models polling never appears.

## Page design (original markup/CSS/JS)

- Dark theme default + light toggle (persisted in `localStorage`); no frameworks, no build step, no external fonts/assets.
- Header: `Gufo` brand, model state dot (loaded/active), `API key` button (revealed on 401).
- Tab bar: **Overview · Chat · Models**.
- **Overview**: stat cards (model + ready, active requests, rolling gen tok/s, last request duration); sparkline charts drawn on `<canvas>` (CPU %, RAM %, GPU %, VRAM used, gen tok/s, prompt tok/s) fed from `history`; requests table (time, `METHOD path`, client, status, prompt/completion tokens, TTFT, duration, tok/s) with search box + status filter; row click expands a timing breakdown row (queue / prefill / decode / cached tokens / cache kind / finish / plan / draft acceptance).
- **Chat**: conversation list, system-prompt field, `temperature` and `max_tokens` inputs, model label from `/v1/models` (tab disabled with an explanation when no text backend is present). Sends the full message array to `POST /v1/chat/completions` with `stream: true`, parses SSE deltas with `fetch` + `ReadableStream` (server already emits chunked SSE, http_server.cpp:1515-1534), renders tokens as they arrive; **Stop** button via `AbortController`; conversation kept in page memory only.
- **Models**: table from `/v1/models` (id, capability, owned_by) + config panel from `/dashboard/status.server` (context, sessions, speculative, sampling defaults, reasoning defaults, services present).
- **Platform-awareness**: reads `hardware.platform`; on `windows` shows `—` for temp/power/sysfs-VRAM with a tooltip explaining availability, and footnotes the GPU util tile ("includes display engine activity"). Null fields render as `—` everywhere, so the same page works on both OSes without feature flags.
- Polling: `/dashboard/status` and `/dashboard/requests` every 2 s with `cache: no-store`; page shows `Disconnected · retrying` on fetch failure (server down / bad key).

## Files to add

| File | Purpose |
|---|---|
| `src/cli/serve/dashboard_telemetry.hpp` / `.cpp` | `TelemetrySampler`: 1 Hz `std::jthread`, 60-sample ring buffers (std::deque) under `std::mutex`; `Snapshot()` → current + history + static + `platform`. Uses `gufo::diagnostics::LinuxSysfs` (`QueryMemInfo`, `QueryCpuInfo`, `QueryDrmGpuDevices`, `ReadFile`), `gufo::platform::DeviceMemoryInfo` behind `#if defined(ENGINE_ENABLE_HIP)`, `hipGetDeviceProperties` for the GPU name on Windows (same guard/gate as system_inventory.cpp:97-108), `/proc/stat` delta (Linux) / `GetSystemTimes` (Windows) for CPU %, Pdh GPU Engine query (Windows only, `pdh.h`), RSS read as in `Logger::MemoryStatus`, Δ`TotalGenTokens()` for rolling tps. Fully exception-guarded; all readers platform-branched with null fallbacks. |
| `src/cli/serve/dashboard_recorder.hpp` / `.cpp` | `DashboardRecorder`: ring buffer (std::deque, max 100) of `RequestRecord` (fields per the contract above); `begin(req)` / `finish(status, duration_ms, outcome)` / `note_generation(const TextGenerationBackend::Result&)` / `list()` / `active_count()`. Accessor `dashboard::Recorder()` — function-local static singleton, same pattern as `detail::TotalPromptTokens()` (http_server.hpp:146). Thread-local current-record pointer set in `begin()` so `note_generation()` from `WithTiming` (same connection thread) finds it. Mutex-protected. |
| `src/cli/serve/web/dashboard.html` | The full page (single self-contained file, ~60-100 KB). |

## Files to modify

1. **`CMakeLists.txt`**
   - `gufo_http` source list (lines 218-231): add `src/cli/serve/dashboard_telemetry.cpp`, `src/cli/serve/dashboard_recorder.cpp`; link line 232: add `gufo_diagnostics`; add `if (WIN32) target_link_libraries(gufo_http PRIVATE pdh) endif()` (pattern of CMakeLists.txt:193-196).
   - Embed step (pure CMake, near the `gufo_http` definition):
     ```cmake
     file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/serve/web/dashboard.html GUFO_DASHBOARD_HTML HEX)
     string(REGEX REPLACE "([0-9a-f]{2})" "0x\\1," GUFO_DASHBOARD_BYTES "${GUFO_DASHBOARD_HTML}")
     file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/gufo_dashboard_html.hpp
          CONTENT "namespace gufo::server { inline constexpr unsigned char kDashboardHtml[] = {${GUFO_DASHBOARD_BYTES}0x00}; }")
     ```
     plus `${CMAKE_CURRENT_BINARY_DIR}/generated` in `gufo_http`'s include directories. (Fallback if `file(GENERATE)` content is awkward: a ~15-line `cmake/embed_file.cmake` via `add_custom_command`.)
   - Tests (after the existing `http_server_test` block, lines 410-413): `dashboard_test` (new `tests/cli/dashboard_test.cpp`, links `gufo_http`, labels `cpu;server;http`, TIMEOUT 30) and `dashboard_telemetry_test` (new `tests/cli/dashboard_telemetry_test.cpp`, links `gufo_http`, labels `cpu`).
2. **`src/cli/serve/http_server.hpp`** — `#include "src/cli/serve/dashboard_telemetry.hpp"`; `HttpServer` gains a `std::unique_ptr<TelemetrySampler> telemetry_` member (started in the constructor with the `backend_ != nullptr` flag, stopped in `stop()`).
3. **`src/cli/serve/http_server.cpp`**
   - `handle_request` (line 1252): before `IsAuthorized` — `GET /dashboard` → embedded HTML (200, `text/html; charset=utf-8`, `Content-Length`, `Cache-Control: no-store`). After the gate (near the `/v1/models` branch, line 1294) — `GET /dashboard/status` and `GET /dashboard/requests` built with `json::Value` from `telemetry_->Snapshot()`, `dashboard::Recorder()`, and the backend virtuals.
   - `handle_connection` (line 1350): `dashboard::Recorder().begin(req)` right after `req.request_id` is assigned (line 1365); `finish(...)` in each exit path (normal / `TextGenerationError` / `std::exception` / `...`) using the same `response_status`, `elapsed_ms()`, and `outcome` values already passed to `Logger::LogRequest`; gated on the same log predicate (line 1557).
   - `WithTiming` (line 372): after `RecordServerMetrics(result)`, call `dashboard::Recorder().note_generation(result)` — additive, covers streamed and non-streamed completions.
4. **`src/cli/serve/text_generation_backend.hpp`** — add to `TextGenerationBackend`:
   ```cpp
   struct ModelInfo { std::uint32_t context{0}; std::size_t sessions{1}; std::string speculative{"off"}; };
   [[nodiscard]] virtual ModelInfo model_info() const { return ModelInfo{}; }
   ```
   `InferenceBackend` (inference_backend.hpp/.cpp) overrides: `max_context()` exists; store `session_count` and the speculative backend name during `load()` (the off/dflash2/dspark/mtp mapping already exists at serve.cpp:1201-1208) and return them.
5. **`docs/SERVER.md`** — endpoint table (line ~279): add `/dashboard`, `/dashboard/status`, `/dashboard/requests`; short "Dashboard" section: what it shows, auth behavior, metadata-only note tied to the existing telemetry policy (line 255-257), polling cadence, and the per-platform availability table (Windows: GPU util via Pdh incl. display-engine footnote; temp/power/sysfs-VRAM unavailable); note it complements (not replaces) `/metrics`.

## Ordered implementation tasks

1. `dashboard_recorder.{hpp,cpp}`: record struct (fields per contract), ring buffer, thread-local linkage, `begin`/`finish`/`note_generation`/`list`/`active_count` — no server changes yet.
2. `dashboard_telemetry.{hpp,cpp}`: cross-platform readers — `LinuxSysfs` (RAM, CPU static, GPU discovery), `DeviceMemoryInfo` VRAM, GPU name (sysfs / `hipGetDeviceProperties`), GPU busy (sysfs / Pdh), temp/power (sysfs / null), CPU % (`/proc/stat` / `GetSystemTimes`), RSS, rolling tps — sampler thread, 60-sample history, exception guards, `platform` in the snapshot.
3. `text_generation_backend.hpp` `ModelInfo` + `InferenceBackend` override (store session count + speculative name in `load()`).
4. `http_server.hpp/.cpp` integration: sampler member, `/dashboard` page route, `/dashboard/status` + `/dashboard/requests` routes, `handle_connection` begin/finish, `WithTiming` hook.
5. `src/cli/serve/web/dashboard.html` (Overview → Chat → Models), API-key handling, 2 s polling, canvas sparklines, platform-aware null rendering.
6. CMake: sources, `gufo_diagnostics` link, `pdh` (WIN32), embed step, include dir, two test targets.
7. Tests (plain `assert()` + `main()`, repo style):
   - `tests/cli/dashboard_telemetry_test.cpp`: after one sample — `cpu.pct` in [0,100] when readable, `ram_total_bytes > 0`, `platform` matches the build OS, history deques respect the 60 cap, `Snapshot()` stable under concurrent polling, GPU fields null (not crash) when unavailable (cpu-only builds, hosts without amdgpu/Pdh counters).
   - `tests/cli/dashboard_test.cpp` — `FakeBackend` + `RunningServer` harness pattern from `tests/cli/http_server_test.cpp` (lines 29-226):
     - `GET /dashboard` → 200 `text/html`, non-empty, without auth; `GET /dashboard/status` → 401 without key, 200 with (server constructed with `api_key` option).
     - `/dashboard/status` JSON has contract keys; `server.model` matches `FakeBackend::model_id()`; totals increment after a generation.
     - `POST /v1/chat/completions` via `FakeBackend` (canned `Result` with known tokens/timings) → `/dashboard/requests` returns exactly one record with matching `path`, `status 200`, `prompt_tokens`/`completion_tokens`, `ttft_ms`, `gen_tps`, `finish`; health/metrics/dashboard polls do **not** appear; newest-first ordering.
     - Ring buffer cap: seed >100 records (or test seam to lower the cap), assert oldest dropped.
8. `docs/SERVER.md` update.
9. Format check: `nix shell --inputs-from . nixpkgs#clang-tools -c python3 tools/ci/check-format.py` (add `--fix` if needed), then rerun.

## Rollout

Purely additive: no existing endpoint, log format, config flag, or data format changes; no migration or state to carry over. The dashboard appears on the next server start (the user's currently running server keeps its behavior until restarted). Disabling path if ever needed: none required at runtime — the routes are cheap no-ops when idle, and the only always-on cost is the 1 Hz sampler thread.

## Validation

1. Hosted (no GPU): `cmake --build --preset pr` — the new tests are `cpu`-labeled and must pass on both Linux and Windows CI hosts (null-GPU paths, Pdh-absent path, `GetSystemTimes`/`/proc/stat` CPU %).
2. GPU builds:
   - Linux Strix Halo: `nix develop -c cmake --preset gpu-test` → build `dashboard_test dashboard_telemetry_test http_server_test` → `nix develop -c ctest --preset gpu-full -R '^(dashboard_test|dashboard_telemetry_test|http_server_test)$' --output-on-failure`.
   - Windows Strix Halo: build the release preset on the Windows ROCm toolchain, run the same three ctest targets.
3. Manual smoke — **Linux**: `./build/release/gufo serve -p 8123 llm -m <gguf>` → open `http://127.0.0.1:8123/dashboard`; verify: tiles and sparklines update every ~1-2 s; GPU util % climbs during generation (send one via the Chat tab — confirm streaming tokens, Stop works, and the record appears in Overview with sensible TTFT/tok/s); temp/power/sysfs-VRAM populated; Models tab shows context/sessions/speculative; with `--api-key`, the page loads but JSON calls prompt for the key.
4. Manual smoke — **Windows**: same steps on a Windows Strix Halo dev machine; verify: GPU util % present and plausible (max across Pdh engines, display-activity footnote shown), VRAM from the DXGI budget, GPU name from `hipGetDeviceProperties`, temp/power/sysfs-VRAM rendered as `—`, CPU % and RAM live, chat + requests work identically.
5. Regression: existing `http_server_test` stays green on both OSes (route additions must not alter existing endpoints; `WithTiming` change is additive).

## Risks / failure modes

- **VRAM read before HIP init** — poll `DeviceMemoryInfo` and `hipGetDeviceProperties` only when a text backend exists (model loads before `HttpServer` is constructed, serve.cpp:1217, so the runtime is up); treat any `hipError_t != hipSuccess` as null; guard with `#if defined(ENGINE_ENABLE_HIP)` so cpu-only builds compile (`ENGINE_ENABLE_HIP` is a PUBLIC definition of `gufo_core`, CMakeLists.txt:197).
- **Windows GPU util includes display activity** — Strix Halo's iGPU drives the displays, so Pdh engine counters reflect compositor work even when idle; reported as max across engines with an explicit UI footnote (documented in SERVER.md). Acceptable: the alternative (ADL vendor DLL) violates gufo's system-library-only dependency rule.
- **Pdh absent/failing (CI, GPU-less hosts, restricted sessions)** — query creation or collection fails → `util_pct` null, sampler continues; test asserts null-without-crash.
- **Windows / no-sysfs hosts** — all sysfs-sourced fields null, CPU/RAM/VRAM/util still populated; the page renders nulls as `—` uniformly.
- **`gufo_diagnostics` link** — static lib, only `linux_sysfs.o` is pulled into `gufo_http`; its Windows branch relies on the globally linked `gufo_posix_compat` shim (already the case, CMakeLists.txt:30-34); the HIP benchmark object stays out of cpu-only builds.
- **Polling load** — two short GETs per 2 s per open tab against `max_connections=16`; negligible, and dashboard paths never enter the recorder or request log (log predicate).
- **Sampler lifetime** — thread joined in `HttpServer::stop()`/destructor; Windows Pdh handle closed there too; tests create/destroy many `HttpServer` instances, so startup must be cheap and shutdown deterministic (no detached threads, no leaked Pdh queries).
- **Embed size** — one ~100 KB byte array in the binary (acceptable; the binary embeds far more). Page must contain no secret, so unauthenticated serving is safe.
- **SSE parsing in browser** — reuse the proven server framing (chunked `text/event-stream`); client splits on `\n\n`, reads `data:` lines, ignores the final `data: [DONE]`.

## Out of scope

- Runtime model load/unload endpoints (not implemented in gufo; documented SERVER.md TODO).
- Multi-process "available models" discovery (one model per process by design; llama-swap routes between processes).
- Capturing prompt/response content in request records (user decision: metadata only).
- Prometheus changes (`/metrics` stays as-is; the dashboard complements it).
- Video/TTS/ASR job detail views (their requests appear as transport-level rows only).
- Extending `LinuxSysfs::QueryDrmGpuDevices()` with live keys (it feeds the `gufo diagnose` report contract; live reads live in the sampler).
- Vendor GPU libraries on Windows (ADL/`atiadlxx.dll`): not system libraries, unstable ABI; Pdh is the sanctioned source for GPU busy %, and temp/power remain `null` there.
