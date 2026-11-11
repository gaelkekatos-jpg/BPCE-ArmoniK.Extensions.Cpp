#include "TestHelpers.h"
#include <armonik/sdk/worker/ConventionExecutor.h>
#include <armonik/sdk/worker/ServiceBase.h>
#include <gtest/gtest.h>
#include <stdexcept>

using ArmoniK::Sdk::Common::ArmoniKSdkException;
using ArmoniK::Sdk::Common::TaskOptions;
using ArmoniK::Sdk::Worker::ConventionExecutor;
using ArmoniK::Sdk::Worker::ServiceBase;
using ArmoniK::Sdk::Worker::UnitTest::EmptyTaskOptions;
using ArmoniK::Sdk::Worker::UnitTest::FakeAgentServer;
using ArmoniK::Sdk::Worker::UnitTest::TaskBuilder;

namespace {
class ConcatService : public ServiceBase {
public:
  using ServiceBase::call;

  void *enter_session(const char *session_id) override {
    sessions.emplace_back(session_id);
    return &sessions.back();
  }
  void leave_session(void *session_ctx) override { left.push_back(*static_cast<std::string *>(session_ctx)); }

  std::string call(void *session_ctx, const std::string &name,
                   const std::map<std::string, std::string> &inputs) override {
    if (name == "permanent") {
      throw ArmoniKSdkException("permanent failure");
    }
    if (name == "transient") {
      throw std::runtime_error("transient failure");
    }
    std::string output = name + "@" + *static_cast<std::string *>(session_ctx);
    for (const auto &input : inputs) {
      output += ":" + input.first + "=" + input.second;
    }
    return output;
  }

  std::vector<std::string> sessions;
  std::vector<std::string> left;
};

TaskOptions Convention(const std::string &symbol) {
  auto options = EmptyTaskOptions();
  options.SetConvention(symbol);
  return options;
}

class ConventionExecutorTest : public ::testing::Test {
protected:
  void SetUp() override {
    service.sessions.reserve(16);
    stub = server.NewStub();
  }

  armonik::api::worker::ProcessStatus Run(ConventionExecutor &executor, const TaskBuilder &task) {
    auto handler = task.Handler(*stub);
    return executor.Execute(*handler);
  }

  ConcatService service;
  FakeAgentServer server;
  std::unique_ptr<armonik::api::grpc::v1::agent::Agent::Stub> stub;
};
} // namespace

TEST_F(ConventionExecutorTest, CallsServiceAndSendsFirstExpectedResult) {
  ConventionExecutor executor(service);
  TaskBuilder task;
  task.SetTaskOptions(Convention("concat"))
      .SetConventionPayload({{"inline", "value"}, {"blob", "dep-id"}}, {{"out", "result-1"}})
      .AddDataDependency("dep-id", "downloaded")
      .AddExpectedOutput("result-1")
      .AddExpectedOutput("result-2");

  const auto status = Run(executor, task);

  ASSERT_TRUE(status.ok()) << status.details();
  EXPECT_EQ(task.Result("result-1"), "concat@session:blob=downloaded:inline=value");
  EXPECT_EQ(server.service().NotifiedResultIds(), std::vector<std::string>({"result-1"}));
  EXPECT_EQ(server.service().NotifiedSessionIds(), std::vector<std::string>({"session"}));
}

TEST_F(ConventionExecutorTest, ReusesSessionAndSwitchesWhenItChanges) {
  {
    ConventionExecutor executor(service);
    for (const auto &session : {"s1", "s1", "s2"}) {
      TaskBuilder task;
      task.SetSession(session).SetTaskOptions(Convention("m")).SetConventionPayload({}, {}).AddExpectedOutput("result");
      const auto status = Run(executor, task);
      ASSERT_TRUE(status.ok()) << status.details();
      EXPECT_EQ(task.Result("result"), std::string("m@") + session);
    }
    EXPECT_EQ(service.sessions, std::vector<std::string>({"s1", "s2"}));
    EXPECT_EQ(service.left, std::vector<std::string>({"s1"}));
  }
  // The executor leaves the current session on destruction
  EXPECT_EQ(service.left, std::vector<std::string>({"s1", "s2"}));
}

TEST_F(ConventionExecutorTest, NonConventionTaskFails) {
  ConventionExecutor executor(service);
  TaskBuilder task;
  task.SetTaskOptions(EmptyTaskOptions()).AddExpectedOutput("result");

  const auto status = Run(executor, task);

  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.details().find("Not a convention task"), std::string::npos) << status.details();
  EXPECT_TRUE(service.sessions.empty());
}

TEST_F(ConventionExecutorTest, UnsupportedConventionVersionFails) {
  ConventionExecutor executor(service);
  auto options = Convention("m");
  options.options[ArmoniK::Sdk::Common::DynamicLibrary::KeyConventionVersion] = "v0";
  TaskBuilder task;
  task.SetTaskOptions(options).SetConventionPayload({}, {}).AddExpectedOutput("result");

  const auto status = Run(executor, task);

  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.details().find("Unsupported convention version: v0"), std::string::npos) << status.details();
}

TEST_F(ConventionExecutorTest, MissingSymbolFails) {
  ConventionExecutor executor(service);
  auto options = EmptyTaskOptions();
  options.options[ArmoniK::Sdk::Common::DynamicLibrary::KeyConventionVersion] =
      ArmoniK::Sdk::Common::DynamicLibrary::ConventionVersion;
  TaskBuilder task;
  task.SetTaskOptions(options).SetConventionPayload({}, {}).AddExpectedOutput("result");

  EXPECT_FALSE(Run(executor, task).ok());
}

TEST_F(ConventionExecutorTest, MalformedPayloadFails) {
  ConventionExecutor executor(service);
  TaskBuilder task;
  task.SetTaskOptions(Convention("m")).SetPayload("{not json").AddExpectedOutput("result");

  const auto status = Run(executor, task);

  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(server.service().NotifiedResultIds().empty());
}

TEST_F(ConventionExecutorTest, NoExpectedResultFails) {
  ConventionExecutor executor(service);
  TaskBuilder task;
  task.SetTaskOptions(Convention("m")).SetConventionPayload({}, {});

  const auto status = Run(executor, task);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.details(), "Convention task has no expected result");
}

TEST_F(ConventionExecutorTest, ArmoniKSdkExceptionBecomesFailedStatus) {
  ConventionExecutor executor(service);
  TaskBuilder task;
  task.SetTaskOptions(Convention("permanent")).SetConventionPayload({}, {}).AddExpectedOutput("result");

  const auto status = Run(executor, task);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.details(), "permanent failure");
  EXPECT_TRUE(server.service().NotifiedResultIds().empty());
}

TEST_F(ConventionExecutorTest, OtherExceptionsPropagateForRetry) {
  ConventionExecutor executor(service);
  TaskBuilder task;
  task.SetTaskOptions(Convention("transient")).SetConventionPayload({}, {}).AddExpectedOutput("result");

  EXPECT_THROW(Run(executor, task), std::runtime_error);
}

TEST_F(ConventionExecutorTest, AgentErrorOnResultUploadPropagates) {
  ConventionExecutor executor(service);
  server.service().FailNotifyWith(::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "agent down"));
  TaskBuilder task;
  task.SetTaskOptions(Convention("m")).SetConventionPayload({}, {}).AddExpectedOutput("result");

  try {
    Run(executor, task);
    FAIL() << "Expected the agent error to propagate";
  } catch (const ArmoniKSdkException &) {
    FAIL() << "An agent error must not be reported as a permanent task failure";
  } catch (const std::exception &e) {
    EXPECT_NE(std::string(e.what()).find("agent down"), std::string::npos) << e.what();
  }
}
