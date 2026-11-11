#pragma once

#include "fakes/FakeAgent.h"
#include <armonik/common/logger/formatter.h>
#include <armonik/common/logger/logger.h>
#include <armonik/common/logger/writer.h>
#include <armonik/sdk/common/TaskOptions.h>
#include <armonik/sdk/common/internal/ConventionPayload.h>
#include <armonik/worker/Worker/TaskHandler.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ftw.h>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
#include <worker_common.pb.h>

namespace ArmoniK {
namespace Sdk {
namespace Worker {
namespace UnitTest {

inline std::string ReadFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Cannot open " + path);
  }
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

inline void WriteFile(const std::string &path, const std::string &content) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << content;
}

/**
 * @brief Temporary directory removed recursively on destruction
 */
class TempDir {
public:
  TempDir() {
    std::string pattern = "/tmp/armonik-worker-unittest-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (mkdtemp(buffer.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = buffer.data();
  }
  ~TempDir() {
    nftw(
        path_.c_str(), [](const char *fpath, const struct stat *, int, struct FTW *) { return std::remove(fpath); }, 16,
        FTW_DEPTH | FTW_PHYS);
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;

  const std::string &path() const { return path_; }
  std::string file(const std::string &name) const { return path_ + "/" + name; }

private:
  std::string path_;
};

inline armonik::api::common::logger::Logger MakeTestLogger() {
  return armonik::api::common::logger::Logger(armonik::api::common::logger::writer_console(),
                                              armonik::api::common::logger::formatter_plain(false),
                                              armonik::api::common::logger::Level::Warning);
}

/**
 * @brief Builds the ProcessRequest of a task with its payload and data dependencies stored in a temporary data folder,
 * as the ArmoniK agent does
 */
class TaskBuilder {
public:
  TaskBuilder() {
    request_.set_communication_token("token");
    request_.set_session_id("session");
    request_.set_task_id("task");
    request_.set_data_folder(folder_.path());
    request_.mutable_configuration()->set_data_chunk_max_size(84000);
    SetPayload("");
  }

  TaskBuilder &SetSession(const std::string &session_id) {
    request_.set_session_id(session_id);
    return *this;
  }

  TaskBuilder &SetPayload(const std::string &payload) {
    request_.set_payload_id("payload");
    WriteFile(folder_.file("payload"), payload);
    return *this;
  }

  TaskBuilder &SetConventionPayload(const std::map<std::string, std::string> &inputs,
                                    const std::map<std::string, std::string> &outputs) {
    Common::ConventionPayload payload;
    payload.inputs = inputs;
    payload.outputs = outputs;
    return SetPayload(payload.Serialize());
  }

  TaskBuilder &AddDataDependency(const std::string &id, const std::string &content) {
    request_.add_data_dependencies(id);
    WriteFile(folder_.file(id), content);
    return *this;
  }

  TaskBuilder &AddExpectedOutput(const std::string &id) {
    request_.add_expected_output_keys(id);
    return *this;
  }

  TaskBuilder &SetTaskOptions(const Common::TaskOptions &options) {
    *request_.mutable_task_options() = static_cast<armonik::api::grpc::v1::TaskOptions>(options);
    return *this;
  }

  const armonik::api::grpc::v1::worker::ProcessRequest &request() const { return request_; }

  std::unique_ptr<armonik::api::worker::TaskHandler> Handler(armonik::api::grpc::v1::agent::Agent::Stub &stub) const {
    return std::unique_ptr<armonik::api::worker::TaskHandler>(new armonik::api::worker::TaskHandler(stub, request_));
  }

  /**
   * @brief Content written by the worker for the given result
   */
  std::string Result(const std::string &id) const { return ReadFile(folder_.file(id)); }

private:
  TempDir folder_;
  armonik::api::grpc::v1::worker::ProcessRequest request_;
};

inline Common::TaskOptions EmptyTaskOptions() { return Common::TaskOptions("", "", "", ""); }

inline Common::TaskOptions ConventionOptions(const std::string &library_path, const std::string &symbol,
                                             const std::string &service_namespace = "",
                                             const std::string &service_name = "") {
  Common::TaskOptions options("", "", service_namespace, service_name);
  Common::DynamicLibrary lib;
  lib.library_path = library_path;
  lib.symbol = symbol;
  options.SetDynamicLibrary(lib);
  return options;
}

} // namespace UnitTest
} // namespace Worker
} // namespace Sdk
} // namespace ArmoniK
