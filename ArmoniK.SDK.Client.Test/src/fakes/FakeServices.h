#pragma once

#include <armonik/client/results_service.grpc.pb.h>
#include <armonik/client/sessions_service.grpc.pb.h>
#include <armonik/client/tasks_service.grpc.pb.h>
#include <armonik/common/objects.pb.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ArmoniK {
namespace Sdk {
namespace Client {
namespace Test {
namespace Fakes {

/**
 * @brief Names of the scriptable RPCs, as used by FakeState::InjectError / CallCount
 */
namespace Rpc {
constexpr const char CreateSession[] = "CreateSession";
constexpr const char CloseSession[] = "CloseSession";
constexpr const char CancelSession[] = "CancelSession";
constexpr const char PurgeSession[] = "PurgeSession";
constexpr const char GetServiceConfiguration[] = "GetServiceConfiguration";
constexpr const char CreateResultsMetaData[] = "CreateResultsMetaData";
constexpr const char CreateResults[] = "CreateResults";
constexpr const char UploadResultData[] = "UploadResultData";
constexpr const char DownloadResultData[] = "DownloadResultData";
constexpr const char ListResults[] = "ListResults";
constexpr const char DeleteResultsData[] = "DeleteResultsData";
constexpr const char SubmitTasks[] = "SubmitTasks";
constexpr const char GetTask[] = "GetTask";
constexpr const char CancelTasks[] = "CancelTasks";
constexpr const char GetResultIds[] = "GetResultIds";
} // namespace Rpc

/**
 * @brief A task as received by the fake Tasks service
 */
struct FakeTask {
  std::string task_id;
  std::string session_id;
  std::string payload_id;
  /** Content of the payload result at submission time (empty if not uploaded yet) */
  std::string payload;
  std::vector<std::string> data_dependencies;
  std::vector<std::string> expected_output_ids;
  /** Effective task options (per-task options if given, else request options, else session options) */
  armonik::api::grpc::v1::TaskOptions options;
  armonik::api::grpc::v1::task_status::TaskStatus status = armonik::api::grpc::v1::task_status::TASK_STATUS_SUBMITTED;
  std::string error;
  /** Id of the task this one is a retry of (empty for an original task) */
  std::string retry_of;
};

/**
 * @brief Scripted outcome of a submitted task, applied to its expected outputs
 */
struct TaskOutcome {
  enum Kind {
    /** Outputs stay CREATED until the test calls FakeState::CompleteResult / AbortResult */
    Pending,
    /** Outputs become COMPLETED with the given data */
    Complete,
    /** Outputs become ABORTED; the owner task (last attempt) carries error_detail */
    Abort,
  };
  Kind kind = Complete;
  std::string output;
  /** Number of ListResults observations reporting CREATED before the outcome is applied */
  int ready_after_polls = 0;
  /** Total number of attempts the control plane performs (1 = no retry). Extra attempts are recorded as retry tasks */
  int attempts = 1;
  std::string error_detail;
  armonik::api::grpc::v1::task_status::TaskStatus final_status = armonik::api::grpc::v1::task_status::TASK_STATUS_ERROR;
};

/**
 * @brief Shared, thread-safe state of the fake ArmoniK control plane, with scripting hooks
 */
class FakeState {
public:
  using TaskHandler = std::function<TaskOutcome(const FakeTask &)>;

  FakeState();

  /**
   * @brief Make the next @p times calls of @p rpc fail with @p status (before any response is written)
   */
  void InjectError(const std::string &rpc, const grpc::Status &status, int times = 1);
  /**
   * @brief Make the next @p times DownloadResultData calls fail with @p status after writing the first chunk
   */
  void InjectMidStreamError(const grpc::Status &status, int times = 1);
  /**
   * @brief Delay each call of @p rpc by @p delay before handling it
   */
  void SetDelay(const std::string &rpc, std::chrono::milliseconds delay);
  /**
   * @brief Wakes up every call currently sleeping in a SetDelay delay (used on server shutdown)
   */
  void CancelDelays();
  /**
   * @brief Number of times @p rpc has been called (including calls that got an injected error)
   */
  int CallCount(const std::string &rpc) const;

  /**
   * @brief Sets the handler deciding the outcome of each submitted task (default: echo the payload)
   */
  void SetTaskHandler(TaskHandler handler);
  /** @brief Value returned by GetServiceConfiguration (also the default download chunk size) */
  void SetDataChunkMaxSize(int size);
  /** @brief Size of the chunks streamed by DownloadResultData (0: use the data chunk max size) */
  void SetDownloadChunkSize(int size);

  void CompleteResult(const std::string &result_id, const std::string &data);
  void AbortResult(const std::string &result_id, const std::string &owner_task_id);

  std::vector<FakeTask> Tasks() const;
  std::vector<FakeTask> TasksOfSession(const std::string &session_id) const;
  std::vector<armonik::api::grpc::v1::sessions::CreateSessionRequest> CreateSessionRequests() const;
  std::vector<armonik::api::grpc::v1::tasks::SubmitTasksRequest> SubmitTasksRequests() const;
  std::string ResultData(const std::string &result_id) const;
  armonik::api::grpc::v1::result_status::ResultStatus ResultStatus(const std::string &result_id) const;
  std::vector<std::string> SessionStatusChanges(const std::string &session_id) const;
  std::vector<std::string> GetTaskIds() const;

  // ---- Used by the fake services ----

  /**
   * @brief Registers a call of @p rpc, applies the delay and returns the injected status (OK if none)
   */
  grpc::Status Enter(const std::string &rpc);
  bool ConsumeMidStreamError(grpc::Status &status);

  std::string CreateSession(const armonik::api::grpc::v1::sessions::CreateSessionRequest &request);
  bool SetSessionStatus(const std::string &session_id, const std::string &status);
  int DataChunkMaxSize() const;
  int DownloadChunkSize() const;
  std::string CreateResult(const std::string &session_id, const std::string &name, const std::string *data);
  bool SetResultData(const std::string &result_id, std::string data, std::int64_t &size);
  bool GetResultData(const std::string &result_id, std::string &data) const;
  void ListResults(const armonik::api::grpc::v1::results::ListResultsRequest &request,
                   armonik::api::grpc::v1::results::ListResultsResponse &response);
  void DeleteResultsData(const std::vector<std::string> &ids);
  void SubmitTasks(const armonik::api::grpc::v1::tasks::SubmitTasksRequest &request,
                   armonik::api::grpc::v1::tasks::SubmitTasksResponse &response);
  bool GetTask(const std::string &task_id, armonik::api::grpc::v1::tasks::TaskDetailed &task);
  void CancelTasks(const std::vector<std::string> &ids);
  std::map<std::string, std::vector<std::string>> GetResultIds(const std::vector<std::string> &task_ids) const;

private:
  struct Script {
    int calls = 0;
    std::deque<grpc::Status> injected;
    std::chrono::milliseconds delay{0};
  };
  struct Result {
    std::string session_id;
    std::string name;
    std::string data;
    std::string owner_task_id;
    armonik::api::grpc::v1::result_status::ResultStatus status =
        armonik::api::grpc::v1::result_status::RESULT_STATUS_CREATED;
    /** Outcome to apply once polls_remaining reaches zero */
    bool has_pending_outcome = false;
    int polls_remaining = 0;
    TaskOutcome outcome;
  };
  struct Session {
    armonik::api::grpc::v1::TaskOptions options;
    std::vector<std::string> status_changes;
  };

  void ApplyOutcomeLocked(const std::string &result_id, Result &result);
  void FillResultRawLocked(const std::string &result_id, const Result &result,
                           armonik::api::grpc::v1::results::ResultRaw &raw) const;

  mutable std::mutex mutex_;
  std::condition_variable delay_cancelled_;
  bool delays_cancelled_ = false;
  std::map<std::string, Script> scripts_;
  std::deque<grpc::Status> mid_stream_errors_;
  std::map<std::string, Session> sessions_;
  std::map<std::string, Result> results_;
  std::vector<std::string> task_order_;
  std::map<std::string, FakeTask> tasks_;
  std::vector<armonik::api::grpc::v1::sessions::CreateSessionRequest> create_session_requests_;
  std::vector<armonik::api::grpc::v1::tasks::SubmitTasksRequest> submit_tasks_requests_;
  std::vector<std::string> get_task_ids_;
  TaskHandler task_handler_;
  int data_chunk_max_size_ = 64 * 1024;
  int download_chunk_size_ = 0;
  long next_id_ = 0;
};

class FakeSessionsService final : public armonik::api::grpc::v1::sessions::Sessions::Service {
public:
  explicit FakeSessionsService(FakeState &state) : state_(state) {}
  grpc::Status CreateSession(grpc::ServerContext *context,
                             const armonik::api::grpc::v1::sessions::CreateSessionRequest *request,
                             armonik::api::grpc::v1::sessions::CreateSessionReply *response) override;
  grpc::Status CloseSession(grpc::ServerContext *context,
                            const armonik::api::grpc::v1::sessions::CloseSessionRequest *request,
                            armonik::api::grpc::v1::sessions::CloseSessionResponse *response) override;
  grpc::Status CancelSession(grpc::ServerContext *context,
                             const armonik::api::grpc::v1::sessions::CancelSessionRequest *request,
                             armonik::api::grpc::v1::sessions::CancelSessionResponse *response) override;
  grpc::Status PurgeSession(grpc::ServerContext *context,
                            const armonik::api::grpc::v1::sessions::PurgeSessionRequest *request,
                            armonik::api::grpc::v1::sessions::PurgeSessionResponse *response) override;

private:
  FakeState &state_;
};

class FakeResultsService final : public armonik::api::grpc::v1::results::Results::Service {
public:
  explicit FakeResultsService(FakeState &state) : state_(state) {}
  grpc::Status
  GetServiceConfiguration(grpc::ServerContext *context, const armonik::api::grpc::v1::Empty *request,
                          armonik::api::grpc::v1::results::ResultsServiceConfigurationResponse *response) override;
  grpc::Status CreateResultsMetaData(grpc::ServerContext *context,
                                     const armonik::api::grpc::v1::results::CreateResultsMetaDataRequest *request,
                                     armonik::api::grpc::v1::results::CreateResultsMetaDataResponse *response) override;
  grpc::Status CreateResults(grpc::ServerContext *context,
                             const armonik::api::grpc::v1::results::CreateResultsRequest *request,
                             armonik::api::grpc::v1::results::CreateResultsResponse *response) override;
  grpc::Status UploadResultData(grpc::ServerContext *context,
                                grpc::ServerReader<armonik::api::grpc::v1::results::UploadResultDataRequest> *reader,
                                armonik::api::grpc::v1::results::UploadResultDataResponse *response) override;
  grpc::Status
  DownloadResultData(grpc::ServerContext *context,
                     const armonik::api::grpc::v1::results::DownloadResultDataRequest *request,
                     grpc::ServerWriter<armonik::api::grpc::v1::results::DownloadResultDataResponse> *writer) override;
  grpc::Status ListResults(grpc::ServerContext *context,
                           const armonik::api::grpc::v1::results::ListResultsRequest *request,
                           armonik::api::grpc::v1::results::ListResultsResponse *response) override;
  grpc::Status DeleteResultsData(grpc::ServerContext *context,
                                 const armonik::api::grpc::v1::results::DeleteResultsDataRequest *request,
                                 armonik::api::grpc::v1::results::DeleteResultsDataResponse *response) override;

private:
  FakeState &state_;
};

class FakeTasksService final : public armonik::api::grpc::v1::tasks::Tasks::Service {
public:
  explicit FakeTasksService(FakeState &state) : state_(state) {}
  grpc::Status SubmitTasks(grpc::ServerContext *context,
                           const armonik::api::grpc::v1::tasks::SubmitTasksRequest *request,
                           armonik::api::grpc::v1::tasks::SubmitTasksResponse *response) override;
  grpc::Status GetTask(grpc::ServerContext *context, const armonik::api::grpc::v1::tasks::GetTaskRequest *request,
                       armonik::api::grpc::v1::tasks::GetTaskResponse *response) override;
  grpc::Status CancelTasks(grpc::ServerContext *context,
                           const armonik::api::grpc::v1::tasks::CancelTasksRequest *request,
                           armonik::api::grpc::v1::tasks::CancelTasksResponse *response) override;
  grpc::Status GetResultIds(grpc::ServerContext *context,
                            const armonik::api::grpc::v1::tasks::GetResultIdsRequest *request,
                            armonik::api::grpc::v1::tasks::GetResultIdsResponse *response) override;

private:
  FakeState &state_;
};

/**
 * @brief In-process gRPC server hosting the fake Sessions/Results/Tasks services.
 *
 * Listens on an ephemeral 127.0.0.1 port so the SDK can reach it through its regular
 * GrpcClient__Endpoint configuration (and therefore through the real ChannelFactory, including
 * its retry service config). InProcessChannel() gives direct stub access without the network.
 */
class FakeArmoniKServer {
public:
  FakeArmoniKServer();
  ~FakeArmoniKServer();
  FakeArmoniKServer(const FakeArmoniKServer &) = delete;
  FakeArmoniKServer &operator=(const FakeArmoniKServer &) = delete;

  FakeState &state() { return state_; }
  /** @brief Endpoint to use as GrpcClient__Endpoint, e.g. "http://127.0.0.1:41234" */
  std::string endpoint() const;
  std::shared_ptr<grpc::Channel> InProcessChannel();

private:
  FakeState state_;
  FakeSessionsService sessions_;
  FakeResultsService results_;
  FakeTasksService tasks_;
  std::unique_ptr<grpc::Server> server_;
  int port_ = 0;
};

} // namespace Fakes
} // namespace Test
} // namespace Client
} // namespace Sdk
} // namespace ArmoniK
