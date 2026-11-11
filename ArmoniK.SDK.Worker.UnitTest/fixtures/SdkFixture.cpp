/**
 * Library built on ArmoniK.SDK.Worker: only armonik_create_service is defined, the other entry points are the
 * library defaults dispatching to ServiceBase.
 */
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/worker/ArmoniKSDKInterface.h>
#include <armonik/sdk/worker/ServiceBase.h>
#include <stdexcept>
#include <string>

namespace {
class CalculatorService : public ArmoniK::Sdk::Worker::ServiceBase {
public:
  using ServiceBase::call;

  void *enter_session(const char *session_id) override { return new std::string(session_id); }
  void leave_session(void *session_ctx) override { delete static_cast<std::string *>(session_ctx); }

  std::string call(void *session_ctx, const std::string &name,
                   const std::map<std::string, std::string> &inputs) override {
    if (name == "add") {
      return std::to_string(std::stoi(inputs.at("a")) + std::stoi(inputs.at("b")));
    }
    if (name == "session") {
      return *static_cast<std::string *>(session_ctx);
    }
    if (name == "fail") {
      throw ArmoniK::Sdk::Common::ArmoniKSdkException("permanent failure");
    }
    if (name == "transient") {
      throw std::runtime_error("transient failure");
    }
    return ServiceBase::call(session_ctx, name, inputs);
  }
};
} // namespace

extern "C" void *armonik_create_service(const char *service_namespace, const char *service_name) {
  (void)service_namespace;
  (void)service_name;
  return new CalculatorService();
}
