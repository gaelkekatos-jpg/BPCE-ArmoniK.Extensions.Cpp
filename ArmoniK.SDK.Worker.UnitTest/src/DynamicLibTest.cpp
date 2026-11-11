#include "DynamicLib.h"
#include "TestHelpers.h"
#include <armonik/sdk/common/ArmoniKSdkException.h>
#include <armonik/sdk/worker/ArmoniKSDKInterface.h>
#include <gtest/gtest.h>

using ArmoniK::Sdk::Common::ArmoniKSdkException;
using ArmoniK::Sdk::DynamicWorker::DynamicLib;
using ArmoniK::Sdk::Worker::UnitTest::TempDir;
using ArmoniK::Sdk::Worker::UnitTest::WriteFile;

TEST(DynamicLib, DefaultConstructedHoldsNoLibrary) {
  DynamicLib lib;
  EXPECT_FALSE(lib);
  EXPECT_NO_THROW(lib.unload());
  EXPECT_THROW(lib.get("armonik_call"), ArmoniKSdkException);
}

TEST(DynamicLib, NonExistentPathThrows) {
  try {
    DynamicLib lib("/nonexistent/libarmonik-missing.so");
    FAIL() << "Expected ArmoniKSdkException";
  } catch (const ArmoniKSdkException &e) {
    EXPECT_NE(std::string(e.what()).find("Could not dlopen /nonexistent/libarmonik-missing.so"), std::string::npos);
  }
}

TEST(DynamicLib, NonLibraryFileThrows) {
  TempDir dir;
  WriteFile(dir.file("not-a-library.so"), "definitely not an ELF file");
  EXPECT_THROW(DynamicLib(dir.file("not-a-library.so").c_str()), ArmoniKSdkException);
}

TEST(DynamicLib, ResolvesExportedSymbols) {
  DynamicLib lib(FIXTURE_V1_PATH);
  ASSERT_TRUE(lib);
  EXPECT_NE(lib.get<armonik_create_service_t>("armonik_create_service"), nullptr);
  EXPECT_NE(lib.get<armonik_call_t>("armonik_call"), nullptr);
}

TEST(DynamicLib, MissingSymbolThrows) {
  DynamicLib lib(FIXTURE_MISSING_CALL_PATH);
  EXPECT_NE(lib.get("armonik_create_service"), nullptr);
  try {
    lib.get("armonik_call");
    FAIL() << "Expected ArmoniKSdkException";
  } catch (const ArmoniKSdkException &e) {
    EXPECT_NE(std::string(e.what()).find("armonik_call"), std::string::npos);
  }
}

TEST(DynamicLib, UnloadReleasesLibrary) {
  DynamicLib lib(FIXTURE_V1_PATH);
  lib.unload();
  EXPECT_FALSE(lib);
  EXPECT_THROW(lib.get("armonik_call"), ArmoniKSdkException);
  EXPECT_NO_THROW(lib.unload());
}

TEST(DynamicLib, MoveTransfersOwnership) {
  DynamicLib source(FIXTURE_V1_PATH);
  DynamicLib moved(std::move(source));
  EXPECT_FALSE(source);
  ASSERT_TRUE(moved);
  EXPECT_NE(moved.get("armonik_call"), nullptr);

  DynamicLib assigned;
  assigned = std::move(moved);
  EXPECT_TRUE(assigned);
  EXPECT_FALSE(moved);
}
