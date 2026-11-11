#pragma once

#include <agent_service.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ArmoniK {
namespace Sdk {
namespace Worker {
namespace UnitTest {

/**
 * @brief Fake ArmoniK Agent recording the results notified by the worker
 */
class FakeAgentService final : public armonik::api::grpc::v1::agent::Agent::Service {
public:
  ::grpc::Status NotifyResultData(::grpc::ServerContext *context,
                                  const armonik::api::grpc::v1::agent::NotifyResultDataRequest *request,
                                  armonik::api::grpc::v1::agent::NotifyResultDataResponse *response) override;

  /**
   * @brief Makes subsequent NotifyResultData calls fail with the given status
   */
  void FailNotifyWith(::grpc::Status status);

  std::vector<std::string> NotifiedResultIds() const;
  std::vector<std::string> NotifiedSessionIds() const;
  std::vector<std::string> CommunicationTokens() const;

private:
  mutable std::mutex mutex_;
  std::vector<std::string> result_ids_;
  std::vector<std::string> session_ids_;
  std::vector<std::string> tokens_;
  ::grpc::Status notify_status_;
};

/**
 * @brief In-process gRPC server hosting a FakeAgentService
 */
class FakeAgentServer {
public:
  FakeAgentServer();
  ~FakeAgentServer();

  FakeAgentService &service() { return service_; }

  std::unique_ptr<armonik::api::grpc::v1::agent::Agent::Stub> NewStub();

private:
  FakeAgentService service_;
  std::unique_ptr<::grpc::Server> server_;
};

} // namespace UnitTest
} // namespace Worker
} // namespace Sdk
} // namespace ArmoniK
