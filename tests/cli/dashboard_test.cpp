#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "src/cli/serve/dashboard_html.hpp"
#include "src/cli/serve/dashboard_recorder.hpp"
#include "src/cli/serve/dashboard_telemetry.hpp"
#include "src/cli/serve/generation_metrics.hpp"
#include "src/cli/serve/http_server.hpp"
#include "src/core/json.hpp"
#include "src/core/platform/socket.hpp"

namespace {

using gufo::server::TextGenerationBackend;
using gufo::server::dashboard::ClientMetrics;
using gufo::server::dashboard::DashboardRecorder;
using gufo::server::dashboard::RequestRecord;
using gufo::server::dashboard::TelemetrySampler;

RequestRecord FinishAndFind(DashboardRecorder& recorder, const std::string& id,
                            int status, bool record) {
  recorder.begin(id, "POST", "/v1/chat/completions", "127.0.0.1");
  recorder.finish(status, 12.5, "completed", true, record);
  for (const auto& entry : recorder.list()) {
    if (entry.id == id) {
      return entry;
    }
  }
  return {};
}

TextGenerationBackend::Result SampleResult() {
  TextGenerationBackend::Result result;
  result.prompt_tokens = 120;
  result.cached_prompt_tokens = 40;
  result.completion_tokens = 30;
  result.prefill_tokens = 120;
  result.ttft_ms = 80.0;
  result.queue_ms = 5.0;
  result.prefill_ms = 40.0;
  result.decode_ms = 120.0;
  result.execution_plan = "serial-c1";
  result.queue_depth_at_submit = 2;
  result.draft_tokens = 8;
  result.draft_accepted_tokens = 5;
  return result;
}

void TestLifecycle() {
  DashboardRecorder recorder;
  assert(recorder.active_count() == 0);
  assert(recorder.total_requests() == 0);
  assert(recorder.list().empty());

  recorder.note_generation(SampleResult());

  recorder.begin("r1", "GET", "/health", "10.0.0.2");
  assert(recorder.active_count() == 1);
  assert(recorder.total_requests() == 1);
  recorder.finish(200, 3.25, "ok", false, false);
  assert(recorder.active_count() == 0);
  assert(recorder.list().empty());

  const auto entry = FinishAndFind(recorder, "r2", 404, true);
  assert(!entry.id.empty());
  assert(entry.method == "POST");
  assert(entry.status == 404);
  assert(entry.outcome == "completed");
  assert(entry.stream);
  assert(!entry.generation);
  assert(std::abs(entry.duration_ms - 12.5) < 1e-9);
  assert(entry.started_at > 0);

  std::thread([] {
    auto& shared = gufo::server::dashboard::Recorder();
    shared.begin("worker", "GET", "/worker", "127.0.0.1");
    shared.finish(200, 1.0, "ok", false, true);
  }).join();
  bool seen = false;
  for (const auto& record : gufo::server::dashboard::Recorder().list()) {
    seen = seen || record.id == "worker";
  }
  assert(seen);
}

void TestNoteGenerationMapping() {
  DashboardRecorder recorder;
  recorder.begin("g1", "POST", "/v1/chat/completions", "127.0.0.1");
  auto result = SampleResult();
  result.cache_hit = true;
  result.finish_reason = TextGenerationBackend::FinishReason::kLength;
  recorder.note_generation(result);
  recorder.finish(200, 250.0, "completed", true, true);

  const auto records = recorder.list();
  assert(records.size() == 1);
  const RequestRecord& entry = records.front();
  assert(entry.generation);
  assert(entry.prompt_tokens == 120);
  assert(entry.completion_tokens == 30);
  assert(entry.cached_prompt_tokens == 40);
  assert(std::abs(entry.ttft_ms - 80.0) < 1e-9);
  assert(std::abs(entry.queue_ms - 5.0) < 1e-9);
  assert(std::abs(entry.prefill_ms - 40.0) < 1e-9);
  assert(std::abs(entry.decode_ms - 120.0) < 1e-9);
  assert(std::abs(entry.prompt_tps -
                  gufo::server::PrefillTokensPerSecond(result)) < 1e-9);
  assert(std::abs(entry.gen_tps - 250.0) < 1e-9);
  assert(entry.finish == "length");
  assert(entry.cache == "memory");
  assert(entry.queue_depth == 2);
  assert(entry.plan == "serial-c1");
  assert(entry.draft_proposed == 8);
  assert(entry.draft_accepted == 5);

  recorder.begin("g2", "POST", "/v1/completions", "127.0.0.1");
  auto disk = SampleResult();
  disk.cache_hit = true;
  disk.cache_disk_hit = true;
  disk.cancelled = true;
  recorder.note_generation(disk);
  recorder.finish(499, 10.0, "cancelled", false, true);

  recorder.begin("g3", "POST", "/v1/completions", "127.0.0.1");
  recorder.note_generation(SampleResult());
  recorder.finish(200, 10.0, "completed", false, true);

  const auto all = recorder.list();
  assert(all.size() == 3);
  assert(all[0].id == "g3");
  assert(all[0].finish == "stop");
  assert(all[0].cache == "miss");
  assert(all[1].id == "g2");
  assert(all[1].finish == "cancelled");
  assert(all[1].cache == "disk");
}

void TestRingCapAndOrder() {
  DashboardRecorder recorder;
  for (int i = 0; i < 105; ++i) {
    recorder.begin("id" + std::to_string(i), "GET", "/x", "127.0.0.1");
    recorder.finish(200, 1.0, "ok", false, true);
  }
  const auto records = recorder.list();
  assert(records.size() == DashboardRecorder::kMaxRecords);
  assert(records.front().id == "id104");
  assert(records.back().id == "id5");
  assert(recorder.active_count() == 0);
  assert(recorder.total_requests() == 105);
}

void TestClientMetrics() {
  DashboardRecorder recorder;
  assert(recorder.clients().empty());

  recorder.begin("c1", "POST", "/v1/completions", "10.0.0.5");
  recorder.note_generation(SampleResult());
  recorder.finish(200, 100.0, "completed", true, true);
  recorder.begin("c2", "GET", "/health", "10.0.0.5");

  const auto clients = recorder.clients();
  assert(clients.size() == 1);
  const ClientMetrics& busy = clients.front();
  assert(busy.client == "10.0.0.5");
  assert(busy.requests == 2);
  assert(busy.active == 1);
  assert(busy.generations == 1);
  assert(busy.prompt_tokens == 120);
  assert(busy.completion_tokens == 30);
  assert(busy.cached_prompt_tokens == 40);
  assert(busy.prefill_tokens == 120);
  assert(busy.draft_proposed == 8);
  assert(busy.draft_accepted == 5);
  assert(std::abs(busy.prefill_ms - 40.0) < 1e-9);
  assert(std::abs(busy.decode_ms - 120.0) < 1e-9);
  assert(busy.last_method == "GET");
  assert(busy.last_path == "/health");
  assert(busy.last_seen > 0);

  recorder.finish(200, 1.0, "ok", false, false);
  for (const auto& client : recorder.clients()) {
    assert(client.active == 0);
    assert(client.requests == 2);
  }

  // Quiet paths carry no client entry even when they complete a generation.
  DashboardRecorder quiet;
  quiet.begin("q1", "GET", "/dashboard/status", "10.0.0.2", false);
  quiet.note_generation(SampleResult());
  quiet.finish(200, 1.0, "ok", false, false);
  assert(quiet.clients().empty());
  assert(quiet.list().empty());
  assert(quiet.active_count() == 0);
  assert(quiet.total_requests() == 1);

  // The table is bounded, and an in-flight client is never retired. begin()
  // and finish() pair per connection thread, so hold one open on its own
  // thread while the table fills.
  std::atomic_flag held_started = ATOMIC_FLAG_INIT;
  std::atomic_flag release_held = ATOMIC_FLAG_INIT;
  std::thread holder([&recorder, &held_started, &release_held] {
    recorder.begin("held", "POST", "/v1/chat/completions", "held-peer");
    held_started.test_and_set();
    held_started.notify_all();
    release_held.wait(false);
    recorder.finish(200, 1.0, "ok", true, true);
  });
  held_started.wait(false);
  for (int i = 0; i < 80; ++i) {
    recorder.begin("k" + std::to_string(i), "GET", "/x",
                   "peer" + std::to_string(i));
    recorder.finish(200, 1.0, "ok", false, false);
  }
  bool held = false;
  for (const auto& client : recorder.clients()) {
    held = held || client.client == "held-peer";
  }
  assert(held);
  assert(recorder.clients().size() <= DashboardRecorder::kMaxClients + 1);
  release_held.test_and_set();
  release_held.notify_all();
  holder.join();
}

void TestEmbeddedPage() {
  assert(gufo::server::kDashboardHtmlSize > 4096);
  assert(gufo::server::kDashboardHtml[gufo::server::kDashboardHtmlSize] == 0);
  assert(std::strlen(
             reinterpret_cast<const char*>(gufo::server::kDashboardHtml)) ==
         gufo::server::kDashboardHtmlSize);
  const std::string html(
      reinterpret_cast<const char*>(gufo::server::kDashboardHtml),
      gufo::server::kDashboardHtmlSize);
  assert(html.rfind("<!DOCTYPE html>", 0) == 0);
  assert(html.find("</html>") != std::string::npos);
  assert(html.find("/dashboard/status") != std::string::npos);
  assert(html.find("/dashboard/requests") != std::string::npos);
  assert(html.find("gufoApiKey") != std::string::npos);
  assert(html.find("http://") == std::string::npos);
  assert(html.find("https://") == std::string::npos);
}

void TestTelemetry() {
  TelemetrySampler sampler(false);
  const auto initial = sampler.snapshot();
  assert(initial.platform == "linux" || initial.platform == "windows");

  sampler.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(2200));
  const auto snap = sampler.snapshot();
  assert(snap.platform == initial.platform);
  assert(snap.cpu_threads > 0);
  assert(snap.ram_total_bytes.has_value());
  assert(snap.ram_total_bytes.value() > 0);
  assert(snap.ram_used_bytes.has_value());
  assert(snap.process_rss_bytes.has_value());
  assert(snap.process_rss_bytes.value() > 0);
  assert(snap.cpu_pct.has_value());
  assert(snap.cpu_pct.value() >= 0.0 && snap.cpu_pct.value() <= 100.0);
  assert(!snap.history.empty());
  assert(snap.history.size() <= gufo::server::dashboard::kHistoryCapacity);
  assert(snap.history.back().t >= snap.history.front().t);
  for (const auto& sample : snap.history) {
    if (sample.prompt_tps) {
      assert(*sample.prompt_tps >= 0.0);
    }
    if (sample.gpu_power_w) {
      assert(*sample.gpu_power_w >= 0.0 && *sample.gpu_power_w <= 2000.0);
    }
  }
  sampler.stop();
  sampler.stop();
}

class StubBackend final : public TextGenerationBackend {
public:
  [[nodiscard]] std::string model_id() const override { return "stub-model"; }
  [[nodiscard]] bool ready() const override { return true; }
  [[nodiscard]] ModelInfo model_info() const override {
    ModelInfo info;
    info.context = 4096;
    info.sessions = 2;
    info.speculative = "mtp";
    return info;
  }
  [[nodiscard]] std::size_t count_tokens(std::string_view text) const override {
    return text.size();
  }
  Result complete(std::string_view prompt, std::size_t limit,
                  const gufo::sampling::SamplingConfig& sampling,
                  const CancellationCheck& cancel, const TokenCallback& token,
                  std::string_view client_id = "anonymous",
                  const std::vector<std::string>& = {}) override {
    (void)prompt;
    (void)limit;
    (void)sampling;
    (void)cancel;
    (void)client_id;
    Result result;
    result.text = "reply";
    result.prompt_tokens = 10;
    result.cached_prompt_tokens = 8;
    result.cache_hit = true;
    result.prefill_tokens = 2;
    result.prefill_ms = 4.0;
    result.completion_tokens = 3;
    result.decode_ms = 12.0;
    result.ttft_ms = 6.0;
    result.execution_plan = "serial-c1";
    if (token) {
      (void)token(result.text);
    }
    return result;
  }
  Result chat(const gufo::server::ChatRequest& request, std::size_t limit,
              const gufo::sampling::SamplingConfig& sampling,
              const CancellationCheck& cancel,
              const TokenCallback& token) override {
    auto result =
        complete("", limit, sampling, cancel, token, request.client_id);
    return result;
  }
};

class TestServer {
public:
  explicit TestServer(std::string api_key = {})
      : server("127.0.0.1", 0, std::make_shared<StubBackend>(), nullptr,
               nullptr, nullptr,
               gufo::server::HttpServerOptions{.api_key = std::move(api_key)}) {
    std::string error;
    assert(server.start(&error));
    worker = std::jthread([this] { server.run(false); });
  }
  ~TestServer() {
    server.stop();
    worker.join();
  }

  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  [[nodiscard]] std::string Request(const std::string& payload) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    assert(gufo::platform::SetSocketTimeout(fd, SO_RCVTIMEO,
                                            std::chrono::seconds{5}) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.port());
    assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == 0);
    std::string_view pending = payload;
    while (!pending.empty()) {
      const auto sent =
          ::send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
      assert(sent > 0);
      pending.remove_prefix(static_cast<std::size_t>(sent));
    }
    std::string response;
    char buffer[8192];
    for (;;) {
      const auto count = gufo::platform::SocketRead(fd, buffer, sizeof(buffer));
      assert(count >= 0);
      if (count == 0) {
        break;
      }
      response.append(buffer, static_cast<std::size_t>(count));
    }
    gufo::platform::CloseSocket(fd);
    return response;
  }

  [[nodiscard]] std::string Get(const std::string& path,
                                const std::string& auth = {}) {
    return Request("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1" + auth +
                   "\r\n\r\n");
  }

  [[nodiscard]] std::string Post(const std::string& path,
                                 const std::string& body,
                                 const std::string& auth = {}) {
    return Request("POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1" + auth +
                   "\r\nContent-Length: " + std::to_string(body.size()) +
                   "\r\n\r\n" + body);
  }

  gufo::server::HttpServer server;

private:
  std::jthread worker;
};

std::string HeaderOf(const std::string& response, std::string_view name) {
  const auto head = response.substr(0, response.find("\r\n\r\n"));
  std::size_t at = 0;
  while ((at = head.find(name, at)) != std::string::npos) {
    if (at == 0 || head[at - 1] == '\n') {
      const auto end = head.find("\r\n", at);
      return head.substr(at + name.size(), end - at - name.size());
    }
    at += name.size();
  }
  return {};
}

gufo::json::Value BodyJson(const std::string& response) {
  const auto split = response.find("\r\n\r\n");
  assert(split != std::string::npos);
  return gufo::json::parse(response.substr(split + 4));
}

bool StatusIs(const std::string& response, int status) {
  return response.starts_with("HTTP/1.1 " + std::to_string(status) + " ");
}

const RequestRecord* FindPath(const std::deque<RequestRecord>& records,
                              std::string_view path) {
  for (const auto& record : records) {
    if (record.path == path) {
      return &record;
    }
  }
  return nullptr;
}

std::size_t CountSuccessful(const std::deque<RequestRecord>& records,
                            std::string_view path) {
  std::size_t count = 0;
  for (const auto& record : records) {
    if (record.path == path && record.status == 200) {
      ++count;
    }
  }
  return count;
}

/// Waits for the sampler's first 1 Hz sample so hardware fields are filled.
gufo::json::Value Sample(TestServer& server, const std::string& auth) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    const auto response = server.Get("/dashboard/status", auth);
    assert(StatusIs(response, 200));
    auto body = BodyJson(response);
    if (body.at("hardware").at("ram_total_bytes").is_number()) {
      return body;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::cerr << "dashboard telemetry sampler produced no sample\n";
  std::abort();
}

void TestDashboardRoutes() {
  const std::string auth = "\r\nAuthorization: Bearer test-secret";

  TestServer secured("test-secret");
  // The page carries no secrets and must load before a key is known.
  const auto page = secured.Get("/dashboard");
  assert(StatusIs(page, 200));
  assert(HeaderOf(page, "Content-Type: ").find("text/html") !=
         std::string::npos);
  assert(page.find("<!DOCTYPE html>") != std::string::npos);
  assert(StatusIs(secured.Get("/dashboard/"), 200));
  assert(StatusIs(secured.Get("/dashboard/status"), 401));
  assert(StatusIs(secured.Get("/dashboard/requests"), 401));
  assert(StatusIs(secured.Get("/dashboard/status", auth), 200));

  const auto status = Sample(secured, auth);
  for (const char* key : {"server", "hardware", "throughput", "history"}) {
    assert(status.contains(key));
  }
  const auto& server_info = status.at("server");
  assert(server_info.member_str("model") == "stub-model");
  assert(server_info.at("ready").as_bool());
  assert(server_info.member_size("context") == 4096);
  assert(server_info.member_size("sessions") == 2);
  assert(server_info.member_str("speculative") == "mtp");
  assert(server_info.at("sampling").contains("temperature"));
  assert(server_info.at("reasoning").contains("effort"));
  assert(server_info.at("services").at("text").as_bool());
  assert(!server_info.at("services").at("video").as_bool());
  const auto& hardware = status.at("hardware");
#ifdef _WIN32
  assert(hardware.member_str("platform") == "windows");
#else
  assert(hardware.member_str("platform") == "linux");
#endif
  assert(hardware.member_size("ram_total_bytes") > 0);
  assert(status.at("history").at("t").is_array());
  const auto& history = status.at("history");
  for (const char* key : {"cpu", "ram_used", "gpu_util", "gpu_power",
                          "vram_used", "gen_tps", "prompt_tps"}) {
    assert(history.contains(key));
    assert(history.at(key).is_array());
    assert(history.at(key).size() == history.at("t").size());
  }
  const auto generated_before =
      server_info.member_size("total_generated_tokens");

  const std::string body =
      R"({"model":"stub-model","messages":[{"role":"user","content":"hi"}],"max_tokens":3})";
  assert(StatusIs(secured.Post("/v1/chat/completions", body), 401));
  assert(StatusIs(secured.Post("/v1/chat/completions", body, auth), 200));
  assert(StatusIs(secured.Post("/v1/chat/completions", body, auth), 200));

  const auto after = BodyJson(secured.Get("/dashboard/status", auth));
  assert(after.at("server").member_size("total_generated_tokens") ==
         generated_before + 6);
  assert(after.at("server").member_size("total_requests") >= 3);
  assert(after.at("throughput").member_double("last_decode_tps") > 0.0);

  // Nothing is in flight once every response completed, and each finished
  // generation lands in the per-client totals.
  const auto& active_now = after.at("server").at("active");
  assert(active_now.is_array() && active_now.empty());
  const auto& clients = after.at("clients");
  assert(clients.is_array() && !clients.empty());
  const gufo::json::Value* loopback = nullptr;
  for (const auto& entry : clients.items()) {
    if (entry.member_str("client") == "127.0.0.1") {
      loopback = &entry;
    }
  }
  assert(loopback != nullptr);
  assert(loopback->member_size("active") == 0);
  assert(loopback->member_size("generations") == 2);
  assert(loopback->member_size("prompt_tokens") == 20);
  assert(loopback->member_size("completion_tokens") == 6);
  assert(loopback->member_size("cached_prompt_tokens") == 16);
  assert(loopback->member_size("prefill_tokens") == 4);
  assert(loopback->member_double("gen_tps") > 0.0);
  assert(loopback->member_str("last_path") == "/v1/chat/completions");
  // The dashboard's own quiet polling stays out of the client table.
  assert(loopback->member_size("requests") >= 3);

  const auto records = gufo::server::dashboard::Recorder().list();
  const auto* first = FindPath(records, "/v1/chat/completions");
  assert(first != nullptr);
  assert(first->generation);
  assert(first->status == 200);
  assert(first->method == "POST");
  assert(first->prompt_tokens == 10);
  assert(first->completion_tokens == 3);
  assert(first->cached_prompt_tokens == 8);
  assert(first->finish == "stop");
  assert(first->cache == "memory");
  assert(first->client == "127.0.0.1");
  assert(first->duration_ms >= 0.0);
  // Two successful generations plus the rejected one (errors are recorded like
  // the request log does), newest first.
  std::vector<std::size_t> accepted;
  std::size_t rejected = 0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (records[i].path != "/v1/chat/completions") {
      continue;
    }
    if (records[i].status == 200) {
      accepted.push_back(i);
    } else {
      ++rejected;
    }
  }
  assert(accepted.size() == 2);
  assert(rejected == 1);
  const auto id_number = [](const std::string& id) {
    return std::strtoull(id.c_str() + 1, nullptr, 10);
  };
  assert(id_number(records[accepted[0]].id) >
         id_number(records[accepted[1]].id));
  assert(records[accepted[0]].generation);
  assert(!records[accepted[1]].generation ||
         records[accepted[1]].prompt_tokens == 10);

  // Successful quiet polling never enters the request list; the 401s above do,
  // exactly like the request log treats every status >= 400.
  assert(StatusIs(secured.Get("/health", auth), 200));
  assert(StatusIs(secured.Get("/dashboard/status", auth), 200));
  assert(StatusIs(secured.Get("/dashboard/requests", auth), 200));
  const auto polled = gufo::server::dashboard::Recorder().list();
  assert(CountSuccessful(polled, "/health") == 0);
  assert(CountSuccessful(polled, "/dashboard") == 0);
  assert(CountSuccessful(polled, "/dashboard/status") == 0);
  assert(CountSuccessful(polled, "/dashboard/requests") == 0);
  const auto listed = BodyJson(secured.Get("/dashboard/requests", auth));
  const auto& requests = listed.at("requests");
  assert(requests.is_array());
  assert(requests.size() == polled.size());
  const auto& entry = requests.items().front();
  for (const char* key : {"id", "method", "path", "client", "status", "outcome",
                          "stream", "finish"}) {
    assert(entry.contains(key));
  }
}

}  // namespace

int main() {
  TestLifecycle();
  TestNoteGenerationMapping();
  TestRingCapAndOrder();
  TestEmbeddedPage();
  TestClientMetrics();
  TestTelemetry();
  TestDashboardRoutes();
  std::cout << "dashboard_test: ok\n";
  return 0;
}
