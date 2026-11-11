#include "fakes/FakeAgent.h"

namespace ArmoniK {
namespace Sdk {
namespace Worker {
namespace UnitTest {

::grpc::Status FakeAgentService::NotifyResultData(::grpc::ServerContext *context,
                                                  const armonik::api::grpc::v1::agent::NotifyResultDataRequest *request,
                                                  armonik::api::grpc::v1::agent::NotifyResultDataResponse *response) {
  (void)context;
  std::lock_guard<std::mutex> lock(mutex_);
  tokens_.push_back(request->communication_token());
  if (!notify_status_.ok()) {
    return notify_status_;
  }
  for (const auto &id : request->ids()) {
    result_ids_.push_back(id.result_id());
    session_ids_.push_back(id.session_id());
    response->add_result_ids(id.result_id());
  }
  return ::grpc::Status::OK;
}

void FakeAgentService::FailNotifyWith(::grpc::Status status) {
  std::lock_guard<std::mutex> lock(mutex_);
  notify_status_ = std::move(status);
}

std::vector<std::string> FakeAgentService::NotifiedResultIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_ids_;
}

std::vector<std::string> FakeAgentService::NotifiedSessionIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_ids_;
}

std::vector<std::string> FakeAgentService::CommunicationTokens() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tokens_;
}

FakeAgentServer::FakeAgentServer() {
  ::grpc::ServerBuilder builder;
  builder.RegisterService(&service_);
  server_ = builder.BuildAndStart();
}

FakeAgentServer::~FakeAgentServer() { server_->Shutdown(); }

std::unique_ptr<armonik::api::grpc::v1::agent::Agent::Stub> FakeAgentServer::NewStub() {
  return armonik::api::grpc::v1::agent::Agent::NewStub(server_->InProcessChannel(::grpc::ChannelArguments()));
}

} // namespace UnitTest
} // namespace Worker
} // namespace Sdk
} // namespace ArmoniK
