#pragma once

#include "ContextIds.h"
#include "DynamicLib.h"
#include "ServiceManager.h"
#include <Worker/ProcessStatus.h>
#include <armonik/common/logger/logger.h>
#include <armonik/sdk/common/DynamicLibrary.h>
#include <armonik/worker/Worker/TaskHandler.h>
#include <map>
#include <string>

namespace ArmoniK {
namespace Sdk {
namespace Common {
class Configuration;
}
} // namespace Sdk
} // namespace ArmoniK

namespace ArmoniK {
namespace Sdk {
namespace DynamicWorker {
/**
 * @brief Application manager to load and unload applications
 */
class ApplicationManager {

public:
  /**
   * @brief Creates an application manager
   * @param config Configuration
   * @note Configuration key: `Worker__ApplicationBasePath` (default: /data) (type: string)
   * @note Base directory used to locate the legacy-path application shared library by filename
   *       (`application_name.application_version` or `application_name`)
   */
  explicit ApplicationManager(const ArmoniK::Sdk::Common::Configuration &config,
                              const armonik::api::common::logger::Logger &logger);

  /**
   * @brief Configures the application manager to use the given application
   * @param appId Application Id
   * @return this ApplicationHandler
   * @note If the appId is different to the current one, the currently loaded service will be destroyed and the
   * application will be unloaded before loading the new one
   * @note If the appId is identical to the current one, this call does nothing
   */
  ApplicationManager &UseApplication(const AppId &appId) &;

  /**
   * @brief Configures the application manager to load a library directly by path (convention mode).
   * The library is loaded from lib.library_path; ABI symbols are always resolved with the "armonik_" prefix.
   * lib.symbol carries the method name passed to armonik_call at execution time.
   * @param lib DynamicLibrary descriptor
   * @param service_namespace Namespace passed to armonik_create_service (empty for single-service workers)
   * @param service_name Name passed to armonik_create_service (empty for single-service workers)
   * @return this ApplicationManager
   * @note Caches by library_path + service_name; a repeated call with the same values is a no-op.
   */
  ApplicationManager &UseLibrary(const ArmoniK::Sdk::Common::DynamicLibrary &lib,
                                 const std::string &service_namespace = "", const std::string &service_name = "") &;

  /**
   * @brief Configures the application manager to use the given service
   * @param serviceId Service id
   * @return this Application manager
   * @note If the current service matches the requested service id, then this does nothing
   * @note If the current service is different from the requested service, then this will destroy the current service
   * before creating the new one
   */
  ApplicationManager &UseService(const ServiceId &serviceId) &;

  /**
   * @brief Configures the applcation manager to use the given session
   * @param sessionId Session id
   * @return this Application manager
   * @note See ArmoniK::Sdk::DynamicWorker::ServiceManager::UseSession() for more info
   */
  ApplicationManager &UseSession(const std::string &sessionId) &;

  /**
   * @brief Executes the task given by the task handler
   * @param taskHandler Task handler
   * @param method_name Name of the method to execute
   * @param method_arguments Serialized arguments of the method
   * @return ProcessStatus telling whether the call was successful or not
   */
  armonik::api::worker::ProcessStatus Execute(armonik::api::worker::TaskHandler &taskHandler,
                                              const std::string &method_name, const std::string &method_arguments);

  /**
   * @brief Executes the task given by the task handler using named blob maps (convention mode).
   * Serializes inputs and outputs to JSON and delegates to Execute(taskHandler, method_name, json).
   * @param taskHandler Task handler
   * @param method_name Name of the method to execute
   * @param inputs Named input blob IDs
   * @param outputs Named output blob IDs
   * @return ProcessStatus telling whether the call was successful or not
   */
  armonik::api::worker::ProcessStatus Execute(armonik::api::worker::TaskHandler &taskHandler,
                                              const std::string &method_name,
                                              const std::map<std::string, std::string> &inputs,
                                              const std::map<std::string, std::string> &outputs);

private:
  /**
   * @brief Releases the current service and forgets the loaded library, so that a failed load leaves no stale state
   */
  void ResetLibrary();

  /**
   * @brief Loads the given library and resolves the ArmoniK SDK entry points
   * @param filename Path to the library
   * @throws ArmoniKSdkException if the library cannot be loaded or misses an entry point
   */
  void LoadDynamicLibrary(const std::string &filename);

  /**
   * @brief Loaded application's function pointers
   */
  ArmoniKFunctionPointers functionPointers;
  /**
   * @brief Currently loaded application id
   */
  AppId currentId;

  /**
   * @brief Currently loaded library
   * @note Declared before service_manager so that the service is destroyed before its library is unloaded
   */
  DynamicLib currentLibrary;

  /**
   * @brief Current service manager
   */
  ServiceManager service_manager;

  /**
   * @brief Base path in which to look for the library to load (legacy mode)
   */
  std::string applicationsBasePath;

  /**
   * @brief Path of the currently loaded library (convention mode); empty when in legacy mode
   */
  std::string currentLibraryPath;

  /**
   * @brief Service name used when the current library was loaded (convention mode)
   */
  std::string currentLibraryServiceName;

  /**
   * @brief Local Logger
   */
  armonik::api::common::logger::LocalLogger logger;
};
} // namespace DynamicWorker
} // namespace Sdk
} // namespace ArmoniK
