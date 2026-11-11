#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/worker/ArmoniKSDKInterface.h>
#include <armonik/sdk/worker/ServiceBase.h>
#include <gtest/gtest.h>
#include <stdexcept>

using ArmoniK::Sdk::Common::ArmoniKSdkException;
using ArmoniK::Sdk::Worker::ServiceBase;

namespace {
struct CallbackResult {
  int calls = 0;
  armonik_status_t status = ARMONIK_STATUS_OK;
  std::string data;
};

void record_callback(void *armonik_context, armonik_status_t status, const char *data, size_t size) {
  auto result = static_cast<CallbackResult *>(armonik_context);
  result->calls++;
  result->status = status;
  result->data.assign(data, size);
}

/**
 * @brief Service exercising the default ArmoniK.SDK.Worker entry points
 */
class RecordingService : public ServiceBase {
public:
  using ServiceBase::call;

  explicit RecordingService(bool *destroyed = nullptr) : destroyed(destroyed) {}
  ~RecordingService() override {
    if (destroyed) {
      *destroyed = true;
    }
  }

  void *enter_session(const char *session_id) override {
    entered = session_id;
    return &entered;
  }
  void leave_session(void *session_ctx) override { left = static_cast<std::string *>(session_ctx); }

  std::string call(void *session_ctx, const std::string &name,
                   const std::map<std::string, std::string> &inputs) override {
    last_session = session_ctx;
    last_inputs = inputs;
    if (name == "permanent") {
      throw ArmoniKSdkException("permanent failure");
    }
    if (name == "transient") {
      throw std::runtime_error("transient failure");
    }
    if (name == "base") {
      return ServiceBase::call(session_ctx, name, inputs);
    }
    return "called " + name;
  }

  bool *destroyed;
  std::string entered;
  std::string *left = nullptr;
  void *last_session = nullptr;
  std::map<std::string, std::string> last_inputs;
};

armonik_status_t Call(RecordingService &service, const std::string &name, const std::string &input,
                      CallbackResult &result) {
  return armonik_call(&result, &service, nullptr, name.c_str(), input.data(), input.size(), record_callback);
}
} // namespace

TEST(SdkInterface, CallDispatchesConventionPayloadToService) {
  RecordingService service;
  CallbackResult result;
  int session = 0;
  const std::string input = R"({"inputs":{"a":"1","b":"2"},"outputs":{"result":"id"}})";

  const auto status = armonik_call(&result, &service, &session, "method", input.data(), input.size(), record_callback);

  EXPECT_EQ(status, ARMONIK_STATUS_OK);
  EXPECT_EQ(result.calls, 1);
  EXPECT_EQ(result.status, ARMONIK_STATUS_OK);
  EXPECT_EQ(result.data, "called method");
  EXPECT_EQ(service.last_session, &session);
  EXPECT_EQ(service.last_inputs, (std::map<std::string, std::string>{{"a", "1"}, {"b", "2"}}));
}

TEST(SdkInterface, ArmoniKSdkExceptionMapsToPermanentError) {
  RecordingService service;
  CallbackResult result;
  EXPECT_EQ(Call(service, "permanent", R"({"inputs":{}})", result), ARMONIK_STATUS_ERROR);
  EXPECT_EQ(result.status, ARMONIK_STATUS_ERROR);
  EXPECT_EQ(result.data, "permanent failure");
}

TEST(SdkInterface, OtherExceptionMapsToRetry) {
  RecordingService service;
  CallbackResult result;
  EXPECT_EQ(Call(service, "transient", R"({"inputs":{}})", result), ARMONIK_STATUS_RETRY);
  EXPECT_EQ(result.status, ARMONIK_STATUS_RETRY);
  EXPECT_EQ(result.data, "transient failure");
}

TEST(SdkInterface, UnimplementedMethodIsPermanentError) {
  RecordingService service;
  CallbackResult result;
  EXPECT_EQ(Call(service, "base", R"({"inputs":{}})", result), ARMONIK_STATUS_ERROR);
  EXPECT_NE(result.data.find("ServiceBase::call not implemented for method: base"), std::string::npos);
}

class SdkInterfaceMalformedPayload : public ::testing::TestWithParam<std::string> {};

TEST_P(SdkInterfaceMalformedPayload, IsPermanentError) {
  RecordingService service;
  CallbackResult result;
  EXPECT_EQ(Call(service, "method", GetParam(), result), ARMONIK_STATUS_ERROR);
  EXPECT_EQ(result.status, ARMONIK_STATUS_ERROR);
  EXPECT_NE(result.data.find("Failed to parse convention payload"), std::string::npos) << result.data;
}

INSTANTIATE_TEST_SUITE_P(SdkInterface, SdkInterfaceMalformedPayload,
                         ::testing::Values("", "not json", R"({"inputs":)", R"({"outputs":{}})",
                                           R"({"inputs":{"a":1}})", R"({"inputs":["a"]})"));

TEST(SdkInterface, SessionEntryPointsDelegateToService) {
  RecordingService service;
  void *session = armonik_enter_session(&service, "my-session");
  EXPECT_EQ(service.entered, "my-session");
  EXPECT_EQ(session, &service.entered);
  armonik_leave_session(&service, session);
  EXPECT_EQ(service.left, &service.entered);
}

TEST(SdkInterface, DestroyServiceDeletesService) {
  bool destroyed = false;
  armonik_destroy_service(new RecordingService(&destroyed));
  EXPECT_TRUE(destroyed);
  EXPECT_NO_THROW(armonik_destroy_service(nullptr));
}
