#pragma once

#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/worker/ServiceBase.h>
#include <iostream>
namespace ArmoniK {
namespace Sdk {
namespace Worker {
namespace Test {

/**
 * @brief Example implementation of a ArmoniK::Sdk::Worker::ServiceBase
 */
class ExceptionService : ServiceBase {
public:
  std::string call(void *, const std::string &name, const std::string &) override {
    std::cout << "ExceptionService method : " << name << std::endl;
    if (name == "sdkError") {
      throw ArmoniK::Sdk::Common::ArmoniKSdkException("Permanent SDK error, no retry");
    }
    if (name == "retry") {
      throw std::runtime_error("Transient error, retry requested");
    }
    throw std::logic_error("Logic error in method");
  }

  void *enter_session(const char *session_id) override {
    std::cout << "ExceptionService entering session : " << session_id << std::endl;
    return new std::string(session_id);
  }
  void leave_session(void *session_ctx) override {
    auto session_id = static_cast<std::string *>(session_ctx);
    std::cout << "ExceptionService leaving session : " << *session_id << std::endl;
    delete session_id;
  }
  ~ExceptionService() override { std::cout << "Deleted ExceptionService" << std::endl; };

  ExceptionService() : ServiceBase() { std::cout << "Created ExceptionService" << std::endl; }
};

} // namespace Test
} // namespace Worker
} // namespace Sdk
} // namespace ArmoniK
