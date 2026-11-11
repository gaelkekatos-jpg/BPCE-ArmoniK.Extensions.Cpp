#include "ServiceManager.h"
#include "ContextIds.h"
#include <armonik/common/utils/string_view.h>
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/worker/Worker/ProcessStatus.h>
#include <stdexcept>
#include <utility>

namespace ArmoniK {
namespace Sdk {
namespace DynamicWorker {
namespace {
struct ArmonikContext {
  armonik::api::worker::TaskHandler &taskHandler;
  armonik::api::worker::ProcessStatus output;
  bool retry_requested = false;
  std::string retry_message;

  explicit ArmonikContext(armonik::api::worker::TaskHandler &taskHandler) : taskHandler(taskHandler) {}
};
} // namespace

ServiceManager::ServiceManager(ArmoniKFunctionPointers functionsPointers, ServiceId serviceId)
    : serviceId(std::move(serviceId)), functionPointers(functionsPointers) {
  service_context = this->functionPointers.create_service(this->serviceId.service_namespace.c_str(),
                                                          this->serviceId.service_name.c_str());
}
ServiceManager::~ServiceManager() { clear(); }
ServiceManager &ServiceManager::UseSession(const std::string &sessionId) & {
  if (serviceId.empty()) {
    throw ArmoniK::Sdk::Common::ArmoniKSdkException("No service is loaded");
  }
  if (sessionId != current_session) {
    if (!current_session.empty()) {
      functionPointers.leave_session(service_context, session_context);
    }
    current_session = sessionId;
    session_context = functionPointers.enter_session(service_context, sessionId.c_str());
  }
  return *this;
}
armonik::api::worker::ProcessStatus ServiceManager::Execute(armonik::api::worker::TaskHandler &taskHandler,
                                                            const std::string &method_name,
                                                            const std::string &method_arguments) {
  ArmonikContext callContext(taskHandler);
  if (current_session.empty()) {
    throw ArmoniK::Sdk::Common::ArmoniKSdkException("Session is not initialized");
  }
  auto status = functionPointers.call(&callContext, service_context, session_context, method_name.c_str(),
                                      method_arguments.data(), method_arguments.size(), ServiceManager::UploadResult);
  if (callContext.retry_requested) {
    throw std::runtime_error(callContext.retry_message);
  }
  if (status != ARMONIK_STATUS_OK) {
    if (callContext.output.ok()) {
      callContext.output.set_error("Unknown error in worker, check logs.");
    }
  } else {
    callContext.output.set_ok();
  }
  return callContext.output;
}

void ServiceManager::UploadResult(void *opaque_context, armonik_status_t status, const char *data, size_t data_size) {
  auto context = static_cast<ArmonikContext *>(opaque_context);
  if (status == ARMONIK_STATUS_RETRY) {
    context->retry_requested = true;
    context->retry_message = std::string(data, data_size);
    return;
  }
  if (status != ARMONIK_STATUS_OK) {
    context->output.set_error(std::string(data, data_size));
    return;
  }
  context->taskHandler.send_result(context->taskHandler.getExpectedResults()[0],
                                   armonik::api::string_view(data, data_size));
  context->output.set_ok();
}
bool ServiceManager::matches(const ServiceId &other) { return other == serviceId; }
void ServiceManager::clear() {
  if (serviceId.empty()) {
    return;
  }
  if (!current_session.empty()) {
    functionPointers.leave_session(service_context, session_context);
    current_session.clear();
  }
  functionPointers.destroy_service(service_context);
  serviceId.clear();
}
} // namespace DynamicWorker
} // namespace Sdk
} // namespace ArmoniK
