#include "DynamicWorker.h"
#include "TestHelpers.h"
#include <armonik/sdk/common/Configuration.h>
#include <armonik/sdk/common/Diagnostics.h>
#include <armonik/sdk/common/TaskPayload.h>
#include <gtest/gtest.h>
#include <stdexcept>

using ArmoniK::Sdk::Common::Configuration;
using ArmoniK::Sdk::Common::DynamicLibrary;
using ArmoniK::Sdk::Common::TaskOptions;
using ArmoniK::Sdk::DynamicWorker::DynamicWorker;
using ArmoniK::Sdk::Worker::UnitTest::ConventionOptions;
using ArmoniK::Sdk::Worker::UnitTest::FakeAgentServer;
using ArmoniK::Sdk::Worker::UnitTest::MakeTestLogger;
using ArmoniK::Sdk::Worker::UnitTest::ReadFile;
using ArmoniK::Sdk::Worker::UnitTest::TaskBuilder;
using ArmoniK::Sdk::Worker::UnitTest::TempDir;

namespace {
class DynamicWorkerTest : public ::testing::Test {
protected:
  DynamicWorkerTest() : logger(MakeTestLogger()) {
    config.set("Worker__ApplicationBasePath", app_dir.path());
    symlink(FIXTURE_V1_PATH, app_dir.file("legacy.1").c_str());
    symlink(FIXTURE_V2_PATH, app_dir.file("legacy.2").c_str());
    stub = server.NewStub();
    worker.reset(new DynamicWorker(server.NewStub(), config, logger));
  }

  armonik::api::worker::ProcessStatus Run(TaskBuilder &task) {
    task.AddExpectedOutput("result");
    auto handler = task.Handler(*stub);
    return worker->Execute(*handler);
  }

  /**
   * @brief Runs a convention task calling symbol from the library and returns its result or error details
   */
  std::string RunConvention(const TaskOptions &options, const std::map<std::string, std::string> &inputs = {},
                            const std::string &session = "session") {
    TaskBuilder task;
    task.SetSession(session).SetTaskOptions(options).SetConventionPayload(inputs, {{"out", "result"}});
    const auto status = Run(task);
    return status.ok() ? task.Result("result") : "error: " + status.details();
  }

  TempDir app_dir;
  Configuration config;
  armonik::api::common::logger::Logger logger;
  FakeAgentServer server;
  std::unique_ptr<armonik::api::grpc::v1::agent::Agent::Stub> stub;
  std::unique_ptr<DynamicWorker> worker;
};

TaskOptions LegacyOptions(const std::string &name, const std::string &version) {
  return TaskOptions(name, version, "", "");
}

std::string LegacyPayload(const std::string &method, const std::string &arguments) {
  ARMONIK_SDK_SUPPRESS_DEPRECATED_BEGIN
  return ArmoniK::Sdk::Common::TaskPayload(method, arguments).Serialize();
  ARMONIK_SDK_SUPPRESS_DEPRECATED_END
}
} // namespace

TEST_F(DynamicWorkerTest, ConventionTaskCallsLibrarySymbol) {
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "version")), "v1");
  EXPECT_EQ(server.service().NotifiedResultIds(), std::vector<std::string>({"result"}));
}

TEST_F(DynamicWorkerTest, ConventionTaskPassesServiceIdentityAndSession) {
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "service", "ns", "svc")), "ns::svc");
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "session", "ns", "svc"), {}, "other"), "other");
}

TEST_F(DynamicWorkerTest, ConventionTaskReloadsLibraryWhenPathChanges) {
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "version")), "v1");
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V2_PATH, "version")), "v2");
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "version")), "v1");
}

TEST_F(DynamicWorkerTest, ConventionTaskWithSdkServiceResolvesDataDependencies) {
  TaskBuilder task;
  task.SetTaskOptions(ConventionOptions(SDK_FIXTURE_PATH, "add"))
      .SetConventionPayload({{"a", "blob-a"}, {"b", "3"}}, {{"out", "result"}})
      .AddDataDependency("blob-a", "2");

  const auto status = Run(task);

  ASSERT_TRUE(status.ok()) << status.details();
  EXPECT_EQ(task.Result("result"), "5");
}

TEST_F(DynamicWorkerTest, ConventionTaskWithSdkServicePermanentErrorFails) {
  EXPECT_EQ(RunConvention(ConventionOptions(SDK_FIXTURE_PATH, "fail")), "error: permanent failure");
  EXPECT_TRUE(server.service().NotifiedResultIds().empty());
}

TEST_F(DynamicWorkerTest, ConventionTaskWithSdkServiceTransientErrorThrowsForRetry) {
  TaskBuilder task;
  task.SetTaskOptions(ConventionOptions(SDK_FIXTURE_PATH, "transient")).SetConventionPayload({}, {});
  EXPECT_THROW(Run(task), std::runtime_error);
  // The worker remains usable after a retryable error
  EXPECT_EQ(RunConvention(ConventionOptions(SDK_FIXTURE_PATH, "add"), {{"a", "1"}, {"b", "1"}}), "2");
}

TEST_F(DynamicWorkerTest, ConventionTaskWithSdkServiceUnknownMethodFails) {
  const auto result = RunConvention(ConventionOptions(SDK_FIXTURE_PATH, "unknown"));
  EXPECT_NE(result.find("ServiceBase::call not implemented for method: unknown"), std::string::npos) << result;
}

TEST_F(DynamicWorkerTest, LibraryErrorStatusIsReported) {
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "error_silent")),
            "error: Unknown error in worker, check logs.");
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "unknown")), "error: Unknown method unknown");
}

TEST_F(DynamicWorkerTest, ConventionTaskWithBadLibraryPathFails) {
  const auto result = RunConvention(ConventionOptions("/nonexistent/libnothing.so", "version"));
  EXPECT_NE(result.find("error: Could not dlopen /nonexistent/libnothing.so"), std::string::npos) << result;
}

TEST_F(DynamicWorkerTest, ConventionTaskWithMissingSymbolFails) {
  const auto result = RunConvention(ConventionOptions(FIXTURE_MISSING_CALL_PATH, "version"));
  EXPECT_NE(result.find("armonik_call"), std::string::npos) << result;
}

TEST_F(DynamicWorkerTest, WorkerRecoversAfterFailedLoads) {
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "version")), "v1");
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_MISSING_CALL_PATH, "version")).rfind("error: ", 0), 0u);
  EXPECT_EQ(RunConvention(ConventionOptions("/nonexistent/libnothing.so", "version")).rfind("error: ", 0), 0u);
  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "version")), "v1");
}

TEST_F(DynamicWorkerTest, ConventionTaskWithoutLibraryLocationFails) {
  auto options = TaskOptions("", "", "", "");
  options.SetConvention("version");
  EXPECT_EQ(RunConvention(options).rfind("error: ", 0), 0u);
}

TEST_F(DynamicWorkerTest, ConventionTaskWithMalformedPayloadFails) {
  TaskBuilder task;
  task.SetTaskOptions(ConventionOptions(FIXTURE_V1_PATH, "version")).SetPayload("not json");
  const auto status = Run(task);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(server.service().NotifiedResultIds().empty());
}

TEST_F(DynamicWorkerTest, ConventionTaskLoadsLibraryFromBlob) {
  const std::string blob_id = "unittest-blob-" + std::to_string(getpid());
  DynamicLibrary lib;
  lib.symbol = "version";
  lib.library_blob_id = blob_id;
  TaskOptions options("", "", "", "");
  options.SetDynamicLibrary(lib);

  TaskBuilder task;
  task.SetTaskOptions(options)
      .SetConventionPayload({}, {{"out", "result"}})
      .AddDataDependency(blob_id, ReadFile(FIXTURE_V2_PATH));
  const auto status = Run(task);
  std::remove(("/tmp/armonik-lib-" + blob_id + ".so").c_str());

  ASSERT_TRUE(status.ok()) << status.details();
  EXPECT_EQ(task.Result("result"), "v2");
}

TEST_F(DynamicWorkerTest, ConventionTaskWithMissingLibraryBlobFails) {
  DynamicLibrary lib;
  lib.symbol = "version";
  lib.library_blob_id = "absent-blob";
  TaskOptions options("", "", "", "");
  options.SetDynamicLibrary(lib);

  EXPECT_EQ(RunConvention(options), "error: Library blob 'absent-blob' not found in data dependencies");
}

TEST_F(DynamicWorkerTest, LegacyTaskLoadsApplicationFromBasePath) {
  for (const auto &version : {"1", "2"}) {
    TaskBuilder task;
    task.SetTaskOptions(LegacyOptions("legacy", version)).SetPayload(LegacyPayload("version", ""));
    const auto status = Run(task);
    ASSERT_TRUE(status.ok()) << status.details();
    EXPECT_EQ(task.Result("result"), std::string("v") + version);
  }
}

TEST_F(DynamicWorkerTest, LegacyTaskPassesArguments) {
  TaskBuilder task;
  task.SetTaskOptions(LegacyOptions("legacy", "1")).SetPayload(LegacyPayload("echo", std::string("raw\0args", 8)));
  const auto status = Run(task);
  ASSERT_TRUE(status.ok()) << status.details();
  EXPECT_EQ(task.Result("result"), std::string("raw\0args", 8));
}

TEST_F(DynamicWorkerTest, LegacyTaskWithUnknownApplicationFails) {
  TaskBuilder task;
  task.SetTaskOptions(LegacyOptions("unknown", "1")).SetPayload(LegacyPayload("version", ""));
  const auto status = Run(task);
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.details().find("Could not dlopen"), std::string::npos) << status.details();
}

TEST_F(DynamicWorkerTest, SwitchingBetweenLegacyAndConventionTasks) {
  TaskBuilder legacy;
  legacy.SetTaskOptions(LegacyOptions("legacy", "2")).SetPayload(LegacyPayload("version", ""));
  ASSERT_TRUE(Run(legacy).ok());
  EXPECT_EQ(legacy.Result("result"), "v2");

  EXPECT_EQ(RunConvention(ConventionOptions(FIXTURE_V1_PATH, "version")), "v1");

  TaskBuilder legacy_again;
  legacy_again.SetTaskOptions(LegacyOptions("legacy", "2")).SetPayload(LegacyPayload("version", ""));
  ASSERT_TRUE(Run(legacy_again).ok());
  EXPECT_EQ(legacy_again.Result("result"), "v2");
}
