#pragma once

#include <armonik/sdk/common/Configuration.h>
#include <gtest/gtest.h>
#include <string>

/**
 * @brief Reads the control plane endpoint the same way the End2End tests do (appsettings.json, then environment)
 * @return The value of GrpcClient__Endpoint, empty when no ArmoniK cluster is configured
 */
inline std::string GetE2EGrpcEndpoint() {
  ArmoniK::Sdk::Common::Configuration config;
  config.add_json_configuration("appsettings.json").add_env_configuration();
  return config.get("GrpcClient__Endpoint");
}

/**
 * @brief Base fixture for tests that need a deployed ArmoniK cluster: the test is skipped, not failed, when
 * GrpcClient__Endpoint is not configured.
 */
class ArmoniKE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    if (GetE2EGrpcEndpoint().empty()) {
      GTEST_SKIP() << "GrpcClient__Endpoint is not set: skipping End2End test (no ArmoniK cluster configured)";
    }
  }
};
