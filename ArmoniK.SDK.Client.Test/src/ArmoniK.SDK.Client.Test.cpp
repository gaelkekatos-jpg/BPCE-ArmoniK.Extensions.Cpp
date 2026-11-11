#include <cstring>
#include <gtest/gtest.h>
#include <iostream>

#include <algorithm>
#include <armonik/client/tasks_service.grpc.pb.h>
#include <armonik/common/logger/formatter.h>
#include <armonik/common/logger/logger.h>
#include <armonik/common/logger/writer.h>
#include <armonik/common/tasks_filters.pb.h>
#include <armonik/sdk/client/IServiceInvocationHandler.h>
#include <armonik/sdk/client/SessionService.h>
#include <armonik/sdk/common/BlobDefinition.h>
#include <armonik/sdk/common/Configuration.h>
#include <armonik/sdk/common/Diagnostics.h>
#include <armonik/sdk/common/DynamicLibrary.h>
#include <armonik/sdk/common/Properties.h>
#include <armonik/sdk/common/TaskDefinition.h>
#include <armonik/sdk/common/TaskPayload.h>
#include <chrono>
#include <cmath>
#include <numeric>
#include <thread>

#include "ChannelPool.h"
#include "E2ETestFixture.h"
#include "End2EndHandlers.h"

ARMONIK_SDK_SUPPRESS_DEPRECATED_BEGIN

class testSDK : public ArmoniKE2ETest {};

template <typename T> std::string StrSerialize(T a, T b) {
  char payload_val[sizeof(T) * 2];

  std::memcpy(payload_val, &a, sizeof(T));
  std::memcpy(payload_val + sizeof(T), &b, sizeof(T));

  return std::string(payload_val, sizeof(T) * 2);
}

std::vector<double> compute_workload(const std::vector<double> &input, const std::size_t nbOutputBytes,
                                     const std::uint32_t workLoadTimeInMs) {
  using clock = std::chrono::high_resolution_clock;
  using milliseconds = std::chrono::milliseconds;

  if (input.empty() || nbOutputBytes <= 0) {
    return std::vector<double>{};
  }

  double result = 0.;
  for (auto x : input) {
    result += x * x * x;
  }
  std::vector<double> output(nbOutputBytes / 8, 0);
  const std::size_t output_size = output.size();

  const auto end = clock::now() + milliseconds(workLoadTimeInMs);
  do {
    for (std::size_t i = 0; i < output_size; ++i) {
      output[i] = result / static_cast<double>(output_size);
    }
  } while (clock::now() <= end);
  return output;
}

template <typename T> void serialize_item(char *&outputVec, const T *item, const std::size_t nbItem) {
  std::memcpy(outputVec, item, sizeof(T) * nbItem);
  outputVec += sizeof(T) * nbItem;
}
template <typename T> void serialize_item(char *&outputVec, const T &item) { serialize_item(outputVec, &item, 1); }

TEST_F(testSDK, testEcho) {
  std::cout << "Hello, World!" << std::endl;
  // Load configuration from file and environment
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "\nEndpoint : " << config.get("GrpcClient__Endpoint") << std::endl;
  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }

  // Create the task options
  ArmoniK::Sdk::Common::TaskOptions session_task_options("libArmoniK.SDK.Worker.Test.so",
                                                         config.get("WorkerLib__Version"), config.get("Worker__Type"),
                                                         "EchoService", config.get("PartitionId"));
  session_task_options.max_retries = 1;

  // Create the properties
  ArmoniK::Sdk::Common::Properties properties{config, session_task_options};

  // Create the logger
  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Debug};

  // Create the session service
  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  // Get the created session id
  std::cout << "Session : " << service.getSession() << std::endl;
  std::string args;
  args.resize(10);
  args[0] = 'A';
  args[1] = 'r';
  args[2] = 'm';
  args[3] = '0';
  args[4] = '\0';
  args[5] = 'n';
  args[6] = 1;
  args[7] = 'K';
  args[8] = 0;
  args[9] = -128;

  ASSERT_EQ(args[0], 'A');

  // Create the handler
  auto handler = std::make_shared<EchoServiceHandler>(logger);

  // Submit a task
  auto tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("EchoService", args)}, handler);

  std::cout << "Sent : " << tasks[0] << std::endl;

  // Wait for task completion
  service.WaitResults();

  ASSERT_TRUE(!args.empty());
  ASSERT_TRUE(handler->received);
  ASSERT_FALSE(handler->is_error);

  service.CloseSession();
  std::cout << "Done" << std::endl;
}

TEST_F(testSDK, testAddInt) {
  std::cout << "Doing some computation!" << std::endl;
  // Load configuration from file and environment
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "\nEndpoint : " << config.get("GrpcClient__Endpoint") << std::endl;
  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }

  // Create the task options
  ArmoniK::Sdk::Common::TaskOptions session_task_options("libArmoniK.SDK.Worker.Test.so",
                                                         config.get("WorkerLib__Version"), "End2EndTest",
                                                         "AdditionService", config.get("PartitionId"));
  session_task_options.max_retries = 1;

  // Create the properties
  ArmoniK::Sdk::Common::Properties properties{config, session_task_options};

  // Create the logger
  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Debug};

  // Create the session service
  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  // Get the created session id
  std::cout << "Session : " << service.getSession() << std::endl;

  // ASSERT_EQ(args[0], "1");

  // Create the handler
  auto handler = std::make_shared<AddServiceHandler>();
  /*std::shared_ptr<ArmoniK::Sdk::Client::IServiceInvocationHandler> handler(
      (config.get("Worker__Type").c_str() == "PythonTestWorker")
          ? static_cast<ArmoniK::Sdk::Client::IServiceInvocationHandler *>(new PythonTestWorkerHandler)
          : (session_task_options.application_service == "EchoService")
          ? static_cast<ArmoniK::Sdk::Client::IServiceInvocationHandler *>(new EchoServiceHandler)
      : static_cast<ArmoniK::Sdk::Client::IServiceInvocationHandler *>(new AddServiceHandler));*/

  handler->is_int = true;

  std::string payload_val = StrSerialize<int>(52, 72);

  // Submit a task
  std::vector<std::string> task_ids;
  // for (auto &&arg : args) {
  auto tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_ints", payload_val)}, handler);
  service.WaitResults();

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  // auto result_payload =
  service.WaitResults();

  auto ans = handler->check_int_result(52, 72);
  // std::cout << "ans: " << ans << std::endl;
  ASSERT_EQ(handler->int_result, ans);
  ASSERT_TRUE(!handler->str.empty());

  tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_ints", StrSerialize<int>(23, 45))}, handler);

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  service.WaitResults();

  ans = handler->check_int_result(23, 45);
  ASSERT_EQ(handler->int_result, ans);
  ASSERT_TRUE(!handler->str.empty());

  tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_ints", StrSerialize<int>(76, 34))}, handler);

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  service.WaitResults();

  ans = handler->check_int_result(76, 34);
  ASSERT_EQ(handler->int_result, ans);
  ASSERT_TRUE(!handler->str.empty());

  handler->successCounter = 0;
  tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_ints", StrSerialize<int>(82, 1)),
                          ArmoniK::Sdk::Common::TaskPayload("add_ints", StrSerialize<int>(4, 6))},
                         handler);

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  service.WaitResults();
  ASSERT_EQ(handler->successCounter, 2);
  ASSERT_TRUE(!handler->str.empty());

  service.CloseSession();
  std::cout << "Done" << std::endl;
}

TEST_F(testSDK, testAddFloat) {
  std::cout << "Doing some computation!" << std::endl;
  // Load configuration from file and environment
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "\nEndpoint : " << config.get("GrpcClient__Endpoint") << std::endl;
  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }

  // Create the task options
  ArmoniK::Sdk::Common::TaskOptions session_task_options("libArmoniK.SDK.Worker.Test.so",
                                                         config.get("WorkerLib__Version"), "End2EndTest",
                                                         "AdditionService", config.get("PartitionId"));
  session_task_options.max_retries = 1;

  // Create the properties
  ArmoniK::Sdk::Common::Properties properties{config, session_task_options};

  // Create the logger
  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Debug};

  // Create the session service
  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  // Get the created session id
  std::cout << "Session : " << service.getSession() << std::endl;

  // Create the handler
  auto handler = std::make_shared<AddServiceHandler>();
  /*std::shared_ptr<ArmoniK::Sdk::Client::IServiceInvocationHandler> handler(
      (config.get("Worker__Type").c_str() == "PythonTestWorker")
          ? static_cast<ArmoniK::Sdk::Client::IServiceInvocationHandler *>(new PythonTestWorkerHandler)
          : (session_task_options.application_service == "EchoService")
          ? static_cast<ArmoniK::Sdk::Client::IServiceInvocationHandler *>(new EchoServiceHandler)
      : static_cast<ArmoniK::Sdk::Client::IServiceInvocationHandler *>(new AddServiceHandler));*/

  handler->is_int = false;
  // Submit a task
  std::vector<std::string> task_ids;

  auto tasks =
      service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_floats", StrSerialize<float>(32.3f, 21.2f))}, handler);

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  // auto result_payload =
  service.WaitResults();

  auto ans = handler->check_float_result(32.3f, 21.2f);

  auto error = 0.0000001;

  EXPECT_NEAR(handler->float_result, ans, error);

  tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_floats", StrSerialize<float>(32.5f, 54.7f))}, handler);

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  service.WaitResults();

  ans = handler->check_float_result(32.5f, 54.7f);
  EXPECT_NEAR(handler->float_result, ans, error);
  ASSERT_TRUE(!handler->str.empty());
  ASSERT_EQ(handler->str.size(), sizeof(float));

  tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_floats", StrSerialize<float>(2.9f, 1.07f))}, handler);

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  service.WaitResults();

  ans = handler->check_float_result(2.9f, 1.07f);
  EXPECT_NEAR(handler->float_result, ans, error);
  ASSERT_TRUE(!handler->str.empty());
  ASSERT_EQ(handler->str.size(), sizeof(float));

  tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_floats", StrSerialize<float>(64.2f, 54.0f))}, handler);

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  service.WaitResults();

  ans = handler->check_float_result(64.2f, 54.0f);
  EXPECT_NEAR(handler->float_result, ans, error);
  ASSERT_TRUE(!handler->str.empty());
  ASSERT_EQ(handler->str.size(), sizeof(float));

  tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("add_floats", StrSerialize<float>(7.6f, 8.5f))}, handler);

  ASSERT_FALSE(tasks.empty());

  // Wait for task completion
  service.WaitResults();

  ans = handler->check_float_result(7.6f, 8.5f);
  EXPECT_NEAR(handler->float_result, ans, error);
  ASSERT_TRUE(!handler->str.empty());
  ASSERT_EQ(handler->str.size(), sizeof(float));
  //}

  service.CloseSession();
  std::cout << "Done" << std::endl;
}

TEST_F(testSDK, testStressTest) {
  std::cout << "Doing some computation!" << std::endl;
  // Load configuration from file and environment
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "\nEndpoint : " << config.get("GrpcClient__Endpoint") << std::endl;
  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }

  // Create the task options
  ArmoniK::Sdk::Common::TaskOptions session_task_options("libArmoniK.SDK.Worker.Test.so",
                                                         config.get("WorkerLib__Version"), "End2EndTest", "StressTest",
                                                         config.get("PartitionId"));
  session_task_options.max_retries = 1;

  // Create the properties
  ArmoniK::Sdk::Common::Properties properties{config, session_task_options};

  // Create the logger
  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Info};

  // Create the session service
  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  // Get the created session id
  std::cout << "Session : " << service.getSession() << std::endl;

  // Create the handler
  auto handler = std::make_shared<StressTestServiceHandler>(logger);

  const std::uint32_t nbTasks = 1000, nbInputBytes = 100000, nbOutputBytes = 8, workloadTimeInMs = 1;

  const std::vector<double> input(nbInputBytes / sizeof(double),
                                  std::pow(42.0 * 8.0 / static_cast<double>(nbInputBytes), 1.0 / 3.0));
  std::cout << "StressTest default parameters:\n"
            << "number of tasks: " << nbTasks << '\n'
            << "number of input bytes: " << nbInputBytes << '\n'
            << "number of output bytes: " << nbOutputBytes << '\n'
            << "workload time in milliseconds: " << workloadTimeInMs << std::endl;

  auto payload = [nbInputBytes, nbOutputBytes, workloadTimeInMs, &input]() -> std::string {
    std::vector<char> outputVec(nbInputBytes + sizeof(nbOutputBytes) * 3, 0);
    auto beginPtr = outputVec.data();
    serialize_item(beginPtr, nbOutputBytes);

    serialize_item(beginPtr, workloadTimeInMs);

    serialize_item(beginPtr, nbInputBytes);

    serialize_item(beginPtr, input.data(), nbInputBytes / sizeof(double));
    return {outputVec.data(), outputVec.size()};
  }();
  const std::vector<ArmoniK::Sdk::Common::TaskPayload> tasks_payload(nbTasks, {"compute_workload", std::move(payload)});

  auto t0 = std::chrono::high_resolution_clock::now();
  const auto tasks = service.Submit(tasks_payload, handler);
  auto t1 = std::chrono::high_resolution_clock::now();

  std::cout << "Submit: " << std::chrono::duration_cast<std::chrono::duration<float>>(t1 - t0).count() << " s"
            << std::endl;

  ASSERT_FALSE(tasks.empty());
  ASSERT_EQ(tasks.size(), nbTasks);

  // Wait for task completion
  auto t2 = std::chrono::high_resolution_clock::now();
  service.WaitResults();
  auto t3 = std::chrono::high_resolution_clock::now();

  std::cout << "Wait: " << std::chrono::duration_cast<std::chrono::duration<float>>(t3 - t2).count() << " s"
            << std::endl;

  ASSERT_TRUE(handler->is_ok);
  ASSERT_EQ(handler->nb_output_bytes, nbOutputBytes);
  const auto result_out = compute_workload(input, nbOutputBytes, workloadTimeInMs);
  for (auto &task : tasks) {
    for (std::size_t i = 0; i < result_out.size(); ++i) {
      ASSERT_NEAR(handler->results.at(task).at(i), result_out.at(i), 0.0000001);
    }
  }

  service.CloseSession();
  std::cout << "Done" << std::endl;
}

TEST_F(testSDK, parallelSubmit) {
  // Load configuration from file and environment
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }

  std::cout << std::endl;

  // Create the task options
  ArmoniK::Sdk::Common::TaskOptions session_task_options("libArmoniK.SDK.Worker.Test.so",
                                                         config.get("WorkerLib__Version"), "End2EndTest", "StressTest",
                                                         config.get("PartitionId"));
  session_task_options.max_retries = 1;

  // Create the properties
  ArmoniK::Sdk::Common::Properties properties{config, session_task_options};

  // Create the logger
  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Info};

  // Create the session service
  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  const std::uint32_t nbThreads = 10;
  const std::uint32_t nbTasksPerThread = 100;
  const std::uint32_t nbInputBytes = 100000;
  const std::uint32_t nbOutputBytes = 8;
  const std::uint32_t workloadTimeInMs = 1;
  std::vector<std::thread> threads;
  std::vector<std::vector<std::string>> tasks(nbThreads);
  std::vector<std::shared_ptr<StressTestServiceHandler>> handlers;
  handlers.reserve(nbThreads);

  // Get the created session id
  logger.info("Created session", {{"session_id", service.getSession()},
                                  {"nbThreads", std::to_string(nbThreads)},
                                  {"nbTasksPerThread", std::to_string(nbTasksPerThread)},
                                  {"nbInputBytes", std::to_string(nbInputBytes)},
                                  {"nbOutputBytes", std::to_string(nbOutputBytes)},
                                  {"workloadTimeInMs", std::to_string(workloadTimeInMs)}});

  for (std::size_t t = 0; t < nbThreads; ++t) {
    handlers.push_back(std::make_shared<StressTestServiceHandler>(logger));
    threads.emplace_back([&, t]() {
      std::vector<ArmoniK::Sdk::Common::TaskPayload> payloads;
      std::vector<double> input(nbInputBytes / sizeof(double));
      input[0] = t;

      for (std::size_t j = 0; j < nbTasksPerThread; ++j) {
        input[1] = j;
        std::vector<char> outputVec(nbInputBytes + sizeof(nbOutputBytes) * 3, 0);
        auto beginPtr = outputVec.data();
        serialize_item(beginPtr, nbOutputBytes);
        serialize_item(beginPtr, workloadTimeInMs);
        serialize_item(beginPtr, nbInputBytes);
        serialize_item(beginPtr, input.data(), nbInputBytes / sizeof(double));
        payloads.push_back({"compute_workload", {outputVec.data(), outputVec.size()}});
      }

      tasks[t] = service.Submit(payloads, handlers[t]);
      logger.info("Tasks submitted", {{"thread", std::to_string(t)}});

      service.WaitResults({tasks[t].begin(), tasks[t].end()});
      logger.info("Tasks received", {{"thread", std::to_string(t)}});
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }

  std::vector<double> inputReference(2);
  for (std::size_t t = 0; t < nbThreads; ++t) {
    auto &handler = handlers[t];
    inputReference[0] = t;

    for (std::size_t j = 0; j < nbTasksPerThread; ++j) {
      auto &task = tasks[t][j];
      inputReference[1] = j;

      const auto result_out = compute_workload(inputReference, nbOutputBytes, workloadTimeInMs);
      if ((t != 0 || j != 0) && result_out[0] == 0) {
        logger.fatal("Unexepected zero output", {{"t", std::to_string(t)}, {"j", std::to_string(j)}});
      }
      for (std::size_t i = 0; i < result_out.size(); ++i) {
        ASSERT_NEAR(handler->results.at(task).at(i), result_out.at(i), 0.0000001)
            << "\tthread: " << t << "\ttask: " << task << "\tj: " << j << "\ti: " << i;
      }
    }
  }

  service.CloseSession();
  logger.info("Closed session", {{"session_id", service.getSession()}});
}

TEST_F(testSDK, testSegFault) {
  std::cout << "Testing SegFault in worker!" << std::endl;
  // Load configuration from file and environment
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "\nEndpoint : " << config.get("GrpcClient__Endpoint") << std::endl;
  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }

  // Create the task options
  ArmoniK::Sdk::Common::TaskOptions session_task_options("libArmoniK.SDK.Worker.Test.so",
                                                         config.get("WorkerLib__Version"), "End2EndTest",
                                                         "SegFaultService", config.get("PartitionId"));
  session_task_options.max_retries = 1;

  // Create the properties
  ArmoniK::Sdk::Common::Properties properties{config, session_task_options};

  // Create the logger
  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Debug};

  // Create the session service
  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  // Get the created session id
  std::cout << "Session : " << service.getSession() << std::endl;
  std::string args = "SegFault success";

  // Create the handler
  auto handler = std::make_shared<SegFaultServiceHandler>(logger);

  // Submit a task
  auto tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("SegFaultService", args)}, handler);

  std::cout << "Sent : " << tasks[0] << std::endl;

  // Wait for task completion
  service.WaitResults();

  ASSERT_TRUE(!args.empty());
  ASSERT_TRUE(handler->received);
  ASSERT_TRUE(handler->is_error);

  service.CloseSession();
  std::cout << "Done" << std::endl;
}

TEST_F(testSDK, testLargePayload) {
  std::cout << "Testing large payload..." << std::endl;

  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "Endpoint : " << config.get("GrpcClient__Endpoint") << std::endl;
  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }

  ArmoniK::Sdk::Common::TaskOptions session_task_options("libArmoniK.SDK.Worker.Test.so",
                                                         config.get("WorkerLib__Version"), "End2EndTest", "EchoService",
                                                         config.get("PartitionId"));
  session_task_options.max_retries = 1;

  ArmoniK::Sdk::Common::Properties properties{config, session_task_options};

  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Debug};

  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  std::cout << "Session : " << service.getSession() << std::endl;

  // Create a large payload (> 4 MB)
  const size_t large_payload_size = 5 * 1024 * 1024;
  std::string large_payload(large_payload_size, 'L');

  ASSERT_EQ(large_payload.size(), large_payload_size);

  auto handler = std::make_shared<CountServiceHandler>(logger);

  auto tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload("EchoService", large_payload)}, handler);

  std::cout << "Sent : " << tasks[0] << std::endl;

  service.WaitResults();

  ASSERT_TRUE(!large_payload.empty());
  ASSERT_EQ(handler->success, 1);
  ASSERT_EQ(handler->failure, 0);

  service.CloseSession();
  std::cout << "Large payload test done!" << std::endl;
}

struct ExceptionTestParam {
  std::string method_name;
  int max_retries;
  // Expected number of task attempts in ArmoniK (1 + actual retries performed).
  int expected_task_count;
};

void PrintTo(const ExceptionTestParam &param, std::ostream *os) {
  *os << param.method_name << " (max_retries=" << param.max_retries
      << ", expected_task_count=" << param.expected_task_count << ")";
}

class ExceptionServiceTest : public ArmoniKE2ETest, public ::testing::WithParamInterface<ExceptionTestParam> {};

TEST_P(ExceptionServiceTest, HandlesExceptionCases) {
  const auto &param = GetParam();

  std::cout << "Testing Exception service..." << std::endl;

  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "Endpoint : " << config.get("GrpcClient__Endpoint") << std::endl;
  if (config.get("Worker__Type").empty()) {
    config.set("Worker__Type", "End2EndTest");
  }

  ArmoniK::Sdk::Common::TaskOptions session_task_options("libArmoniK.SDK.Worker.Test.so",
                                                         config.get("WorkerLib__Version"), "End2EndTest",
                                                         "ExceptionService", config.get("PartitionId"));
  session_task_options.max_retries = param.max_retries;

  ArmoniK::Sdk::Common::Properties properties{config, session_task_options};

  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Debug};

  ArmoniK::Sdk::Client::SessionService service(properties, logger);

  std::cout << "Session : " << service.getSession() << std::endl;

  auto handler = std::make_shared<ExceptionServiceHandler>(logger);
  std::string args = "Exception success";

  auto tasks = service.Submit({ArmoniK::Sdk::Common::TaskPayload(param.method_name, args)}, handler);

  std::cout << "Sent : " << tasks[0] << std::endl;

  service.WaitResults();

  ASSERT_TRUE(handler->received);
  ASSERT_TRUE(handler->is_error);
  // The client receives exactly one HandleError call once the result is aborted.
  ASSERT_EQ(handler->error_count, 1);

  // Verify the actual number of task attempts recorded by ArmoniK.
  // Each retry creates a new task, so total = 1 (original) + retries performed.
  // "sdkError" never retries (ArmoniKSdkException → output.error path) even though max_retries=2 → total == 1.
  // "retry" exhausts all retries (std::exception → gRPC UNAVAILABLE path) → total == max_retries + 1 == 3.
  {
    ArmoniK::Sdk::Client::Internal::ChannelPool pool(properties, logger);
    auto channel_guard = pool.GetChannel();
    auto tasks_stub = armonik::api::grpc::v1::tasks::Tasks::NewStub(channel_guard.channel);

    armonik::api::grpc::v1::tasks::ListTasksRequest list_request;
    armonik::api::grpc::v1::tasks::ListTasksResponse list_response;

    // Filter to this session only.
    armonik::api::grpc::v1::tasks::FilterField filter_field;
    filter_field.mutable_field()->mutable_task_summary_field()->set_field(
        armonik::api::grpc::v1::tasks::TASK_SUMMARY_ENUM_FIELD_SESSION_ID);
    filter_field.mutable_filter_string()->set_operator_(armonik::api::grpc::v1::FILTER_STRING_OPERATOR_EQUAL);
    filter_field.mutable_filter_string()->set_value(service.getSession());
    *list_request.mutable_filters()->mutable_or_()->Add()->mutable_and_()->Add() = filter_field;

    list_request.mutable_sort()->set_direction(armonik::api::grpc::v1::sort_direction::SORT_DIRECTION_ASC);
    list_request.mutable_sort()->mutable_field()->mutable_task_summary_field()->set_field(
        armonik::api::grpc::v1::tasks::TASK_SUMMARY_ENUM_FIELD_CREATED_AT);

    list_request.set_page(0);
    list_request.set_page_size(1);
    list_request.set_with_errors(true);

    grpc::ClientContext context;
    ASSERT_TRUE(tasks_stub->ListTasks(&context, list_request, &list_response).ok());
    ASSERT_EQ(list_response.total(), param.expected_task_count);
  }

  service.CloseSession();
  std::cout << "Done" << std::endl;
}

// Both cases use max_retries=2 to prove the distinction:
// "sdkError": ArmoniKSdkException → permanent failure (output.error) → 1 task attempt despite retries being allowed.
// "retry": std::runtime_error → transient failure (gRPC UNAVAILABLE) → 3 task attempts (original + 2 retries
// exhausted).
INSTANTIATE_TEST_SUITE_P(ExceptionCases, ExceptionServiceTest,
                         ::testing::Values(ExceptionTestParam{"sdkError", 2, 1}, ExceptionTestParam{"retry", 2, 3}),
                         [](const ::testing::TestParamInfo<ExceptionTestParam> &info) {
                           return info.param.method_name;
                         });

/* Submit a TaskDefinition via the convention path and verify the task
 * completes successfully. The method name is carried by the Symbol task option. */
TEST_F(testSDK, testConventionEcho) {
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "\nEndpoint : " << config.get("GrpcClient__Endpoint") << std::endl;

  ArmoniK::Sdk::Common::DynamicLibrary lib;
  lib.library_path = ConventionWorkerLibPath(config);
  lib.symbol = "EchoService"; // passed as function_name to armonik_call for dispatch

  // application_namespace and application_service are forwarded by DynamicWorker to
  // armonik_create_service — required by the test worker to dispatch to EchoService.
  ArmoniK::Sdk::Common::TaskOptions task_options("", config.get("WorkerLib__Version"), "End2EndTest", "EchoService",
                                                 config.get("PartitionId"));
  task_options.SetDynamicLibrary(lib);
  task_options.max_retries = 1;

  ArmoniK::Sdk::Common::Properties properties{config, task_options};

  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Debug};

  ArmoniK::Sdk::Client::SessionService service(properties, logger);
  std::cout << "Session : " << service.getSession() << std::endl;

  auto handler = std::make_shared<EchoServiceHandler>(logger);

  auto tasks = service.Submit({ArmoniK::Sdk::Common::TaskDefinition(
                                  {{"data", ArmoniK::Sdk::Common::BlobDefinition::FromData("hello-convention")}})},
                              handler);
  std::cout << "Sent : " << tasks[0] << std::endl;

  service.WaitResults();

  ASSERT_FALSE(tasks.empty());
  ASSERT_TRUE(handler->received);
  ASSERT_FALSE(handler->is_error);

  service.CloseSession();
  std::cout << "Convention echo test done!" << std::endl;
}

/* Compute 2^2 + 3^2 = 13 using three chained tasks:
 *   - Task A: square(2) -> result blob containing "4"
 *   - Task B: square(3) -> result blob containing "9"
 *   - Task C: add(FromBlobId(A), FromBlobId(B)) -> "13"
 * Verifies that result_id from HandleResponse can be passed to
 * BlobDefinition::FromBlobId to wire a task output as input of a downstream task. */
TEST_F(testSDK, testConventionChainedSquareThenAdd) {
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();

  std::cout << "\nEndpoint : " << config.get("GrpcClient__Endpoint") << std::endl;

  ArmoniK::Sdk::Common::DynamicLibrary lib;
  lib.library_path = ConventionWorkerLibPath(config);
  lib.symbol = "square";

  ArmoniK::Sdk::Common::TaskOptions task_options("libArmoniK.SDK.Worker.Test.so", config.get("WorkerLib__Version"),
                                                 "End2EndTest", "ConventionArithmetic", config.get("PartitionId"));
  task_options.max_retries = 1;
  task_options.SetDynamicLibrary(lib);

  ArmoniK::Sdk::Common::Properties properties{config, task_options};

  armonik::api::common::logger::Logger logger{armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(true),
                                              armonik::api::common::logger::Level::Debug};

  ArmoniK::Sdk::Client::SessionService service(properties, logger);
  ASSERT_FALSE(service.getSession().empty());

  auto opts_square = properties.taskOptions;

  auto handler_a = std::make_shared<ConventionResultHandler>(logger);
  auto handler_b = std::make_shared<ConventionResultHandler>(logger);
  service.Submit({ArmoniK::Sdk::Common::TaskDefinition({{"x", ArmoniK::Sdk::Common::BlobDefinition::FromData("2")}})},
                 handler_a, opts_square);
  service.Submit({ArmoniK::Sdk::Common::TaskDefinition({{"x", ArmoniK::Sdk::Common::BlobDefinition::FromData("3")}})},
                 handler_b, opts_square);
  service.WaitResults();

  ASSERT_TRUE(handler_a->received);
  ASSERT_FALSE(handler_a->is_error);
  EXPECT_EQ(handler_a->result_payload, "4");

  ASSERT_TRUE(handler_b->received);
  ASSERT_FALSE(handler_b->is_error);
  EXPECT_EQ(handler_b->result_payload, "9");

  auto opts_add = opts_square;
  opts_add.options[ArmoniK::Sdk::Common::DynamicLibrary::KeySymbol] = "add";

  auto handler_c = std::make_shared<ConventionResultHandler>(logger);
  service.Submit({ArmoniK::Sdk::Common::TaskDefinition(
                     {{"a", ArmoniK::Sdk::Common::BlobDefinition::FromBlobId(handler_a->result_id)},
                      {"b", ArmoniK::Sdk::Common::BlobDefinition::FromBlobId(handler_b->result_id)}})},
                 handler_c, opts_add);
  service.WaitResults();

  ASSERT_TRUE(handler_c->received);
  ASSERT_FALSE(handler_c->is_error);
  EXPECT_EQ(handler_c->result_payload, "13");

  service.CloseSession();
  std::cout << "Convention chained square then add test done!" << std::endl;
}

ARMONIK_SDK_SUPPRESS_DEPRECATED_END
