#include "ApplicationManager.h"
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/common/Configuration.h>
#include <armonik/sdk/common/TaskPayload.h>
#include <armonik/sdk/common/internal/ConventionPayload.h>
#include <sstream>

namespace ArmoniK {
namespace Sdk {
namespace DynamicWorker {

ApplicationManager &ApplicationManager::UseApplication(const AppId &appId) & {
  if (appId == currentId) {
    return *this;
  }
  ResetLibrary();
  std::string filename(applicationsBasePath + '/' + appId.application_name +
                       (appId.application_version.empty() ? "" : "." + appId.application_version));
  LoadDynamicLibrary(filename);
  currentId = appId;
  logger.info("Successfully loaded application " + appId.application_name + " ( " + appId.application_version + " )");
  return *this;
}
ApplicationManager &ApplicationManager::UseService(const ServiceId &serviceId) & {
  if (!service_manager.matches(serviceId)) {
    service_manager = ServiceManager(functionPointers, serviceId);
  }

  return *this;
}
ApplicationManager &ApplicationManager::UseSession(const std::string &sessionId) & {
  service_manager.UseSession(sessionId);
  return *this;
}
armonik::api::worker::ProcessStatus ApplicationManager::Execute(armonik::api::worker::TaskHandler &taskHandler,
                                                                const std::string &method_name,
                                                                const std::string &method_arguments) {
  return service_manager.Execute(taskHandler, method_name, method_arguments);
}

armonik::api::worker::ProcessStatus ApplicationManager::Execute(armonik::api::worker::TaskHandler &taskHandler,
                                                                const std::string &method_name,
                                                                const std::map<std::string, std::string> &inputs,
                                                                const std::map<std::string, std::string> &outputs) {
  ArmoniK::Sdk::Common::ConventionPayload payload;
  payload.inputs = inputs;
  payload.outputs = outputs;
  return Execute(taskHandler, method_name, payload.Serialize());
}

ApplicationManager &ApplicationManager::UseLibrary(const ArmoniK::Sdk::Common::DynamicLibrary &lib,
                                                   const std::string &service_namespace,
                                                   const std::string &service_name) & {
  if (lib.library_path == currentLibraryPath && service_name == currentLibraryServiceName) {
    return *this;
  }
  ResetLibrary();
  LoadDynamicLibrary(lib.library_path);

  currentLibraryPath = lib.library_path;
  currentLibraryServiceName = service_name;
  service_manager =
      ServiceManager(functionPointers, ServiceId({lib.library_path, ""}, service_namespace, service_name));
  logger.info("Successfully loaded library " + lib.library_path);
  return *this;
}
void ApplicationManager::ResetLibrary() {
  service_manager.clear();
  currentId.clear();
  currentLibraryPath.clear();
  currentLibraryServiceName.clear();
  functionPointers.clear();
}
void ApplicationManager::LoadDynamicLibrary(const std::string &filename) {
  DynamicLib library(filename.c_str());
  const ArmoniKFunctionPointers pointers{library.get<armonik_create_service_t>("armonik_create_service"),
                                         library.get<armonik_destroy_service_t>("armonik_destroy_service"),
                                         library.get<armonik_enter_session_t>("armonik_enter_session"),
                                         library.get<armonik_leave_session_t>("armonik_leave_session"),
                                         library.get<armonik_call_t>("armonik_call")};
  currentLibrary = std::move(library);
  functionPointers = pointers;
}
ApplicationManager::ApplicationManager(const ArmoniK::Sdk::Common::Configuration &config,
                                       const armonik::api::common::logger::Logger &logger)
    : functionPointers(), logger(logger.local()) {
  applicationsBasePath = config.get("Worker__ApplicationBasePath");
  if (applicationsBasePath.empty()) {
    applicationsBasePath = "/data";
  }
}
} // namespace DynamicWorker
} // namespace Sdk
} // namespace ArmoniK
