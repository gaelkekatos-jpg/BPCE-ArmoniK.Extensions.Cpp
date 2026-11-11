#include "ServiceManager.h"
#include "TestHelpers.h"
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <gtest/gtest.h>

using ArmoniK::Sdk::Common::ArmoniKSdkException;
using ArmoniK::Sdk::DynamicWorker::AppId;
using ArmoniK::Sdk::DynamicWorker::ArmoniKFunctionPointers;
using ArmoniK::Sdk::DynamicWorker::ServiceId;
using ArmoniK::Sdk::DynamicWorker::ServiceManager;
using ArmoniK::Sdk::Worker::UnitTest::FakeAgentServer;
using ArmoniK::Sdk::Worker::UnitTest::TaskBuilder;

namespace {
/**
 * @brief Records the calls made by the ServiceManager to the ArmoniK SDK interface
 */
struct Recorder {
  int created = 0;
  int destroyed = 0;
  std::vector<std::string> entered;
  int left = 0;
  std::string last_namespace;
  std::string last_name;
  std::string last_method;
  std::string last_input;
  // Behavior of the next call
  bool invoke_callback = true;
  armonik_status_t callback_status = ARMONIK_STATUS_OK;
  armonik_status_t return_status = ARMONIK_STATUS_OK;
  std::string output = "output";
};

Recorder recorder;
int service_token;

void *create_service(const char *service_namespace, const char *service_name) {
  recorder.created++;
  recorder.last_namespace = service_namespace;
  recorder.last_name = service_name;
  return &service_token;
}
void destroy_service(void *) { recorder.destroyed++; }
void *enter_session(void *, const char *session_id) {
  recorder.entered.emplace_back(session_id);
  return &recorder.entered.back();
}
void leave_session(void *, void *) { recorder.left++; }
armonik_status_t call(void *armonik_context, void *, void *, const char *function_name, const char *input,
                      size_t input_size, armonik_callback_t callback) {
  recorder.last_method = function_name;
  recorder.last_input.assign(input, input_size);
  if (recorder.invoke_callback) {
    callback(armonik_context, recorder.callback_status, recorder.output.data(), recorder.output.size());
  }
  return recorder.return_status;
}

const ArmoniKFunctionPointers pointers{create_service, destroy_service, enter_session, leave_session, call};
const ServiceId service_id(AppId{"app", "1.0"}, "ns", "svc");

class ServiceManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    recorder = Recorder();
    stub = server.NewStub();
    task.AddExpectedOutput("result-id");
  }

  FakeAgentServer server;
  std::unique_ptr<armonik::api::grpc::v1::agent::Agent::Stub> stub;
  TaskBuilder task;
};
} // namespace

TEST_F(ServiceManagerTest, CreatesServiceOnConstructionAndDestroysItOnDestruction) {
  {
    ServiceManager manager(pointers, service_id);
    EXPECT_EQ(recorder.created, 1);
    EXPECT_EQ(recorder.last_namespace, "ns");
    EXPECT_EQ(recorder.last_name, "svc");
    EXPECT_TRUE(manager.matches(service_id));
    EXPECT_FALSE(manager.matches(ServiceId(AppId{"app", "1.0"}, "ns", "other")));
    EXPECT_EQ(recorder.destroyed, 0);
  }
  EXPECT_EQ(recorder.destroyed, 1);
}

TEST_F(ServiceManagerTest, DefaultConstructedManagerDoesNothingOnDestruction) {
  {
    ServiceManager manager;
  }
  EXPECT_EQ(recorder.destroyed, 0);
}

TEST_F(ServiceManagerTest, UseSessionWithoutServiceThrows) {
  ServiceManager manager;
  EXPECT_THROW(manager.UseSession("s1"), ArmoniKSdkException);

  ServiceManager cleared(pointers, service_id);
  cleared.clear();
  EXPECT_THROW(cleared.UseSession("s1"), ArmoniKSdkException);
  EXPECT_TRUE(recorder.entered.empty());
}

TEST_F(ServiceManagerTest, UseSessionEntersOnceAndSwitchesSessions) {
  {
    ServiceManager manager(pointers, service_id);
    manager.UseSession("s1").UseSession("s1");
    EXPECT_EQ(recorder.entered, std::vector<std::string>({"s1"}));
    EXPECT_EQ(recorder.left, 0);

    manager.UseSession("s2");
    EXPECT_EQ(recorder.entered, std::vector<std::string>({"s1", "s2"}));
    EXPECT_EQ(recorder.left, 1);
  }
  EXPECT_EQ(recorder.left, 2);
  EXPECT_EQ(recorder.destroyed, 1);
}

TEST_F(ServiceManagerTest, ClearLeavesSessionAndDestroysServiceOnce) {
  ServiceManager manager(pointers, service_id);
  manager.UseSession("s1");
  manager.clear();
  EXPECT_EQ(recorder.left, 1);
  EXPECT_EQ(recorder.destroyed, 1);
  manager.clear();
  EXPECT_EQ(recorder.destroyed, 1);
  EXPECT_FALSE(manager.matches(service_id));
}

TEST_F(ServiceManagerTest, MoveTransfersServiceOwnership) {
  {
    ServiceManager source(pointers, service_id);
    ServiceManager moved(std::move(source));
    EXPECT_FALSE(source.matches(service_id));
    EXPECT_TRUE(moved.matches(service_id));

    ServiceManager assigned;
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.matches(service_id));
    EXPECT_EQ(recorder.destroyed, 0);
  }
  EXPECT_EQ(recorder.created, 1);
  EXPECT_EQ(recorder.destroyed, 1);
}

TEST_F(ServiceManagerTest, ExecuteWithoutSessionThrows) {
  ServiceManager manager(pointers, service_id);
  auto handler = task.Handler(*stub);
  EXPECT_THROW(manager.Execute(*handler, "method", "args"), ArmoniKSdkException);
}

TEST_F(ServiceManagerTest, ExecuteSuccessSendsResultToAgent) {
  ServiceManager manager(pointers, service_id);
  manager.UseSession("session");
  auto handler = task.Handler(*stub);
  recorder.output = "computed";

  const auto status = manager.Execute(*handler, "method", std::string("a\0b", 3));

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(recorder.last_method, "method");
  EXPECT_EQ(recorder.last_input, std::string("a\0b", 3));
  EXPECT_EQ(task.Result("result-id"), "computed");
  EXPECT_EQ(server.service().NotifiedResultIds(), std::vector<std::string>({"result-id"}));
  EXPECT_EQ(server.service().NotifiedSessionIds(), std::vector<std::string>({"session"}));
  EXPECT_EQ(server.service().CommunicationTokens(), std::vector<std::string>({"token"}));
}

TEST_F(ServiceManagerTest, ExecuteErrorReturnsFailedStatusWithMessage) {
  ServiceManager manager(pointers, service_id);
  manager.UseSession("session");
  auto handler = task.Handler(*stub);
  recorder.callback_status = ARMONIK_STATUS_ERROR;
  recorder.return_status = ARMONIK_STATUS_ERROR;
  recorder.output = "bad input";

  const auto status = manager.Execute(*handler, "method", "");

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.details(), "bad input");
  EXPECT_TRUE(server.service().NotifiedResultIds().empty());
}

TEST_F(ServiceManagerTest, ExecuteErrorWithoutCallbackReturnsGenericMessage) {
  ServiceManager manager(pointers, service_id);
  manager.UseSession("session");
  auto handler = task.Handler(*stub);
  recorder.invoke_callback = false;
  recorder.return_status = ARMONIK_STATUS_ERROR;

  const auto status = manager.Execute(*handler, "method", "");

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.details(), "Unknown error in worker, check logs.");
}

TEST_F(ServiceManagerTest, ExecuteRetryThrowsTransientError) {
  ServiceManager manager(pointers, service_id);
  manager.UseSession("session");
  auto handler = task.Handler(*stub);
  recorder.callback_status = ARMONIK_STATUS_RETRY;
  recorder.return_status = ARMONIK_STATUS_RETRY;
  recorder.output = "try again";

  try {
    manager.Execute(*handler, "method", "");
    FAIL() << "Expected std::runtime_error";
  } catch (const ArmoniKSdkException &) {
    FAIL() << "Retry must not be reported as a permanent ArmoniKSdkException";
  } catch (const std::runtime_error &e) {
    EXPECT_STREQ(e.what(), "try again");
  }
  EXPECT_TRUE(server.service().NotifiedResultIds().empty());
}
