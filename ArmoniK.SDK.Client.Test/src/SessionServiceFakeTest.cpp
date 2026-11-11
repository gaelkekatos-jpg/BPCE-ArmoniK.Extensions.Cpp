/*
 * SessionService tests against an in-process fake ArmoniK control plane (see fakes/FakeServices.h).
 *
 * No cluster is needed: each test starts its own gRPC server on an ephemeral 127.0.0.1 port and points
 * GrpcClient__Endpoint at it programmatically, so the SDK goes through its real ChannelPool/ChannelFactory
 * (including the gRPC retry policy built from GrpcClient__MaxAttempts / GrpcClient__RequestTimeout).
 */
#include <gtest/gtest.h>

#include <armonik/common/exceptions/ArmoniKApiException.h>
#include <armonik/common/exceptions/ArmoniKTaskError.h>
#include <armonik/common/logger/formatter.h>
#include <armonik/common/logger/logger.h>
#include <armonik/common/logger/writer.h>
#include <armonik/sdk/client/IServiceInvocationHandler.h>
#include <armonik/sdk/client/SessionService.h>
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/common/BlobDefinition.h>
#include <armonik/sdk/common/Configuration.h>
#include <armonik/sdk/common/Diagnostics.h>
#include <armonik/sdk/common/Properties.h>
#include <armonik/sdk/common/TaskDefinition.h>
#include <armonik/sdk/common/TaskOptions.h>
#include <armonik/sdk/common/TaskPayload.h>
#include <armonik/sdk/common/internal/ConventionPayload.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fakes/FakeServices.h"

ARMONIK_SDK_SUPPRESS_DEPRECATED_BEGIN

namespace {

namespace Fakes = ArmoniK::Sdk::Client::Test::Fakes;
namespace Rpc = ArmoniK::Sdk::Client::Test::Fakes::Rpc;
using armonik::api::common::exceptions::ArmoniKApiException;
using armonik::api::common::exceptions::ArmoniKTaskError;
using ArmoniK::Sdk::Client::SessionService;
using ArmoniK::Sdk::Client::WaitOptions;
using ArmoniK::Sdk::Common::TaskPayload;

constexpr int kGrpcMaxAttempts = 3;
constexpr int kDownloadMaxRetry = 3;

class RecordingHandler : public ArmoniK::Sdk::Client::IServiceInvocationHandler {
public:
  struct Error {
    std::string task_id;
    std::string what;
    bool is_task_error = false;
    std::string failed_task_id;
    std::vector<std::pair<std::string, std::string>> details;
  };

  void HandleResponse(std::string &&result_payload, armonik::api::string_view taskId,
                      armonik::api::string_view result_id) override {
    (void)result_id;
    std::lock_guard<std::mutex> _(mutex_);
    responses_[static_cast<std::string>(taskId)] = std::move(result_payload);
  }

  void HandleError(const std::exception &e, armonik::api::string_view taskId) override {
    Error error;
    error.task_id = static_cast<std::string>(taskId);
    error.what = e.what();
    if (auto task_error = dynamic_cast<const ArmoniKTaskError *>(&e)) {
      ArmoniKTaskError copy(*task_error);
      error.is_task_error = true;
      error.failed_task_id = copy.taskId();
      error.details = copy.error_details();
    }
    std::lock_guard<std::mutex> _(mutex_);
    errors_.push_back(std::move(error));
  }

  std::map<std::string, std::string> responses() const {
    std::lock_guard<std::mutex> _(mutex_);
    return responses_;
  }
  std::vector<Error> errors() const {
    std::lock_guard<std::mutex> _(mutex_);
    return errors_;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::string> responses_;
  std::vector<Error> errors_;
};

/**
 * @brief Fake server + SDK configuration pointing at it. Shared by all the fixtures below.
 */
struct FakeEnvironment {
  Fakes::FakeArmoniKServer server;
  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Error};
  ArmoniK::Sdk::Common::Configuration config;
  ArmoniK::Sdk::Common::TaskOptions task_options{"FakeApp", "1.0.0", "FakeNamespace", "FakeService", "fake-partition"};

  FakeEnvironment() {
    config.set("GrpcClient__Endpoint", server.endpoint());
    config.set("GrpcClient__MaxAttempts", std::to_string(kGrpcMaxAttempts));
    config.set("GrpcClient__BackoffMultiplier", "1");
    config.set("GrpcClient__InitialBackOff", "00:00:00.010");
    config.set("GrpcClient__MaxBackOff", "00:00:00.020");
    config.set("GrpcClient__RequestTimeout", "00:00:10");
    config.set("GrpcClient__ThreadPoolSize", "4");
    config.set("GrpcClient__DownloadMaxRetry", std::to_string(kDownloadMaxRetry));
  }

  Fakes::FakeState &state() { return server.state(); }
  ArmoniK::Sdk::Common::Properties properties() const { return ArmoniK::Sdk::Common::Properties{config, task_options}; }
};

WaitOptions FastPolling(unsigned int timeout_ms = 10000) {
  WaitOptions options;
  options.polling_ms = 10;
  options.timeout = timeout_ms;
  return options;
}

const Fakes::FakeTask *FindTask(const std::vector<Fakes::FakeTask> &tasks, const std::string &task_id) {
  for (const auto &task : tasks) {
    if (task.task_id == task_id) {
      return &task;
    }
  }
  return nullptr;
}

class SessionServiceFakeTest : public ::testing::Test {
protected:
  FakeEnvironment env;
};

TEST_F(SessionServiceFakeTest, SubmitHappyPath) {
  SessionService service(env.properties(), env.logger);

  ASSERT_EQ(env.state().CallCount(Rpc::CreateSession), 1);
  auto create_requests = env.state().CreateSessionRequests();
  ASSERT_EQ(create_requests.size(), 1u);
  ASSERT_EQ(create_requests[0].partition_ids_size(), 1);
  EXPECT_EQ(create_requests[0].partition_ids(0), "fake-partition");
  EXPECT_EQ(create_requests[0].default_task_option().application_name(), "FakeApp");
  EXPECT_FALSE(service.getSession().empty());

  auto handler = std::make_shared<RecordingHandler>();
  std::vector<TaskPayload> payloads{TaskPayload("Echo", "first"), TaskPayload("Echo", "second"),
                                    TaskPayload("Echo", "third", {"some-dependency"})};
  auto task_ids = service.Submit(payloads, handler);

  ASSERT_EQ(task_ids.size(), payloads.size());
  EXPECT_EQ(std::set<std::string>(task_ids.begin(), task_ids.end()).size(), payloads.size());

  auto tasks = env.state().TasksOfSession(service.getSession());
  ASSERT_EQ(tasks.size(), payloads.size());
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    auto task = FindTask(tasks, task_ids[i]);
    ASSERT_NE(task, nullptr) << task_ids[i];
    EXPECT_EQ(task->payload, payloads[i].Serialize());
    EXPECT_EQ(task->expected_output_ids.size(), 1u);
    EXPECT_EQ(task->options.application_name(), "FakeApp");
    EXPECT_EQ(task->options.partition_id(), "fake-partition");
  }
  EXPECT_EQ(FindTask(tasks, task_ids[2])->data_dependencies, std::vector<std::string>{"some-dependency"});

  // Small payloads are created inline with CreateResults; only outputs need metadata, nothing is streamed
  EXPECT_EQ(env.state().CallCount(Rpc::GetServiceConfiguration), 1);
  EXPECT_GE(env.state().CallCount(Rpc::CreateResults), 1);
  EXPECT_GE(env.state().CallCount(Rpc::CreateResultsMetaData), 1);
  EXPECT_EQ(env.state().CallCount(Rpc::UploadResultData), 0);
  EXPECT_GE(env.state().CallCount(Rpc::SubmitTasks), 1);

  service.WaitResults({}, ArmoniK::Sdk::Client::All, FastPolling());

  // The default fake task handler echoes the payload into the output
  auto responses = handler->responses();
  EXPECT_TRUE(handler->errors().empty());
  ASSERT_EQ(responses.size(), payloads.size());
  for (std::size_t i = 0; i < payloads.size(); ++i) {
    EXPECT_EQ(responses[task_ids[i]], payloads[i].Serialize());
  }
  EXPECT_EQ(env.state().CallCount(Rpc::DownloadResultData), static_cast<int>(payloads.size()));
}

TEST_F(SessionServiceFakeTest, SubmitTaskDefinitionStreamsLargeInputs) {
  env.state().SetDataChunkMaxSize(1024);
  auto &state = env.state();
  env.state().SetTaskHandler([&state](const Fakes::FakeTask &task) {
    auto payload = ArmoniK::Sdk::Common::ConventionPayload::Deserialize(task.payload);
    Fakes::TaskOutcome outcome;
    outcome.output = std::to_string(state.ResultData(payload.inputs.at("large")).size()) + ":" +
                     state.ResultData(payload.inputs.at("small"));
    return outcome;
  });

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  const std::string large(5000, 'x');
  auto task_ids = service.Submit(
      {ArmoniK::Sdk::Common::TaskDefinition({{"small", ArmoniK::Sdk::Common::BlobDefinition::FromData("tiny")},
                                             {"large", ArmoniK::Sdk::Common::BlobDefinition::FromData(large)}})},
      handler);
  ASSERT_EQ(task_ids.size(), 1u);

  // The large input goes through CreateResultsMetaData + chunked UploadResultData, the small one inline
  EXPECT_EQ(env.state().CallCount(Rpc::UploadResultData), 1);
  EXPECT_GE(env.state().CallCount(Rpc::CreateResults), 1);

  auto tasks = env.state().Tasks();
  ASSERT_EQ(tasks.size(), 1u);
  auto payload = ArmoniK::Sdk::Common::ConventionPayload::Deserialize(tasks[0].payload);
  ASSERT_EQ(payload.inputs.size(), 2u);
  EXPECT_EQ(env.state().ResultData(payload.inputs["large"]), large);
  EXPECT_EQ(env.state().ResultData(payload.inputs["small"]), "tiny");
  std::set<std::string> deps(tasks[0].data_dependencies.begin(), tasks[0].data_dependencies.end());
  EXPECT_EQ(deps, (std::set<std::string>{payload.inputs["large"], payload.inputs["small"]}));

  service.WaitResults({}, ArmoniK::Sdk::Client::All, FastPolling());
  EXPECT_TRUE(handler->errors().empty());
  EXPECT_EQ(handler->responses()[task_ids[0]], "5000:tiny");
}

TEST_F(SessionServiceFakeTest, WaitResultsPollsUntilResultIsReady) {
  env.state().SetTaskHandler([](const Fakes::FakeTask &) {
    Fakes::TaskOutcome outcome;
    outcome.output = "done";
    outcome.ready_after_polls = 3;
    return outcome;
  });

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  auto task_ids = service.Submit({TaskPayload("Echo", "x")}, handler);

  service.WaitResults({}, ArmoniK::Sdk::Client::All, FastPolling());

  // 3 polls see CREATED, the 4th sees COMPLETED
  EXPECT_EQ(env.state().CallCount(Rpc::ListResults), 4);
  EXPECT_EQ(handler->responses()[task_ids[0]], "done");
  EXPECT_EQ(env.state().CallCount(Rpc::DownloadResultData), 1);
}

TEST_F(SessionServiceFakeTest, WaitResultsTimeoutKeepsPendingResults) {
  env.state().SetTaskHandler([](const Fakes::FakeTask &) {
    Fakes::TaskOutcome outcome;
    outcome.kind = Fakes::TaskOutcome::Pending;
    return outcome;
  });

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  auto task_ids = service.Submit({TaskPayload("Echo", "x")}, handler);

  WaitOptions options;
  options.polling_ms = 20;
  options.timeout = 200;
  auto start = std::chrono::steady_clock::now();
  service.WaitResults({}, ArmoniK::Sdk::Client::All, options);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  EXPECT_GE(elapsed.count(), 200);
  EXPECT_LT(elapsed.count(), 5000);
  EXPECT_TRUE(handler->responses().empty());
  EXPECT_TRUE(handler->errors().empty());
  EXPECT_GE(env.state().CallCount(Rpc::ListResults), 3);
  EXPECT_EQ(env.state().CallCount(Rpc::DownloadResultData), 0);

  // The result is still tracked: a later wait delivers it once the control plane completes it
  env.state().CompleteResult(env.state().Tasks()[0].expected_output_ids[0], "late");
  service.WaitResults({task_ids[0]}, ArmoniK::Sdk::Client::All, FastPolling());
  EXPECT_EQ(handler->responses()[task_ids[0]], "late");
}

TEST_F(SessionServiceFakeTest, WaitAnyReturnsOnFirstCompletion) {
  env.state().SetTaskHandler([](const Fakes::FakeTask &task) {
    Fakes::TaskOutcome outcome;
    outcome.kind = TaskPayload::Deserialize(task.payload).method_name == "fast" ? Fakes::TaskOutcome::Complete
                                                                                : Fakes::TaskOutcome::Pending;
    outcome.output = "fast-output";
    return outcome;
  });

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  auto task_ids = service.Submit({TaskPayload("fast", "x"), TaskPayload("slow", "y")}, handler);

  service.WaitResults({}, ArmoniK::Sdk::Client::Any, FastPolling());
  auto responses = handler->responses();
  ASSERT_EQ(responses.size(), 1u);
  EXPECT_EQ(responses[task_ids[0]], "fast-output");

  env.state().CompleteResult(FindTask(env.state().Tasks(), task_ids[1])->expected_output_ids[0], "slow-output");
  service.WaitResults({task_ids[1]}, ArmoniK::Sdk::Client::All, FastPolling());
  EXPECT_EQ(handler->responses()[task_ids[1]], "slow-output");
}

/*
 * Mirrors ExceptionServiceTest (ArmoniK.SDK.Client.Test.cpp). Retrying a task after a transient
 * std::runtime_error, and not retrying after a permanent ArmoniKSdkException, is decided worker-side and by
 * the control plane: the client never sees the intermediate attempts. The fake therefore models that
 * control-plane rule (it runs a "worker" that throws the real exception types and retries up to the
 * max_retries the client sent) and the test asserts what the client does observe: the max_retries it
 * forwards, a single HandleError carrying an ArmoniKTaskError for the last attempt, and the GetTask lookup
 * used to build it.
 */
struct ExceptionTestParam {
  std::string method_name;
  int max_retries;
  // Expected number of task attempts recorded by the (fake) control plane: 1 + actual retries performed.
  int expected_task_count;
};

class SessionServiceFakeExceptionTest : public ::testing::TestWithParam<ExceptionTestParam> {
protected:
  FakeEnvironment env;
};

void FakeWorker(const std::string &method_name) {
  if (method_name == "sdkError") {
    throw ArmoniK::Sdk::Common::ArmoniKSdkException("Permanent failure from sdkError");
  }
  if (method_name == "retry") {
    throw std::runtime_error("Transient failure from retry");
  }
}

TEST_P(SessionServiceFakeExceptionTest, HandlesExceptionCases) {
  const auto &param = GetParam();
  env.task_options.max_retries = param.max_retries;
  env.state().SetTaskHandler([](const Fakes::FakeTask &task) {
    Fakes::TaskOutcome outcome;
    outcome.kind = Fakes::TaskOutcome::Abort;
    outcome.final_status = armonik::api::grpc::v1::task_status::TASK_STATUS_ERROR;
    try {
      FakeWorker(TaskPayload::Deserialize(task.payload).method_name);
      outcome.kind = Fakes::TaskOutcome::Complete;
    } catch (const ArmoniK::Sdk::Common::ArmoniKSdkException &e) {
      outcome.attempts = 1;
      outcome.error_detail = e.what();
    } catch (const std::exception &e) {
      outcome.attempts = task.options.max_retries() + 1;
      outcome.error_detail = e.what();
    }
    return outcome;
  });

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  auto task_ids = service.Submit({TaskPayload(param.method_name, "Exception success")}, handler);
  ASSERT_EQ(task_ids.size(), 1u);

  service.WaitResults({}, ArmoniK::Sdk::Client::All, FastPolling());

  // The client forwards max_retries to the control plane
  auto submits = env.state().SubmitTasksRequests();
  ASSERT_EQ(submits.size(), 1u);
  EXPECT_EQ(submits[0].task_options().max_retries(), param.max_retries);

  EXPECT_TRUE(handler->responses().empty());
  auto errors = handler->errors();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].task_id, task_ids[0]);
  ASSERT_TRUE(errors[0].is_task_error) << errors[0].what;

  auto tasks = env.state().TasksOfSession(service.getSession());
  ASSERT_EQ(static_cast<int>(tasks.size()), param.expected_task_count);
  const auto &last_attempt = tasks.back();
  EXPECT_EQ(errors[0].failed_task_id, last_attempt.task_id);
  EXPECT_EQ(env.state().GetTaskIds(), std::vector<std::string>{last_attempt.task_id});
  ASSERT_EQ(errors[0].details.size(), 1u);
  EXPECT_EQ(errors[0].details[0].first, "TASK_STATUS_ERROR");
  EXPECT_EQ(errors[0].details[0].second,
            param.method_name == "sdkError" ? "Permanent failure from sdkError" : "Transient failure from retry");
}

// Both cases use max_retries=2 to prove the distinction:
// "sdkError": ArmoniKSdkException -> permanent failure -> 1 task attempt despite retries being allowed.
// "retry": std::runtime_error -> transient failure -> 3 task attempts (original + 2 retries exhausted).
INSTANTIATE_TEST_SUITE_P(ExceptionCases, SessionServiceFakeExceptionTest,
                         ::testing::Values(ExceptionTestParam{"sdkError", 2, 1}, ExceptionTestParam{"retry", 2, 3}));

/*
 * gRPC status injection. UNAVAILABLE is in the retryableStatusCodes of the service config built by
 * ArmoniK.Api's ChannelFactory, so gRPC retries it transparently up to GrpcClient__MaxAttempts;
 * DEADLINE_EXCEEDED is not, so it surfaces immediately as an ArmoniKApiException. Downloads are additionally
 * retried by the SDK itself (GrpcClient__DownloadMaxRetry), whatever the status.
 */
enum class Expect { Success, Throws, HandlerError };

struct GrpcErrorParam {
  std::string name;
  std::string rpc;
  grpc::StatusCode code;
  int times;
  Expect expect;
  int expected_calls;
};

void PrintTo(const GrpcErrorParam &param, std::ostream *os) { *os << param.name; }

class SessionServiceFakeGrpcErrorTest : public ::testing::TestWithParam<GrpcErrorParam> {
protected:
  FakeEnvironment env;
};

TEST_P(SessionServiceFakeGrpcErrorTest, InjectedStatus) {
  const auto &param = GetParam();
  env.state().InjectError(param.rpc, grpc::Status(param.code, "injected " + param.name), param.times);

  auto handler = std::make_shared<RecordingHandler>();
  std::vector<std::string> task_ids;
  auto run = [&]() {
    SessionService service(env.properties(), env.logger);
    task_ids = service.Submit({TaskPayload("Echo", "payload")}, handler);
    service.WaitResults({}, ArmoniK::Sdk::Client::All, FastPolling());
  };

  switch (param.expect) {
  case Expect::Success:
    ASSERT_NO_THROW(run());
    ASSERT_EQ(task_ids.size(), 1u);
    EXPECT_EQ(handler->responses()[task_ids[0]], TaskPayload("Echo", "payload").Serialize());
    EXPECT_TRUE(handler->errors().empty());
    break;
  case Expect::Throws:
    try {
      run();
      FAIL() << "Expected an ArmoniKApiException";
    } catch (const ArmoniKApiException &e) {
      EXPECT_NE(std::string(e.what()).find("injected " + param.name), std::string::npos) << e.what();
    }
    EXPECT_TRUE(handler->responses().empty());
    break;
  case Expect::HandlerError: {
    ASSERT_NO_THROW(run());
    EXPECT_TRUE(handler->responses().empty());
    auto errors = handler->errors();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors[0].what.find("injected " + param.name), std::string::npos) << errors[0].what;
    break;
  }
  }
  EXPECT_EQ(env.state().CallCount(param.rpc), param.expected_calls);
}

std::string GrpcErrorParamName(const ::testing::TestParamInfo<GrpcErrorParam> &info) { return info.param.name; }

INSTANTIATE_TEST_SUITE_P(
    GrpcErrors, SessionServiceFakeGrpcErrorTest,
    ::testing::Values(GrpcErrorParam{"CreateSessionUnavailableRetried", Rpc::CreateSession,
                                     grpc::StatusCode::UNAVAILABLE, 1, Expect::Success, 2},
                      GrpcErrorParam{"CreateSessionDeadlineExceeded", Rpc::CreateSession,
                                     grpc::StatusCode::DEADLINE_EXCEEDED, 1, Expect::Throws, 1},
                      GrpcErrorParam{"GetServiceConfigurationUnavailableRetried", Rpc::GetServiceConfiguration,
                                     grpc::StatusCode::UNAVAILABLE, 1, Expect::Success, 2},
                      GrpcErrorParam{"CreateResultsUnavailableRetried", Rpc::CreateResults,
                                     grpc::StatusCode::UNAVAILABLE, 1, Expect::Success, 2},
                      GrpcErrorParam{"CreateResultsDeadlineExceeded", Rpc::CreateResults,
                                     grpc::StatusCode::DEADLINE_EXCEEDED, 1, Expect::Throws, 1},
                      GrpcErrorParam{"SubmitTasksUnavailableRetried", Rpc::SubmitTasks, grpc::StatusCode::UNAVAILABLE,
                                     kGrpcMaxAttempts - 1, Expect::Success, kGrpcMaxAttempts},
                      GrpcErrorParam{"SubmitTasksUnavailableExhausted", Rpc::SubmitTasks, grpc::StatusCode::UNAVAILABLE,
                                     kGrpcMaxAttempts, Expect::Throws, kGrpcMaxAttempts},
                      GrpcErrorParam{"SubmitTasksDeadlineExceeded", Rpc::SubmitTasks,
                                     grpc::StatusCode::DEADLINE_EXCEEDED, 1, Expect::Throws, 1},
                      GrpcErrorParam{"ListResultsUnavailableRetried", Rpc::ListResults, grpc::StatusCode::UNAVAILABLE,
                                     1, Expect::Success, 2},
                      GrpcErrorParam{"ListResultsDeadlineExceeded", Rpc::ListResults,
                                     grpc::StatusCode::DEADLINE_EXCEEDED, 1, Expect::Throws, 1},
                      GrpcErrorParam{"DownloadUnavailableRetried", Rpc::DownloadResultData,
                                     grpc::StatusCode::UNAVAILABLE, 1, Expect::Success, 2},
                      GrpcErrorParam{"DownloadDeadlineExceededRetriedBySdk", Rpc::DownloadResultData,
                                     grpc::StatusCode::DEADLINE_EXCEEDED, kDownloadMaxRetry - 1, Expect::Success,
                                     kDownloadMaxRetry},
                      GrpcErrorParam{"DownloadDeadlineExceededExhausted", Rpc::DownloadResultData,
                                     grpc::StatusCode::DEADLINE_EXCEEDED, kDownloadMaxRetry, Expect::HandlerError,
                                     kDownloadMaxRetry}),
    GrpcErrorParamName);

TEST_F(SessionServiceFakeTest, SlowServerHitsRequestTimeout) {
  env.config.set("GrpcClient__RequestTimeout", "00:00:02");
  env.state().SetDelay(Rpc::SubmitTasks, std::chrono::milliseconds(10000));

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  auto start = std::chrono::steady_clock::now();
  try {
    service.Submit({TaskPayload("Echo", "x")}, handler);
    FAIL() << "Expected an ArmoniKApiException";
  } catch (const ArmoniKApiException &e) {
    EXPECT_NE(std::string(e.what()).find("Deadline"), std::string::npos) << e.what();
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  EXPECT_LT(elapsed.count(), 5000);
  // A deadline is not retried
  EXPECT_EQ(env.state().CallCount(Rpc::SubmitTasks), 1);
}

TEST_F(SessionServiceFakeTest, DownloadInterruptedMidStreamIsRetriedBySdk) {
  // gRPC does not retry once response data has been received, the SDK re-issues the download from scratch
  const std::string output(100, 'z');
  env.state().SetDownloadChunkSize(16);
  env.state().SetTaskHandler([&output](const Fakes::FakeTask &) {
    Fakes::TaskOutcome outcome;
    outcome.output = output;
    return outcome;
  });
  env.state().InjectMidStreamError(grpc::Status(grpc::StatusCode::UNAVAILABLE, "connection dropped"));

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  auto task_ids = service.Submit({TaskPayload("Echo", "x")}, handler);
  service.WaitResults({}, ArmoniK::Sdk::Client::All, FastPolling());

  EXPECT_TRUE(handler->errors().empty());
  EXPECT_EQ(handler->responses()[task_ids[0]], output);
  EXPECT_EQ(env.state().CallCount(Rpc::DownloadResultData), 2);
}

TEST_F(SessionServiceFakeTest, DownloadInterruptedMidStreamExhaustsSdkRetries) {
  env.state().SetDownloadChunkSize(16);
  env.state().SetTaskHandler([](const Fakes::FakeTask &) {
    Fakes::TaskOutcome outcome;
    outcome.output = std::string(100, 'z');
    return outcome;
  });
  env.state().InjectMidStreamError(grpc::Status(grpc::StatusCode::UNAVAILABLE, "connection dropped"),
                                   kDownloadMaxRetry);

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  auto task_ids = service.Submit({TaskPayload("Echo", "x")}, handler);
  service.WaitResults({}, ArmoniK::Sdk::Client::All, FastPolling());

  EXPECT_TRUE(handler->responses().empty());
  auto errors = handler->errors();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].task_id, task_ids[0]);
  EXPECT_NE(errors[0].what.find("connection dropped"), std::string::npos) << errors[0].what;
  EXPECT_EQ(env.state().CallCount(Rpc::DownloadResultData), kDownloadMaxRetry);
}

TEST_F(SessionServiceFakeTest, AbortedResultStillReportedWhenGetTaskIsUnavailable) {
  env.state().SetTaskHandler([](const Fakes::FakeTask &) {
    Fakes::TaskOutcome outcome;
    outcome.kind = Fakes::TaskOutcome::Abort;
    outcome.error_detail = "worker crashed";
    return outcome;
  });
  env.state().InjectError(Rpc::GetTask, grpc::Status(grpc::StatusCode::UNAVAILABLE, "tasks service down"),
                          kGrpcMaxAttempts);

  SessionService service(env.properties(), env.logger);
  auto handler = std::make_shared<RecordingHandler>();
  auto task_ids = service.Submit({TaskPayload("Echo", "x")}, handler);
  ASSERT_NO_THROW(service.WaitResults({}, ArmoniK::Sdk::Client::All, FastPolling()));

  auto errors = handler->errors();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].task_id, task_ids[0]);
  ASSERT_TRUE(errors[0].is_task_error);
  ASSERT_EQ(errors[0].details.size(), 1u);
  EXPECT_NE(errors[0].details[0].second.find("tasks service down"), std::string::npos) << errors[0].details[0].second;
  EXPECT_EQ(env.state().CallCount(Rpc::GetTask), kGrpcMaxAttempts);
}

} // namespace

ARMONIK_SDK_SUPPRESS_DEPRECATED_END
