// Reproduction tests for the client's two byte-budget backpressure knobs: GrpcClient__DownloadByteBudget
// (WaitResults) and GrpcClient__UploadByteBudget (Submit/SubmitRaw). The download test compares peak
// RSS delta between a disabled and a tight budget, since WaitResults() holds each received result in
// memory. The upload test compares elapsed time instead, since Submit() streams from the caller's own
// buffer rather than copying it, so the budget shows up as serialization, not resident memory.
//
// Requires a deployed ArmoniK cluster with the C++ end2end worker, same as the rest of this test
// binary (see .docs/content/guide/2.tests.md).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <armonik/common/logger/formatter.h>
#include <armonik/common/logger/logger.h>
#include <armonik/common/logger/writer.h>

#include "armonik/sdk/client/IServiceInvocationHandler.h"
#include "armonik/sdk/client/SessionService.h"
#include "armonik/sdk/common/Configuration.h"
#include "armonik/sdk/common/Diagnostics.h"
#include "armonik/sdk/common/Properties.h"
#include "armonik/sdk/common/TaskOptions.h"
#include <armonik/sdk/common/TaskPayload.h>

#include "E2ETestFixture.h"

ARMONIK_SDK_SUPPRESS_DEPRECATED_BEGIN

class DownloadByteBudget : public ArmoniKE2ETest {};
class UploadByteBudget : public ArmoniKE2ETest {};

namespace {

// Resident set size of the current process, in KB, or -1 if it could not be determined.
long GetProcessRssKB() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      long kb = 0;
      if (std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) {
        return kb;
      }
    }
  }
  return -1;
}

// Runs `work` while sampling RSS on a background thread, and returns the peak RSS observed during
// the call minus the RSS observed just before it started.
long MeasurePeakRssDeltaKB(const std::function<void()> &work) {
  std::atomic<bool> stop{false};
  std::atomic<long> peak{0};
  long baseline = GetProcessRssKB();

  std::thread sampler([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      long delta = GetProcessRssKB() - baseline;
      long prev = peak.load(std::memory_order_relaxed);
      while (delta > prev && !peak.compare_exchange_weak(prev, delta, std::memory_order_relaxed)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  work();

  stop.store(true, std::memory_order_relaxed);
  sampler.join();
  return peak.load();
}

class SizedEchoHandler final : public ArmoniK::Sdk::Client::IServiceInvocationHandler {
public:
  void HandleResponse(std::string &&result_payload, armonik::api::string_view /*taskId*/,
                      armonik::api::string_view /*result_id*/) override {
    std::lock_guard<std::mutex> _(mutex);
    ++received;
    total_bytes += result_payload.size();
  }
  void HandleError(const std::exception & /*e*/, armonik::api::string_view /*taskId*/) override {
    std::lock_guard<std::mutex> _(mutex);
    ++errors;
  }

  std::mutex mutex;
  int received = 0;
  int errors = 0;
  size_t total_bytes = 0;
};

// Configures a session with both byte budgets pinned explicitly (rather than leaving either to
// whatever add_env_configuration() picks up from the environment), so the download and upload
// tests stay hermetic with respect to each other and to ambient env vars.
std::tuple<ArmoniK::Sdk::Common::Properties, armonik::api::common::logger::Logger>
init_with_byte_budgets(std::int64_t download_byte_budget, std::int64_t upload_byte_budget) {
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();
  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }
  config.set("GrpcClient__DownloadByteBudget", std::to_string(download_byte_budget));
  config.set("GrpcClient__UploadByteBudget", std::to_string(upload_byte_budget));

  ArmoniK::Sdk::Common::TaskOptions task_options("libArmoniK.SDK.Worker.Test.so", config.get("WorkerLib__Version"),
                                                 "End2EndTest", "EchoService", config.get("PartitionId"));
  task_options.max_retries = 1;

  return std::make_tuple<ArmoniK::Sdk::Common::Properties, armonik::api::common::logger::Logger>(
      {config, task_options},
      {armonik::api::common::logger::writer_console(), armonik::api::common::logger::formatter_plain(true)});
}

std::vector<ArmoniK::Sdk::Common::TaskPayload> generate_sized_payloads(unsigned int n, size_t payload_bytes) {
  std::vector<ArmoniK::Sdk::Common::TaskPayload> payloads;
  payloads.reserve(n);
  std::string blob(payload_bytes, 'x');
  for (unsigned int i = 0; i < n; ++i) {
    payloads.emplace_back("EchoService", blob);
  }
  return payloads;
}

// Submits kTaskCount same-sized tasks under the given download byte budget (upload budget
// disabled), waits for all of them at once, and returns the peak RSS delta observed during that
// wait.
long RunDownloadBatchAndMeasurePeak(unsigned int task_count, size_t payload_bytes, std::int64_t download_byte_budget) {
  auto p = init_with_byte_budgets(download_byte_budget, /*upload_byte_budget=*/0);
  auto &properties = std::get<0>(p);
  auto &logger = std::get<1>(p);

  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  auto handler = std::make_shared<SizedEchoHandler>();
  auto task_ids = service.Submit(generate_sized_payloads(task_count, payload_bytes), handler);
  EXPECT_EQ(task_ids.size(), task_count);

  long peak_kb = MeasurePeakRssDeltaKB([&] { service.WaitResults(); });

  EXPECT_EQ(handler->received, static_cast<int>(task_count));
  EXPECT_EQ(handler->errors, 0);

  service.CloseSession();
  return peak_kb;
}

// Submits call_count * tasks_per_call same-sized tasks as call_count concurrent Submit() calls
// (tasks_per_call tasks per call, each call on its own thread), under the given upload byte budget
// (download budget disabled). Unlike downloaded results, a call's payload is never copied by the
// client -- it's streamed from the caller's own buffer in data_chunk_max_size chunks -- so the
// budget mostly bounds concurrency, not resident memory. That shows up as wall-clock time: a tight
// budget forces calls into more, smaller waves. Returns the elapsed time for all calls to complete,
// not including result draining.
long long RunUploadBatchAndMeasureElapsedMs(unsigned int call_count, unsigned int tasks_per_call, size_t payload_bytes,
                                            std::int64_t upload_byte_budget) {
  auto p = init_with_byte_budgets(/*download_byte_budget=*/0, upload_byte_budget);
  auto &properties = std::get<0>(p);
  auto &logger = std::get<1>(p);

  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  auto handler = std::make_shared<SizedEchoHandler>();

  // Pre-generate payloads outside the measured region, so the timing reflects Submit()'s own
  // admission/serialization, not payload construction.
  std::vector<std::vector<ArmoniK::Sdk::Common::TaskPayload>> payloads_per_call;
  payloads_per_call.reserve(call_count);
  for (unsigned int c = 0; c < call_count; ++c) {
    payloads_per_call.push_back(generate_sized_payloads(tasks_per_call, payload_bytes));
  }

  std::vector<std::vector<std::string>> task_ids_per_call(call_count);

  auto start = std::chrono::steady_clock::now();
  {
    std::vector<std::thread> threads;
    threads.reserve(call_count);
    for (unsigned int c = 0; c < call_count; ++c) {
      threads.emplace_back([&, c] { task_ids_per_call[c] = service.Submit(payloads_per_call[c], handler); });
    }
    for (auto &thread : threads) {
      thread.join();
    }
  }
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

  std::size_t total_task_ids = 0;
  for (const auto &ids : task_ids_per_call) {
    total_task_ids += ids.size();
  }
  unsigned int task_count = call_count * tasks_per_call;
  EXPECT_EQ(total_task_ids, task_count);

  service.WaitResults();
  EXPECT_EQ(handler->received, static_cast<int>(task_count));
  EXPECT_EQ(handler->errors, 0);

  service.CloseSession();
  return elapsed_ms;
}

} // namespace

TEST_F(DownloadByteBudget, tight_budget_bounds_peak_download_memory) {
  constexpr unsigned int kTaskCount = 64;
  constexpr size_t kPayloadBytes = 1 * 1024 * 1024; // 1 MiB per result

  // Disabled budget: unbounded, matching pre-existing behavior (concurrency limited only by
  // GrpcClient__ThreadPoolSize).
  long unbounded_peak_kb = RunDownloadBatchAndMeasurePeak(kTaskCount, kPayloadBytes, 0);

  // Tight budget: room for only a couple of results' worth of payload bytes at once, regardless of
  // how many results are ready in a given polling round or how many threads the pool has.
  long tight_peak_kb =
      RunDownloadBatchAndMeasurePeak(kTaskCount, kPayloadBytes, 2 * static_cast<std::int64_t>(kPayloadBytes));

  std::cout << "Peak RSS delta - unbounded: " << unbounded_peak_kb
            << " KB, tight budget (2 payloads): " << tight_peak_kb << " KB" << std::endl;

  EXPECT_LT(tight_peak_kb, unbounded_peak_kb);
}

TEST_F(UploadByteBudget, tight_budget_serializes_concurrent_uploads) {
  constexpr unsigned int kCallCount = 8;
  constexpr unsigned int kTasksPerCall = 8;         // 64 tasks total, spread across kCallCount concurrent calls
  constexpr size_t kPayloadBytes = 1 * 1024 * 1024; // 1 MiB per task payload
  constexpr std::int64_t kCallBytes =
      static_cast<std::int64_t>(kTasksPerCall) * static_cast<std::int64_t>(kPayloadBytes);

  long long unbounded_ms = RunUploadBatchAndMeasureElapsedMs(kCallCount, kTasksPerCall, kPayloadBytes, 0);

  // Tight budget: room for only 2 calls' worth of bytes in flight at once, which is stricter than
  // submit_admission_'s own per-worker cap, so it forces extra waves on top of that.
  long long tight_ms = RunUploadBatchAndMeasureElapsedMs(kCallCount, kTasksPerCall, kPayloadBytes, 2 * kCallBytes);

  std::cout << "Elapsed - unbounded: " << unbounded_ms << " ms, tight budget (2 calls): " << tight_ms << " ms"
            << std::endl;

  EXPECT_GT(tight_ms, unbounded_ms);
}

ARMONIK_SDK_SUPPRESS_DEPRECATED_END
