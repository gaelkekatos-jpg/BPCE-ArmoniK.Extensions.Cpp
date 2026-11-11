/**
 * Minimal library implementing the ArmoniK SDK C interface directly, used to test dynamic loading.
 * Methods:
 *  - "version": returns FIXTURE_VERSION
 *  - "echo": returns the input
 *  - "session": returns the current session id
 *  - "service": returns "<namespace>::<name>" of the service
 *  - "stats": returns "entered=<n>,left=<n>" for the service
 *  - "error": reports a permanent error with the input as message
 *  - "error_silent": returns an error status without calling the callback
 *  - "retry": reports a retryable error with the input as message
 * Defining FIXTURE_OMIT_CALL removes armonik_call to simulate a library with a missing symbol.
 */
#include <armonik/sdk/worker/ArmoniKSDKInterface.h>
#include <string>

namespace {
struct FixtureService {
  std::string service_namespace;
  std::string service_name;
  int entered = 0;
  int left = 0;
};

struct FixtureSession {
  std::string id;
};

#ifndef FIXTURE_OMIT_CALL
armonik_status_t reply(armonik_callback_t callback, void *armonik_context, armonik_status_t status,
                       const std::string &data) {
  callback(armonik_context, status, data.data(), data.size());
  return status;
}
#endif
} // namespace

extern "C" {

void *armonik_create_service(const char *service_namespace, const char *service_name) {
  auto service = new FixtureService();
  service->service_namespace = service_namespace;
  service->service_name = service_name;
  return service;
}

void armonik_destroy_service(void *service_context) { delete static_cast<FixtureService *>(service_context); }

void *armonik_enter_session(void *service_context, const char *session_id) {
  static_cast<FixtureService *>(service_context)->entered++;
  auto session = new FixtureSession();
  session->id = session_id;
  return session;
}

void armonik_leave_session(void *service_context, void *session_context) {
  static_cast<FixtureService *>(service_context)->left++;
  delete static_cast<FixtureSession *>(session_context);
}

#ifndef FIXTURE_OMIT_CALL
armonik_status_t armonik_call(void *armonik_context, void *service_context, void *session_context,
                              const char *function_name, const char *input, size_t input_size,
                              armonik_callback_t callback) {
  const auto service = static_cast<FixtureService *>(service_context);
  const auto session = static_cast<FixtureSession *>(session_context);
  const std::string name(function_name);
  const std::string args(input, input_size);

  if (name == "version") {
    return reply(callback, armonik_context, ARMONIK_STATUS_OK, FIXTURE_VERSION);
  }
  if (name == "echo") {
    return reply(callback, armonik_context, ARMONIK_STATUS_OK, args);
  }
  if (name == "session") {
    return reply(callback, armonik_context, ARMONIK_STATUS_OK, session->id);
  }
  if (name == "service") {
    return reply(callback, armonik_context, ARMONIK_STATUS_OK,
                 service->service_namespace + "::" + service->service_name);
  }
  if (name == "stats") {
    return reply(callback, armonik_context, ARMONIK_STATUS_OK,
                 "entered=" + std::to_string(service->entered) + ",left=" + std::to_string(service->left));
  }
  if (name == "error") {
    return reply(callback, armonik_context, ARMONIK_STATUS_ERROR, args);
  }
  if (name == "error_silent") {
    return ARMONIK_STATUS_ERROR;
  }
  if (name == "retry") {
    return reply(callback, armonik_context, ARMONIK_STATUS_RETRY, args);
  }
  return reply(callback, armonik_context, ARMONIK_STATUS_ERROR, "Unknown method " + name);
}
#endif
}
