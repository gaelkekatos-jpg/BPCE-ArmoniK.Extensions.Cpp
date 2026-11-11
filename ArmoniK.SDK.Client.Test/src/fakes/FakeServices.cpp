#include "FakeServices.h"

#include <algorithm>
#include <utility>

namespace ArmoniK {
namespace Sdk {
namespace Client {
namespace Test {
namespace Fakes {

namespace v1 = armonik::api::grpc::v1;

FakeState::FakeState()
    : task_handler_([](const FakeTask &task) {
        TaskOutcome outcome;
        outcome.kind = TaskOutcome::Complete;
        outcome.output = task.payload;
        return outcome;
      }) {}

void FakeState::InjectError(const std::string &rpc, const grpc::Status &status, int times) {
  std::lock_guard<std::mutex> _(mutex_);
  for (int i = 0; i < times; ++i) {
    scripts_[rpc].injected.push_back(status);
  }
}

void FakeState::InjectMidStreamError(const grpc::Status &status, int times) {
  std::lock_guard<std::mutex> _(mutex_);
  for (int i = 0; i < times; ++i) {
    mid_stream_errors_.push_back(status);
  }
}

void FakeState::SetDelay(const std::string &rpc, std::chrono::milliseconds delay) {
  std::lock_guard<std::mutex> _(mutex_);
  scripts_[rpc].delay = delay;
}

void FakeState::CancelDelays() {
  {
    std::lock_guard<std::mutex> _(mutex_);
    delays_cancelled_ = true;
  }
  delay_cancelled_.notify_all();
}

int FakeState::CallCount(const std::string &rpc) const {
  std::lock_guard<std::mutex> _(mutex_);
  auto it = scripts_.find(rpc);
  return it == scripts_.end() ? 0 : it->second.calls;
}

void FakeState::SetTaskHandler(TaskHandler handler) {
  std::lock_guard<std::mutex> _(mutex_);
  task_handler_ = std::move(handler);
}

void FakeState::SetDataChunkMaxSize(int size) {
  std::lock_guard<std::mutex> _(mutex_);
  data_chunk_max_size_ = size;
}

void FakeState::SetDownloadChunkSize(int size) {
  std::lock_guard<std::mutex> _(mutex_);
  download_chunk_size_ = size;
}

int FakeState::DownloadChunkSize() const {
  std::lock_guard<std::mutex> _(mutex_);
  return download_chunk_size_ > 0 ? download_chunk_size_ : data_chunk_max_size_;
}

int FakeState::DataChunkMaxSize() const {
  std::lock_guard<std::mutex> _(mutex_);
  return data_chunk_max_size_;
}

void FakeState::CompleteResult(const std::string &result_id, const std::string &data) {
  std::lock_guard<std::mutex> _(mutex_);
  auto &result = results_.at(result_id);
  result.has_pending_outcome = false;
  result.data = data;
  result.status = v1::result_status::RESULT_STATUS_COMPLETED;
}

void FakeState::AbortResult(const std::string &result_id, const std::string &owner_task_id) {
  std::lock_guard<std::mutex> _(mutex_);
  auto &result = results_.at(result_id);
  result.has_pending_outcome = false;
  result.owner_task_id = owner_task_id;
  result.status = v1::result_status::RESULT_STATUS_ABORTED;
}

std::vector<FakeTask> FakeState::Tasks() const {
  std::lock_guard<std::mutex> _(mutex_);
  std::vector<FakeTask> tasks;
  for (const auto &id : task_order_) {
    tasks.push_back(tasks_.at(id));
  }
  return tasks;
}

std::vector<FakeTask> FakeState::TasksOfSession(const std::string &session_id) const {
  auto tasks = Tasks();
  tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [&](const FakeTask &t) { return t.session_id != session_id; }),
              tasks.end());
  return tasks;
}

std::vector<v1::sessions::CreateSessionRequest> FakeState::CreateSessionRequests() const {
  std::lock_guard<std::mutex> _(mutex_);
  return create_session_requests_;
}

std::vector<v1::tasks::SubmitTasksRequest> FakeState::SubmitTasksRequests() const {
  std::lock_guard<std::mutex> _(mutex_);
  return submit_tasks_requests_;
}

std::string FakeState::ResultData(const std::string &result_id) const {
  std::lock_guard<std::mutex> _(mutex_);
  auto it = results_.find(result_id);
  return it == results_.end() ? std::string() : it->second.data;
}

v1::result_status::ResultStatus FakeState::ResultStatus(const std::string &result_id) const {
  std::lock_guard<std::mutex> _(mutex_);
  auto it = results_.find(result_id);
  return it == results_.end() ? v1::result_status::RESULT_STATUS_NOTFOUND : it->second.status;
}

std::vector<std::string> FakeState::SessionStatusChanges(const std::string &session_id) const {
  std::lock_guard<std::mutex> _(mutex_);
  auto it = sessions_.find(session_id);
  return it == sessions_.end() ? std::vector<std::string>() : it->second.status_changes;
}

std::vector<std::string> FakeState::GetTaskIds() const {
  std::lock_guard<std::mutex> _(mutex_);
  return get_task_ids_;
}

grpc::Status FakeState::Enter(const std::string &rpc) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto &script = scripts_[rpc];
  ++script.calls;
  const auto delay = script.delay;
  grpc::Status status = grpc::Status::OK;
  if (!script.injected.empty()) {
    status = script.injected.front();
    script.injected.pop_front();
  }
  if (delay.count() > 0) {
    delay_cancelled_.wait_for(lock, delay, [this]() { return delays_cancelled_; });
  }
  return status;
}

bool FakeState::ConsumeMidStreamError(grpc::Status &status) {
  std::lock_guard<std::mutex> _(mutex_);
  if (mid_stream_errors_.empty()) {
    return false;
  }
  status = mid_stream_errors_.front();
  mid_stream_errors_.pop_front();
  return true;
}

std::string FakeState::CreateSession(const v1::sessions::CreateSessionRequest &request) {
  std::lock_guard<std::mutex> _(mutex_);
  create_session_requests_.push_back(request);
  auto id = "session-" + std::to_string(++next_id_);
  sessions_[id].options = request.default_task_option();
  return id;
}

bool FakeState::SetSessionStatus(const std::string &session_id, const std::string &status) {
  std::lock_guard<std::mutex> _(mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return false;
  }
  it->second.status_changes.push_back(status);
  return true;
}

std::string FakeState::CreateResult(const std::string &session_id, const std::string &name, const std::string *data) {
  std::lock_guard<std::mutex> _(mutex_);
  auto id = "result-" + std::to_string(++next_id_);
  auto &result = results_[id];
  result.session_id = session_id;
  result.name = name;
  if (data != nullptr) {
    result.data = *data;
    result.status = v1::result_status::RESULT_STATUS_COMPLETED;
  }
  return id;
}

bool FakeState::SetResultData(const std::string &result_id, std::string data, std::int64_t &size) {
  std::lock_guard<std::mutex> _(mutex_);
  auto it = results_.find(result_id);
  if (it == results_.end()) {
    return false;
  }
  it->second.data = std::move(data);
  it->second.status = v1::result_status::RESULT_STATUS_COMPLETED;
  size = static_cast<std::int64_t>(it->second.data.size());
  return true;
}

bool FakeState::GetResultData(const std::string &result_id, std::string &data) const {
  std::lock_guard<std::mutex> _(mutex_);
  auto it = results_.find(result_id);
  if (it == results_.end() || it->second.status != v1::result_status::RESULT_STATUS_COMPLETED) {
    return false;
  }
  data = it->second.data;
  return true;
}

void FakeState::ApplyOutcomeLocked(const std::string &result_id, Result &result) {
  (void)result_id;
  result.has_pending_outcome = false;
  switch (result.outcome.kind) {
  case TaskOutcome::Complete:
    result.data = result.outcome.output;
    result.status = v1::result_status::RESULT_STATUS_COMPLETED;
    break;
  case TaskOutcome::Abort:
    result.status = v1::result_status::RESULT_STATUS_ABORTED;
    break;
  case TaskOutcome::Pending:
    break;
  }
}

void FakeState::FillResultRawLocked(const std::string &result_id, const Result &result,
                                    v1::results::ResultRaw &raw) const {
  raw.set_result_id(result_id);
  raw.set_session_id(result.session_id);
  raw.set_name(result.name);
  raw.set_owner_task_id(result.owner_task_id);
  raw.set_status(result.status);
  raw.set_size(static_cast<std::int64_t>(result.data.size()));
}

void FakeState::ListResults(const v1::results::ListResultsRequest &request,
                            v1::results::ListResultsResponse &response) {
  std::lock_guard<std::mutex> _(mutex_);
  std::vector<std::string> matching;
  for (auto &kv : results_) {
    bool match = request.filters().or__size() == 0;
    for (const auto &and_filter : request.filters().or_()) {
      bool all = true;
      for (const auto &field : and_filter.and_()) {
        const auto &value = field.filter_string().value();
        switch (field.field().result_raw_field().field()) {
        case v1::results::RESULT_RAW_ENUM_FIELD_RESULT_ID:
          all = all && kv.first == value;
          break;
        case v1::results::RESULT_RAW_ENUM_FIELD_SESSION_ID:
          all = all && kv.second.session_id == value;
          break;
        default:
          break;
        }
      }
      if (all) {
        match = true;
        break;
      }
    }
    if (match) {
      matching.push_back(kv.first);
    }
  }

  const int page_size = request.page_size() > 0 ? request.page_size() : static_cast<int>(matching.size());
  const std::size_t begin = static_cast<std::size_t>(request.page()) * static_cast<std::size_t>(page_size);
  for (std::size_t i = begin; i < matching.size() && i < begin + static_cast<std::size_t>(page_size); ++i) {
    auto &result = results_.at(matching[i]);
    // Each observation of a pending result counts as one poll; the scripted outcome is applied afterwards
    if (result.has_pending_outcome) {
      if (result.polls_remaining > 0) {
        --result.polls_remaining;
      } else {
        ApplyOutcomeLocked(matching[i], result);
      }
    }
    FillResultRawLocked(matching[i], result, *response.add_results());
  }
  response.set_page(request.page());
  response.set_page_size(page_size);
  response.set_total(static_cast<int>(matching.size()));
}

void FakeState::DeleteResultsData(const std::vector<std::string> &ids) {
  std::lock_guard<std::mutex> _(mutex_);
  for (const auto &id : ids) {
    auto it = results_.find(id);
    if (it != results_.end()) {
      it->second.data.clear();
      it->second.status = v1::result_status::RESULT_STATUS_DELETED;
    }
  }
}

void FakeState::SubmitTasks(const v1::tasks::SubmitTasksRequest &request, v1::tasks::SubmitTasksResponse &response) {
  std::vector<FakeTask> created;
  TaskHandler handler;
  {
    std::lock_guard<std::mutex> _(mutex_);
    submit_tasks_requests_.push_back(request);
    handler = task_handler_;
    v1::TaskOptions base;
    auto session = sessions_.find(request.session_id());
    if (session != sessions_.end()) {
      base = session->second.options;
    }
    if (request.has_task_options()) {
      base = request.task_options();
    }
    for (const auto &creation : request.task_creations()) {
      FakeTask task;
      task.task_id = "task-" + std::to_string(++next_id_);
      task.session_id = request.session_id();
      task.payload_id = creation.payload_id();
      auto payload = results_.find(creation.payload_id());
      if (payload != results_.end()) {
        task.payload = payload->second.data;
      }
      task.data_dependencies.assign(creation.data_dependencies().begin(), creation.data_dependencies().end());
      task.expected_output_ids.assign(creation.expected_output_keys().begin(), creation.expected_output_keys().end());
      task.options = creation.has_task_options() ? creation.task_options() : base;
      for (const auto &out : task.expected_output_ids) {
        auto result = results_.find(out);
        if (result != results_.end()) {
          result->second.owner_task_id = task.task_id;
        }
      }
      task_order_.push_back(task.task_id);
      tasks_[task.task_id] = task;
      created.push_back(task);

      auto info = response.add_task_infos();
      info->set_task_id(task.task_id);
      info->set_payload_id(task.payload_id);
      for (const auto &out : task.expected_output_ids) {
        info->add_expected_output_ids(out);
      }
      for (const auto &dep : task.data_dependencies) {
        info->add_data_dependencies(dep);
      }
    }
  }

  // The handler is user code: call it without holding the lock
  std::vector<TaskOutcome> outcomes;
  outcomes.reserve(created.size());
  for (const auto &task : created) {
    outcomes.push_back(handler(task));
  }

  std::lock_guard<std::mutex> _(mutex_);
  for (std::size_t i = 0; i < created.size(); ++i) {
    const auto &outcome = outcomes[i];
    std::string owner = created[i].task_id;
    // Model control-plane retries: every extra attempt is a new task, retry of the previous one
    for (int attempt = 1; attempt < outcome.attempts; ++attempt) {
      auto &previous = tasks_.at(owner);
      previous.status = v1::task_status::TASK_STATUS_RETRIED;
      previous.error = outcome.error_detail;
      FakeTask retry = previous;
      retry.task_id = created[i].task_id + "-retry-" + std::to_string(attempt);
      retry.retry_of = owner;
      task_order_.push_back(retry.task_id);
      tasks_[retry.task_id] = retry;
      owner = retry.task_id;
    }
    auto &last = tasks_.at(owner);
    if (outcome.kind == TaskOutcome::Abort) {
      last.status = outcome.final_status;
      last.error = outcome.error_detail;
    } else if (outcome.kind == TaskOutcome::Complete) {
      last.status = v1::task_status::TASK_STATUS_COMPLETED;
    }
    for (const auto &out : created[i].expected_output_ids) {
      auto result = results_.find(out);
      if (result == results_.end()) {
        continue;
      }
      result->second.owner_task_id = owner;
      if (outcome.kind != TaskOutcome::Pending) {
        result->second.has_pending_outcome = true;
        result->second.polls_remaining = outcome.ready_after_polls;
        result->second.outcome = outcome;
      }
    }
  }
}

bool FakeState::GetTask(const std::string &task_id, v1::tasks::TaskDetailed &task) {
  std::lock_guard<std::mutex> _(mutex_);
  get_task_ids_.push_back(task_id);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) {
    return false;
  }
  const auto &t = it->second;
  task.set_id(t.task_id);
  task.set_session_id(t.session_id);
  task.set_payload_id(t.payload_id);
  task.set_status(t.status);
  *task.mutable_options() = t.options;
  for (const auto &out : t.expected_output_ids) {
    task.add_expected_output_ids(out);
  }
  for (const auto &dep : t.data_dependencies) {
    task.add_data_dependencies(dep);
  }
  if (!t.retry_of.empty()) {
    task.add_retry_of_ids(t.retry_of);
  }
  task.mutable_output()->set_success(t.error.empty());
  task.mutable_output()->set_error(t.error);
  return true;
}

void FakeState::CancelTasks(const std::vector<std::string> &ids) {
  std::lock_guard<std::mutex> _(mutex_);
  for (const auto &id : ids) {
    auto it = tasks_.find(id);
    if (it != tasks_.end()) {
      it->second.status = v1::task_status::TASK_STATUS_CANCELLED;
    }
  }
}

std::map<std::string, std::vector<std::string>>
FakeState::GetResultIds(const std::vector<std::string> &task_ids) const {
  std::lock_guard<std::mutex> _(mutex_);
  std::map<std::string, std::vector<std::string>> mapping;
  for (const auto &id : task_ids) {
    auto it = tasks_.find(id);
    if (it != tasks_.end()) {
      mapping[id] = it->second.expected_output_ids;
    }
  }
  return mapping;
}

// ---- Sessions ----

grpc::Status FakeSessionsService::CreateSession(grpc::ServerContext *,
                                                const v1::sessions::CreateSessionRequest *request,
                                                v1::sessions::CreateSessionReply *response) {
  auto status = state_.Enter(Rpc::CreateSession);
  if (!status.ok()) {
    return status;
  }
  response->set_session_id(state_.CreateSession(*request));
  return grpc::Status::OK;
}

namespace {
template <typename Response>
grpc::Status ChangeSessionStatus(FakeState &state, const char *rpc, const std::string &session_id, Response *response) {
  auto status = state.Enter(rpc);
  if (!status.ok()) {
    return status;
  }
  if (!state.SetSessionStatus(session_id, rpc)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "Unknown session " + session_id);
  }
  response->mutable_session()->set_session_id(session_id);
  return grpc::Status::OK;
}
} // namespace

grpc::Status FakeSessionsService::CloseSession(grpc::ServerContext *, const v1::sessions::CloseSessionRequest *request,
                                               v1::sessions::CloseSessionResponse *response) {
  return ChangeSessionStatus(state_, Rpc::CloseSession, request->session_id(), response);
}

grpc::Status FakeSessionsService::CancelSession(grpc::ServerContext *,
                                                const v1::sessions::CancelSessionRequest *request,
                                                v1::sessions::CancelSessionResponse *response) {
  return ChangeSessionStatus(state_, Rpc::CancelSession, request->session_id(), response);
}

grpc::Status FakeSessionsService::PurgeSession(grpc::ServerContext *, const v1::sessions::PurgeSessionRequest *request,
                                               v1::sessions::PurgeSessionResponse *response) {
  return ChangeSessionStatus(state_, Rpc::PurgeSession, request->session_id(), response);
}

// ---- Results ----

grpc::Status FakeResultsService::GetServiceConfiguration(grpc::ServerContext *, const v1::Empty *,
                                                         v1::results::ResultsServiceConfigurationResponse *response) {
  auto status = state_.Enter(Rpc::GetServiceConfiguration);
  if (!status.ok()) {
    return status;
  }
  response->set_data_chunk_max_size(state_.DataChunkMaxSize());
  return grpc::Status::OK;
}

grpc::Status FakeResultsService::CreateResultsMetaData(grpc::ServerContext *,
                                                       const v1::results::CreateResultsMetaDataRequest *request,
                                                       v1::results::CreateResultsMetaDataResponse *response) {
  auto status = state_.Enter(Rpc::CreateResultsMetaData);
  if (!status.ok()) {
    return status;
  }
  for (const auto &r : request->results()) {
    auto meta = response->add_results();
    meta->set_session_id(request->session_id());
    meta->set_name(r.name());
    meta->set_result_id(state_.CreateResult(request->session_id(), r.name(), nullptr));
  }
  return grpc::Status::OK;
}

grpc::Status FakeResultsService::CreateResults(grpc::ServerContext *, const v1::results::CreateResultsRequest *request,
                                               v1::results::CreateResultsResponse *response) {
  auto status = state_.Enter(Rpc::CreateResults);
  if (!status.ok()) {
    return status;
  }
  for (const auto &r : request->results()) {
    auto meta = response->add_results();
    meta->set_session_id(request->session_id());
    meta->set_name(r.name());
    meta->set_result_id(state_.CreateResult(request->session_id(), r.name(), &r.data()));
  }
  return grpc::Status::OK;
}

grpc::Status FakeResultsService::UploadResultData(grpc::ServerContext *,
                                                  grpc::ServerReader<v1::results::UploadResultDataRequest> *reader,
                                                  v1::results::UploadResultDataResponse *response) {
  auto status = state_.Enter(Rpc::UploadResultData);
  if (!status.ok()) {
    return status;
  }
  v1::results::UploadResultDataRequest request;
  std::string session_id, result_id, data;
  while (reader->Read(&request)) {
    if (request.has_id()) {
      session_id = request.id().session_id();
      result_id = request.id().result_id();
    } else {
      data.append(request.data_chunk());
    }
  }
  std::int64_t size = 0;
  if (!state_.SetResultData(result_id, std::move(data), size)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "Unknown result " + result_id);
  }
  response->mutable_result()->set_result_id(result_id);
  response->mutable_result()->set_session_id(session_id);
  response->mutable_result()->set_size(size);
  response->mutable_result()->set_status(v1::result_status::RESULT_STATUS_COMPLETED);
  return grpc::Status::OK;
}

grpc::Status
FakeResultsService::DownloadResultData(grpc::ServerContext *, const v1::results::DownloadResultDataRequest *request,
                                       grpc::ServerWriter<v1::results::DownloadResultDataResponse> *writer) {
  auto status = state_.Enter(Rpc::DownloadResultData);
  if (!status.ok()) {
    return status;
  }
  std::string data;
  if (!state_.GetResultData(request->result_id(), data)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "Result " + request->result_id() + " has no data");
  }
  const std::size_t chunk_size = static_cast<std::size_t>(std::max(1, state_.DownloadChunkSize()));
  grpc::Status mid_stream;
  const bool fail_mid_stream = state_.ConsumeMidStreamError(mid_stream);
  v1::results::DownloadResultDataResponse response;
  for (std::size_t offset = 0; offset < data.size(); offset += chunk_size) {
    response.set_data_chunk(data.substr(offset, chunk_size));
    if (!writer->Write(response)) {
      return grpc::Status(grpc::StatusCode::CANCELLED, "Client stopped reading");
    }
    if (fail_mid_stream) {
      return mid_stream;
    }
  }
  return grpc::Status::OK;
}

grpc::Status FakeResultsService::ListResults(grpc::ServerContext *, const v1::results::ListResultsRequest *request,
                                             v1::results::ListResultsResponse *response) {
  auto status = state_.Enter(Rpc::ListResults);
  if (!status.ok()) {
    return status;
  }
  state_.ListResults(*request, *response);
  return grpc::Status::OK;
}

grpc::Status FakeResultsService::DeleteResultsData(grpc::ServerContext *,
                                                   const v1::results::DeleteResultsDataRequest *request,
                                                   v1::results::DeleteResultsDataResponse *response) {
  auto status = state_.Enter(Rpc::DeleteResultsData);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::string> ids(request->result_id().begin(), request->result_id().end());
  state_.DeleteResultsData(ids);
  response->set_session_id(request->session_id());
  *response->mutable_result_id() = request->result_id();
  return grpc::Status::OK;
}

// ---- Tasks ----

grpc::Status FakeTasksService::SubmitTasks(grpc::ServerContext *, const v1::tasks::SubmitTasksRequest *request,
                                           v1::tasks::SubmitTasksResponse *response) {
  auto status = state_.Enter(Rpc::SubmitTasks);
  if (!status.ok()) {
    return status;
  }
  state_.SubmitTasks(*request, *response);
  return grpc::Status::OK;
}

grpc::Status FakeTasksService::GetTask(grpc::ServerContext *, const v1::tasks::GetTaskRequest *request,
                                       v1::tasks::GetTaskResponse *response) {
  auto status = state_.Enter(Rpc::GetTask);
  if (!status.ok()) {
    return status;
  }
  if (!state_.GetTask(request->task_id(), *response->mutable_task())) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "Unknown task " + request->task_id());
  }
  return grpc::Status::OK;
}

grpc::Status FakeTasksService::CancelTasks(grpc::ServerContext *, const v1::tasks::CancelTasksRequest *request,
                                           v1::tasks::CancelTasksResponse *response) {
  auto status = state_.Enter(Rpc::CancelTasks);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::string> ids(request->task_ids().begin(), request->task_ids().end());
  state_.CancelTasks(ids);
  for (const auto &id : ids) {
    response->add_tasks()->set_id(id);
  }
  return grpc::Status::OK;
}

grpc::Status FakeTasksService::GetResultIds(grpc::ServerContext *, const v1::tasks::GetResultIdsRequest *request,
                                            v1::tasks::GetResultIdsResponse *response) {
  auto status = state_.Enter(Rpc::GetResultIds);
  if (!status.ok()) {
    return status;
  }
  std::vector<std::string> ids(request->task_id().begin(), request->task_id().end());
  for (auto &kv : state_.GetResultIds(ids)) {
    auto entry = response->add_task_results();
    entry->set_task_id(kv.first);
    for (const auto &rid : kv.second) {
      entry->add_result_ids(rid);
    }
  }
  return grpc::Status::OK;
}

// ---- Server ----

FakeArmoniKServer::FakeArmoniKServer() : sessions_(state_), results_(state_), tasks_(state_) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
  builder.RegisterService(&sessions_);
  builder.RegisterService(&results_);
  builder.RegisterService(&tasks_);
  server_ = builder.BuildAndStart();
  if (!server_ || port_ == 0) {
    throw std::runtime_error("Could not start the fake ArmoniK gRPC server");
  }
}

FakeArmoniKServer::~FakeArmoniKServer() {
  state_.CancelDelays();
  server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  server_->Wait();
}

std::string FakeArmoniKServer::endpoint() const { return "http://127.0.0.1:" + std::to_string(port_); }

std::shared_ptr<grpc::Channel> FakeArmoniKServer::InProcessChannel() {
  return server_->InProcessChannel(grpc::ChannelArguments());
}

} // namespace Fakes
} // namespace Test
} // namespace Client
} // namespace Sdk
} // namespace ArmoniK
