#include <gtest/gtest.h>

#include <armonik/common/objects.pb.h>
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/common/BlobDefinition.h>
#include <armonik/sdk/common/ConventionTask.h>
#include <armonik/sdk/common/DynamicLibrary.h>
#include <armonik/sdk/common/TaskDefinition.h>
#include <armonik/sdk/common/TaskOptions.h>
#include <armonik/sdk/common/internal/ConventionPayload.h>
#include <nlohmann/json.hpp>

using namespace ArmoniK::Sdk::Common;

// ---------------------------------------------------------------------------
// TaskOptions::SetDynamicLibrary / GetDynamicLibrary
// ---------------------------------------------------------------------------

// Path-only loading: the classic case where the worker reads the .so directly from
// its local filesystem. No blob involved, so library_blob_id must come back empty.
TEST(TaskOptionsDynamicLibrary, RoundTripWithPathOnly) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");

  DynamicLibrary lib;
  lib.library_path = "/data/lib.so";
  lib.symbol = "myprefix";

  opts.SetDynamicLibrary(lib);
  auto restored = opts.GetDynamicLibrary();

  EXPECT_EQ(restored.library_path, "/data/lib.so");
  EXPECT_EQ(restored.symbol, "myprefix");
  EXPECT_TRUE(restored.library_blob_id.empty());
}

// Blob-only loading: the cross-SDK / dynamic upload case where the client has uploaded
// the .so via UploadLibrary(path, lib) and lib.library_blob_id is now set.
// library_path is optional and should be empty when not set.
TEST(TaskOptionsDynamicLibrary, RoundTripWithBlobId) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");

  DynamicLibrary lib;
  lib.library_blob_id = "blob-uuid-1234";
  lib.symbol = "myMethod"; // method name passed to armonik_call at execution time

  opts.SetDynamicLibrary(lib);
  auto restored = opts.GetDynamicLibrary();

  EXPECT_EQ(restored.library_blob_id, "blob-uuid-1234");
  EXPECT_EQ(restored.symbol, "myMethod");
}

// Both fields set: library_path is kept as a hint but the worker will prefer the blob.
// Ensures SetDynamicLibrary writes both keys and GetDynamicLibrary reads both.
TEST(TaskOptionsDynamicLibrary, RoundTripWithBothPathAndBlobId) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");

  DynamicLibrary lib;
  lib.library_path = "/data/lib.so";
  lib.library_blob_id = "blob-abc";
  lib.symbol = "sym";

  opts.SetDynamicLibrary(lib);
  auto restored = opts.GetDynamicLibrary();

  EXPECT_EQ(restored.library_path, "/data/lib.so");
  EXPECT_EQ(restored.library_blob_id, "blob-abc");
}

// When neither LibraryPath nor LibraryBlobId is present, GetDynamicLibrary must
// throw rather than silently return an unusable struct — the worker would dlopen("")
// which is implementation-defined and almost always wrong.
TEST(TaskOptionsDynamicLibrary, MissingLibraryPathWithoutBlobIdThrows) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.options[DynamicLibrary::KeyConventionVersion] = DynamicLibrary::ConventionVersion;
  opts.options[DynamicLibrary::KeySymbol] = "sym";
  // Neither LibraryPath nor LibraryBlobId set

  EXPECT_THROW(opts.GetDynamicLibrary(), ArmoniKSdkException);
}

// SetDynamicLibrary must always write the ConventionVersion key so that the worker
// can detect the encoding and dispatch to the right loading path.
TEST(TaskOptionsDynamicLibrary, SetDynamicLibrarySetsConventionVersion) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");

  DynamicLibrary lib;
  lib.library_path = "/data/lib.so";
  opts.SetDynamicLibrary(lib);

  EXPECT_EQ(opts.options.at(DynamicLibrary::KeyConventionVersion), DynamicLibrary::ConventionVersion);
}

// An empty library_blob_id must not be written to the options map: a missing key
// and an empty-string key have different semantics — the worker uses presence of
// LibraryBlobId to decide between path-based and blob-based loading.
TEST(TaskOptionsDynamicLibrary, BlobIdNotWrittenWhenEmpty) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");

  DynamicLibrary lib;
  lib.library_path = "/data/lib.so";
  lib.library_blob_id = "";
  opts.SetDynamicLibrary(lib);

  EXPECT_EQ(opts.options.count(DynamicLibrary::KeyLibraryBlobId), 0u);
}

// ---------------------------------------------------------------------------
// TaskOptions::GetConventionVersion
// ---------------------------------------------------------------------------

// After SetDynamicLibrary the version string must match the current convention.
TEST(TaskOptionsConventionVersion, ReturnsVersionWhenSet) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  DynamicLibrary lib;
  lib.library_path = "/data/lib.so";
  opts.SetDynamicLibrary(lib);

  EXPECT_EQ(opts.GetConventionVersion(), DynamicLibrary::ConventionVersion);
}

// Tasks that were submitted without the convention (e.g. legacy path) will not have
// ConventionVersion in their options; GetConventionVersion must throw so the caller
// can distinguish them from convention tasks.
TEST(TaskOptionsConventionVersion, ThrowsWhenMissing) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  EXPECT_THROW(opts.GetConventionVersion(), ArmoniKSdkException);
}

// ---------------------------------------------------------------------------
// TaskOptions::GetSymbol / ConventionTask::Decode
// ---------------------------------------------------------------------------

// A worker with the service compiled in only needs the method name: GetSymbol must
// not require LibraryPath or LibraryBlobId.
TEST(TaskOptionsSymbol, ReturnsSymbolWithoutLibraryLocation) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.options[DynamicLibrary::KeySymbol] = "square";

  EXPECT_EQ(opts.GetSymbol(), "square");
}

// SetConvention must write only the convention keys, so that a static worker
// receives no library location.
TEST(TaskOptionsSymbol, SetConventionWritesNoLibraryKeys) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.SetConvention("square");

  EXPECT_EQ(opts.GetConventionVersion(), DynamicLibrary::ConventionVersion);
  EXPECT_EQ(opts.GetSymbol(), "square");
  EXPECT_EQ(opts.options.count(DynamicLibrary::KeyLibraryPath), 0u);
  EXPECT_EQ(opts.options.count(DynamicLibrary::KeyLibraryBlobId), 0u);
}

TEST(TaskOptionsSymbol, ThrowsWhenMissingOrEmpty) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  EXPECT_THROW(opts.GetSymbol(), ArmoniKSdkException);

  opts.options[DynamicLibrary::KeySymbol] = "";
  EXPECT_THROW(opts.GetSymbol(), ArmoniKSdkException);
}

// Decoding must succeed with only ConventionVersion and Symbol set, and replace input
// values that name a data dependency by the dependency's content.
TEST(ConventionTask, DecodeWithoutLibraryLocation) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.SetConvention("square");
  ASSERT_TRUE(ConventionTask::IsConventionTask(opts));

  ConventionPayload payload;
  payload.inputs = {{"x", "blob-x"}, {"y", "inline"}};
  payload.outputs = {{"result", "blob-out"}};
  const std::map<std::string, std::string> deps = {{"blob-x", "7"}};

  const auto task = ConventionTask::Decode(opts, payload.Serialize(), deps);

  EXPECT_EQ(task.method_name, "square");
  EXPECT_EQ(task.inputs.at("x"), "7");
  EXPECT_EQ(task.inputs.at("y"), "inline");
  EXPECT_EQ(task.outputs.at("result"), "blob-out");
}

TEST(ConventionTask, DecodeRejectsUnsupportedVersion) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.options[DynamicLibrary::KeyConventionVersion] = "v0";
  opts.options[DynamicLibrary::KeySymbol] = "square";

  EXPECT_THROW(ConventionTask::Decode(opts, ConventionPayload().Serialize(), {}), ArmoniKSdkException);
}

TEST(ConventionTask, NotAConventionTaskWithoutVersion) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  EXPECT_FALSE(ConventionTask::IsConventionTask(opts));
}

// ---------------------------------------------------------------------------
// BlobDefinition
// ---------------------------------------------------------------------------

TEST(BlobDefinition, FromDataIsRaw) {
  auto b = BlobDefinition::FromData("hello");
  EXPECT_TRUE(b.IsRawData());
  EXPECT_EQ(b.GetData(), "hello");
}

TEST(BlobDefinition, FromDataPreservesBinaryContent) {
  std::string bytes(4, '\0');
  bytes[0] = '\x01';
  bytes[2] = '\xFF';
  auto b = BlobDefinition::FromData(bytes);
  EXPECT_TRUE(b.IsRawData());
  EXPECT_EQ(b.GetData(), bytes);
}

TEST(BlobDefinition, FromBlobIdIsNotRaw) {
  auto b = BlobDefinition::FromBlobId("result-uuid-1234");
  EXPECT_FALSE(b.IsRawData());
  EXPECT_EQ(b.GetBlobId(), "result-uuid-1234");
}

TEST(BlobDefinition, FromDataEmptyString) {
  auto b = BlobDefinition::FromData("");
  EXPECT_TRUE(b.IsRawData());
  EXPECT_EQ(b.GetData(), "");
}

TEST(BlobDefinition, FromBlobIdEmptyString) {
  auto b = BlobDefinition::FromBlobId("");
  EXPECT_FALSE(b.IsRawData());
  EXPECT_EQ(b.GetBlobId(), "");
}

// ---------------------------------------------------------------------------
// TaskDefinition
// ---------------------------------------------------------------------------

TEST(TaskDefinition, BraceInit) {
  TaskDefinition td({{"x", BlobDefinition::FromData("data_x")}, {"y", BlobDefinition::FromBlobId("blob-y")}});
  EXPECT_EQ(td.inputs.size(), 2u);
  EXPECT_TRUE(td.inputs.at("x").IsRawData());
  EXPECT_EQ(td.inputs.at("x").GetData(), "data_x");
  EXPECT_FALSE(td.inputs.at("y").IsRawData());
  EXPECT_EQ(td.inputs.at("y").GetBlobId(), "blob-y");
}

TEST(TaskDefinition, WithInputBuilder) {
  TaskDefinition td;
  td.WithInput("a", BlobDefinition::FromData("1")).WithInput("b", BlobDefinition::FromData("2"));

  EXPECT_EQ(td.inputs.size(), 2u);
  EXPECT_EQ(td.inputs.at("a").GetData(), "1");
  EXPECT_EQ(td.inputs.at("b").GetData(), "2");
}

TEST(TaskDefinition, NoInputs) {
  TaskDefinition td;
  EXPECT_TRUE(td.inputs.empty());
}

TEST(TaskDefinition, WithInputOverwritesExisting) {
  TaskDefinition td;
  td.WithInput("k", BlobDefinition::FromData("v1"));
  td.WithInput("k", BlobDefinition::FromData("v2")); // same key, different value
  // std::map::emplace does not overwrite — first insertion wins
  EXPECT_EQ(td.inputs.at("k").GetData(), "v1");
}

// ---------------------------------------------------------------------------
// ConventionPayload (JSON format shared with the other SDKs)
// ---------------------------------------------------------------------------

// The payload holds only "inputs" and "outputs", as in the C# SDK: the method is a task option
TEST(ConventionPayload, SerializesInputsAndOutputsOnly) {
  ConventionPayload payload;
  payload.inputs = {{"a", "blob-a"}};
  payload.outputs = {{"q", "blob-q"}, {"r", "blob-r"}};

  auto j = nlohmann::json::parse(payload.Serialize());
  EXPECT_EQ(j.size(), 2u);
  EXPECT_EQ(j.at("inputs"), (nlohmann::json{{"a", "blob-a"}}));
  EXPECT_EQ(j.at("outputs"), (nlohmann::json{{"q", "blob-q"}, {"r", "blob-r"}}));
}

TEST(ConventionPayload, RoundTrip) {
  ConventionPayload payload;
  payload.inputs = {{"a", "blob-a"}, {"b", "blob-b"}};
  payload.outputs = {{"q", "blob-q"}};

  auto restored = ConventionPayload::Deserialize(payload.Serialize());
  EXPECT_EQ(restored.inputs, payload.inputs);
  EXPECT_EQ(restored.outputs, payload.outputs);
}

// Payloads from older clients may carry "method": it is ignored
TEST(ConventionPayload, IgnoresUnknownFields) {
  auto restored = ConventionPayload::Deserialize(R"({"method":"m","inputs":{"a":"x"},"outputs":{"q":"y"}})");
  EXPECT_EQ(restored.inputs.at("a"), "x");
  EXPECT_EQ(restored.outputs.at("q"), "y");
}

TEST(ConventionPayload, MissingInputsThrows) {
  EXPECT_THROW(ConventionPayload::Deserialize(R"({"outputs":{}})"), ArmoniKSdkException);
}

// Malformed JSON must surface as ArmoniKSdkException (permanent failure, no retry), never as a raw
// nlohmann exception escaping to the worker.
TEST(ConventionPayload, MalformedJsonThrows) {
  EXPECT_THROW(ConventionPayload::Deserialize("{not json"), ArmoniKSdkException);
  EXPECT_THROW(ConventionPayload::Deserialize(R"({"inputs":{"a":"x"},"outputs":{"q":"y"})"
                                              "trailing"),
               ArmoniKSdkException);
  EXPECT_THROW(ConventionPayload::Deserialize(R"({"inputs":{"a":"x"},"outputs":)"), ArmoniKSdkException);
}

TEST(ConventionPayload, MissingOutputsThrows) {
  EXPECT_THROW(ConventionPayload::Deserialize(R"({"inputs":{}})"), ArmoniKSdkException);
}

TEST(ConventionPayload, WrongValueTypesThrow) {
  // Top-level value is not an object
  EXPECT_THROW(ConventionPayload::Deserialize(R"([1,2,3])"), ArmoniKSdkException);
  // "inputs" is not an object
  EXPECT_THROW(ConventionPayload::Deserialize(R"({"inputs":5,"outputs":{}})"), ArmoniKSdkException);
  // Blob ids must be strings
  EXPECT_THROW(ConventionPayload::Deserialize(R"({"inputs":{"a":1},"outputs":{}})"), ArmoniKSdkException);
  EXPECT_THROW(ConventionPayload::Deserialize(R"({"inputs":{},"outputs":{"q":null}})"), ArmoniKSdkException);
}

TEST(ConventionPayload, EmptyPayloadThrows) {
  EXPECT_THROW(ConventionPayload::Deserialize(""), ArmoniKSdkException);
  EXPECT_THROW(ConventionPayload::Deserialize("{}"), ArmoniKSdkException);
}

// A task with no inputs and no outputs is still a valid payload.
TEST(ConventionPayload, EmptyMapsRoundTrip) {
  ConventionPayload payload;
  auto restored = ConventionPayload::Deserialize(payload.Serialize());
  EXPECT_TRUE(restored.inputs.empty());
  EXPECT_TRUE(restored.outputs.empty());
}

// Large payloads (many entries and large values) must round-trip unchanged.
TEST(ConventionPayload, OversizedPayloadRoundTrip) {
  ConventionPayload payload;
  for (int i = 0; i < 10000; ++i) {
    payload.inputs["in" + std::to_string(i)] = "blob-in-" + std::to_string(i);
    payload.outputs["out" + std::to_string(i)] = "blob-out-" + std::to_string(i);
  }
  const std::string large_value(4 * 1024 * 1024, 'x');
  payload.inputs["large"] = large_value;

  auto restored = ConventionPayload::Deserialize(payload.Serialize());
  EXPECT_EQ(restored.inputs.size(), 10001u);
  EXPECT_EQ(restored.outputs.size(), 10000u);
  EXPECT_EQ(restored.inputs.at("large"), large_value);
  EXPECT_EQ(restored.inputs, payload.inputs);
  EXPECT_EQ(restored.outputs, payload.outputs);
}

// ---------------------------------------------------------------------------
// ConventionTask::Decode edge cases
// ---------------------------------------------------------------------------

TEST(ConventionTask, DecodeMalformedPayloadThrows) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.SetConvention("square");

  EXPECT_THROW(ConventionTask::Decode(opts, "{not json", {}), ArmoniKSdkException);
  EXPECT_THROW(ConventionTask::Decode(opts, "", {}), ArmoniKSdkException);
  EXPECT_THROW(ConventionTask::Decode(opts, R"({"inputs":{"x":"blob-x"}})", {}), ArmoniKSdkException);
}

TEST(ConventionTask, DecodeEmptyPayloadMaps) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.SetConvention("noop");

  const auto task = ConventionTask::Decode(opts, ConventionPayload().Serialize(), {});
  EXPECT_EQ(task.method_name, "noop");
  EXPECT_TRUE(task.inputs.empty());
  EXPECT_TRUE(task.outputs.empty());
}

// A large data dependency must be substituted verbatim, including binary content.
TEST(ConventionTask, DecodeSubstitutesOversizedDependency) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.SetConvention("square");

  std::string large_dep(4 * 1024 * 1024, '\0');
  for (std::size_t i = 0; i < large_dep.size(); ++i) {
    large_dep[i] = static_cast<char>(i % 256);
  }
  ConventionPayload payload;
  payload.inputs = {{"x", "blob-x"}};
  payload.outputs = {{"result", "blob-out"}};

  const auto task = ConventionTask::Decode(opts, payload.Serialize(), {{"blob-x", large_dep}});
  EXPECT_EQ(task.inputs.at("x"), large_dep);
}

TEST(ConventionTask, DecodeMissingVersionThrows) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.options[DynamicLibrary::KeySymbol] = "square";

  EXPECT_THROW(ConventionTask::Decode(opts, ConventionPayload().Serialize(), {}), ArmoniKSdkException);
}

TEST(ConventionTask, DecodeMissingSymbolThrows) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  opts.options[DynamicLibrary::KeyConventionVersion] = DynamicLibrary::ConventionVersion;

  EXPECT_THROW(ConventionTask::Decode(opts, ConventionPayload().Serialize(), {}), ArmoniKSdkException);
}

// ---------------------------------------------------------------------------
// TaskOptions with missing fields
// ---------------------------------------------------------------------------

// A gRPC TaskOptions message with no field set (e.g. sent by a client that only filled the partition)
// must convert to SDK defaults rather than crash, and must not be mistaken for a convention task.
TEST(TaskOptionsMissingFields, FromEmptyGrpcMessage) {
  armonik::api::grpc::v1::TaskOptions raw;
  TaskOptions opts(raw);

  EXPECT_TRUE(opts.application_name.empty());
  EXPECT_TRUE(opts.application_version.empty());
  EXPECT_TRUE(opts.application_namespace.empty());
  EXPECT_TRUE(opts.application_service.empty());
  EXPECT_TRUE(opts.partition_id.empty());
  EXPECT_TRUE(opts.engine_type.empty());
  EXPECT_EQ(opts.priority, 0);
  EXPECT_EQ(opts.max_retries, 0);
  EXPECT_EQ(opts.max_duration.seconds, 0);
  EXPECT_EQ(opts.max_duration.nanos, 0);
  EXPECT_TRUE(opts.options.empty());

  EXPECT_FALSE(ConventionTask::IsConventionTask(opts));
  EXPECT_THROW(opts.GetConventionVersion(), ArmoniKSdkException);
  EXPECT_THROW(opts.GetSymbol(), ArmoniKSdkException);
  EXPECT_THROW(opts.GetDynamicLibrary(), ArmoniKSdkException);
}

TEST(TaskOptionsMissingFields, DefaultArgumentsRoundTripThroughGrpc) {
  TaskOptions opts("app", "1.0", "ns", "svc");
  auto raw = static_cast<armonik::api::grpc::v1::TaskOptions>(opts);
  TaskOptions restored(raw);

  EXPECT_EQ(restored.application_name, "app");
  EXPECT_EQ(restored.application_version, "1.0");
  EXPECT_EQ(restored.application_namespace, "ns");
  EXPECT_EQ(restored.application_service, "svc");
  EXPECT_TRUE(restored.partition_id.empty());
  EXPECT_TRUE(restored.engine_type.empty());
  EXPECT_EQ(restored.priority, 2);
  EXPECT_EQ(restored.max_retries, 3);
  EXPECT_EQ(restored.max_duration.seconds, 300);
  EXPECT_EQ(restored.max_duration.nanos, 0);
  EXPECT_TRUE(restored.options.empty());
}

// The convention keys must survive the gRPC round trip so the worker sees what the client set.
TEST(TaskOptionsMissingFields, ConventionKeysRoundTripThroughGrpc) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  DynamicLibrary lib;
  lib.library_blob_id = "blob-1";
  lib.symbol = "sym";
  opts.SetDynamicLibrary(lib);

  TaskOptions restored(static_cast<armonik::api::grpc::v1::TaskOptions>(opts));
  EXPECT_EQ(restored.GetConventionVersion(), DynamicLibrary::ConventionVersion);
  EXPECT_EQ(restored.GetSymbol(), "sym");
  EXPECT_EQ(restored.GetDynamicLibrary().library_blob_id, "blob-1");
}

// An empty LibraryPath with no LibraryBlobId is as unusable as a missing one.
TEST(TaskOptionsMissingFields, EmptyLibraryPathWithoutBlobIdThrows) {
  TaskOptions opts("app", "1.0", "ns", "svc", "part");
  DynamicLibrary lib;
  lib.symbol = "sym";
  opts.SetDynamicLibrary(lib);

  EXPECT_THROW(opts.GetDynamicLibrary(), ArmoniKSdkException);
}
