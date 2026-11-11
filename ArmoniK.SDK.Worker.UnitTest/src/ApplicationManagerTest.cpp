#include "ApplicationManager.h"
#include "TestHelpers.h"
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/common/Configuration.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using ArmoniK::Sdk::Common::ArmoniKSdkException;
using ArmoniK::Sdk::Common::Configuration;
using ArmoniK::Sdk::Common::DynamicLibrary;
using ArmoniK::Sdk::DynamicWorker::AppId;
using ArmoniK::Sdk::DynamicWorker::ApplicationManager;
using ArmoniK::Sdk::DynamicWorker::ServiceId;
using ArmoniK::Sdk::Worker::UnitTest::FakeAgentServer;
using ArmoniK::Sdk::Worker::UnitTest::MakeTestLogger;
using ArmoniK::Sdk::Worker::UnitTest::TaskBuilder;
using ArmoniK::Sdk::Worker::UnitTest::TempDir;

namespace {
DynamicLibrary Library(const std::string &path) {
  DynamicLibrary lib;
  lib.library_path = path;
  return lib;
}

class ApplicationManagerTest : public ::testing::Test {
protected:
  ApplicationManagerTest() : logger(MakeTestLogger()) {
    // Legacy applications are looked up as <base>/<name>[.<version>]
    config.set("Worker__ApplicationBasePath", app_dir.path());
    symlink(FIXTURE_V1_PATH, app_dir.file("app.1").c_str());
    symlink(FIXTURE_V2_PATH, app_dir.file("app.2").c_str());
    symlink(FIXTURE_V1_PATH, app_dir.file("app").c_str());
    symlink(FIXTURE_MISSING_CALL_PATH, app_dir.file("broken.1").c_str());
    stub = server.NewStub();
  }

  /**
   * @brief Runs a method of the current service and returns its result (or error details)
   */
  std::string Run(ApplicationManager &manager, const std::string &method, const std::string &args = "") {
    TaskBuilder task;
    task.AddExpectedOutput("result");
    auto handler = task.Handler(*stub);
    const auto status = manager.Execute(*handler, method, args);
    return status.ok() ? task.Result("result") : "error: " + status.details();
  }

  TempDir app_dir;
  Configuration config;
  armonik::api::common::logger::Logger logger;
  FakeAgentServer server;
  std::unique_ptr<armonik::api::grpc::v1::agent::Agent::Stub> stub;
};
} // namespace

TEST_F(ApplicationManagerTest, DefaultsApplicationBasePathToData) {
  Configuration empty;
  ApplicationManager manager(empty, logger);
  try {
    manager.UseApplication(AppId{"no-such-app", "1"});
    FAIL() << "Expected ArmoniKSdkException";
  } catch (const ArmoniKSdkException &e) {
    EXPECT_NE(std::string(e.what()).find("/data/no-such-app.1"), std::string::npos) << e.what();
  }
}

TEST_F(ApplicationManagerTest, UseApplicationWithBadPathThrows) {
  ApplicationManager manager(config, logger);
  EXPECT_THROW(manager.UseApplication(AppId{"unknown", "1"}), ArmoniKSdkException);
}

TEST_F(ApplicationManagerTest, UseApplicationWithMissingSymbolThrows) {
  ApplicationManager manager(config, logger);
  try {
    manager.UseApplication(AppId{"broken", "1"});
    FAIL() << "Expected ArmoniKSdkException";
  } catch (const ArmoniKSdkException &e) {
    EXPECT_NE(std::string(e.what()).find("armonik_call"), std::string::npos) << e.what();
  }
}

TEST_F(ApplicationManagerTest, UseApplicationLoadsVersionedAndUnversionedLibraries) {
  ApplicationManager manager(config, logger);
  manager.UseApplication(AppId{"app", "2"}).UseService(ServiceId(AppId{"app", "2"}, "ns", "svc")).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v2");
  EXPECT_EQ(Run(manager, "service"), "ns::svc");
  EXPECT_EQ(Run(manager, "session"), "s");

  manager.UseApplication(AppId{"app", ""}).UseService(ServiceId(AppId{"app", ""}, "", "")).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v1");
}

TEST_F(ApplicationManagerTest, UseApplicationReloadsOnVersionChange) {
  ApplicationManager manager(config, logger);
  for (const auto &version : {"1", "2", "1"}) {
    const AppId app{"app", version};
    manager.UseApplication(app).UseService(ServiceId(app, "", "")).UseSession("s");
    EXPECT_EQ(Run(manager, "version"), std::string("v") + version);
  }
}

TEST_F(ApplicationManagerTest, ExecuteWithoutSessionThrows) {
  ApplicationManager manager(config, logger);
  manager.UseLibrary(Library(FIXTURE_V1_PATH));
  EXPECT_THROW(Run(manager, "version"), ArmoniKSdkException);
}

TEST_F(ApplicationManagerTest, UseLibraryWithBadPathThrows) {
  ApplicationManager manager(config, logger);
  EXPECT_THROW(manager.UseLibrary(Library("/nonexistent/libnothing.so")), ArmoniKSdkException);
}

TEST_F(ApplicationManagerTest, UseLibraryWithMissingSymbolThrows) {
  ApplicationManager manager(config, logger);
  try {
    manager.UseLibrary(Library(FIXTURE_MISSING_CALL_PATH));
    FAIL() << "Expected ArmoniKSdkException";
  } catch (const ArmoniKSdkException &e) {
    EXPECT_NE(std::string(e.what()).find("armonik_call"), std::string::npos) << e.what();
  }
}

TEST_F(ApplicationManagerTest, UseLibraryPassesServiceIdentity) {
  ApplicationManager manager(config, logger);
  manager.UseLibrary(Library(FIXTURE_V1_PATH), "ns", "svc").UseSession("s");
  EXPECT_EQ(Run(manager, "service"), "ns::svc");
  EXPECT_EQ(Run(manager, "echo", "payload"), "payload");
}

TEST_F(ApplicationManagerTest, UseLibraryReloadsOnPathChange) {
  ApplicationManager manager(config, logger);
  for (const auto &path : {FIXTURE_V1_PATH, FIXTURE_V2_PATH, FIXTURE_V1_PATH}) {
    manager.UseLibrary(Library(path)).UseSession("s");
    EXPECT_EQ(Run(manager, "version"), path == std::string(FIXTURE_V1_PATH) ? "v1" : "v2");
  }
}

TEST_F(ApplicationManagerTest, UseLibraryWithSameLibraryKeepsService) {
  ApplicationManager manager(config, logger);
  manager.UseLibrary(Library(FIXTURE_V1_PATH), "", "svc").UseSession("s1").UseSession("s2");
  EXPECT_EQ(Run(manager, "stats"), "entered=2,left=1");

  manager.UseLibrary(Library(FIXTURE_V1_PATH), "", "svc").UseSession("s2");
  EXPECT_EQ(Run(manager, "stats"), "entered=2,left=1");
}

TEST_F(ApplicationManagerTest, UseLibraryWithOtherServiceRecreatesService) {
  ApplicationManager manager(config, logger);
  manager.UseLibrary(Library(FIXTURE_V1_PATH), "ns", "a").UseSession("s");
  EXPECT_EQ(Run(manager, "service"), "ns::a");

  manager.UseLibrary(Library(FIXTURE_V1_PATH), "ns", "b").UseSession("s");
  EXPECT_EQ(Run(manager, "service"), "ns::b");
  EXPECT_EQ(Run(manager, "stats"), "entered=1,left=0");
}

TEST_F(ApplicationManagerTest, SwitchingFromLegacyToLibraryMode) {
  ApplicationManager manager(config, logger);
  manager.UseApplication(AppId{"app", "2"}).UseService(ServiceId(AppId{"app", "2"}, "", "")).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v2");

  manager.UseLibrary(Library(FIXTURE_V1_PATH)).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v1");

  manager.UseApplication(AppId{"app", "2"}).UseService(ServiceId(AppId{"app", "2"}, "", "")).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v2");
}

TEST_F(ApplicationManagerTest, UseLibraryRecoversAfterFailedLoad) {
  ApplicationManager manager(config, logger);
  manager.UseLibrary(Library(FIXTURE_V1_PATH)).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v1");

  EXPECT_THROW(manager.UseLibrary(Library("/nonexistent/libnothing.so")), ArmoniKSdkException);

  manager.UseLibrary(Library(FIXTURE_V1_PATH)).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v1");
}

TEST_F(ApplicationManagerTest, UseLibraryRecoversAfterMissingSymbol) {
  ApplicationManager manager(config, logger);
  manager.UseLibrary(Library(FIXTURE_V2_PATH)).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v2");

  EXPECT_THROW(manager.UseLibrary(Library(FIXTURE_MISSING_CALL_PATH)), ArmoniKSdkException);

  manager.UseLibrary(Library(FIXTURE_V2_PATH)).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v2");
}

TEST_F(ApplicationManagerTest, UseApplicationRecoversAfterMissingSymbol) {
  ApplicationManager manager(config, logger);
  const AppId app{"app", "2"};
  manager.UseApplication(app).UseService(ServiceId(app, "", "")).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v2");

  EXPECT_THROW(manager.UseApplication(AppId{"broken", "1"}), ArmoniKSdkException);

  manager.UseApplication(app).UseService(ServiceId(app, "", "")).UseSession("s");
  EXPECT_EQ(Run(manager, "version"), "v2");
}

TEST_F(ApplicationManagerTest, ExecuteWithMapsSendsConventionPayload) {
  ApplicationManager manager(config, logger);
  manager.UseLibrary(Library(FIXTURE_V1_PATH)).UseSession("s");
  TaskBuilder task;
  task.AddExpectedOutput("result");
  auto handler = task.Handler(*stub);

  const auto status = manager.Execute(*handler, "echo", {{"a", "1"}}, {{"out", "result"}});

  ASSERT_TRUE(status.ok()) << status.details();
  const auto echoed = nlohmann::json::parse(task.Result("result"));
  EXPECT_EQ(echoed.at("inputs").at("a"), "1");
  EXPECT_EQ(echoed.at("outputs").at("out"), "result");
}
