cmake_minimum_required(VERSION 3.22)
set(PROJECT_NAME ArmoniK.SDK.Worker.UnitTest)

project(${PROJECT_NAME})

include(${CMAKE_CURRENT_SOURCE_DIR}/../Utils.cmake)

find_package(ArmoniK.Api.Worker CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

# Fixture libraries dlopen'ed by the tests.
# Raw fixtures implement the C ABI of ArmoniKSDKInterface.h directly.
function(add_raw_fixture fixture_name fixture_version)
    add_library(${fixture_name} SHARED fixtures/RawFixture.cpp)
    target_include_directories(${fixture_name} PRIVATE "$<TARGET_PROPERTY:ArmoniK.SDK.Worker,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${fixture_name} PRIVATE FIXTURE_VERSION="${fixture_version}" ${ARGN})
    setup_options(${fixture_name})
endfunction()

add_raw_fixture(WorkerUnitTest.FixtureV1 v1)
add_raw_fixture(WorkerUnitTest.FixtureV2 v2)
add_raw_fixture(WorkerUnitTest.FixtureMissingCall v1 FIXTURE_OMIT_CALL)

# SDK fixture: a ServiceBase implementation relying on the default entry points of ArmoniK.SDK.Worker.
add_library(WorkerUnitTest.SdkFixture SHARED fixtures/SdkFixture.cpp)
target_link_libraries(WorkerUnitTest.SdkFixture PRIVATE "-Wl,--whole-archive" ArmoniK.SDK.Worker "-Wl,--no-whole-archive")
target_include_directories(WorkerUnitTest.SdkFixture PRIVATE "$<TARGET_PROPERTY:ArmoniK.SDK.Common,INTERFACE_INCLUDE_DIRECTORIES>")
setup_options(WorkerUnitTest.SdkFixture)

set(FIXTURE_TARGETS WorkerUnitTest.FixtureV1 WorkerUnitTest.FixtureV2 WorkerUnitTest.FixtureMissingCall WorkerUnitTest.SdkFixture)

add_executable(${PROJECT_NAME}
        src/fakes/FakeAgent.cpp
        src/ApplicationManagerTest.cpp
        src/ConventionExecutorTest.cpp
        src/DynamicLibTest.cpp
        src/DynamicWorkerTest.cpp
        src/SdkInterfaceTest.cpp
        src/ServiceManagerTest.cpp
        )
add_dependencies(${PROJECT_NAME} ${FIXTURE_TARGETS})

target_link_libraries(${PROJECT_NAME} PRIVATE
        ArmoniK.SDK.DynamicWorker.Core
        ArmoniK.SDK.Worker
        ArmoniK.SDK.Common
        nlohmann_json::nlohmann_json
        GTest::gtest_main)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(${PROJECT_NAME} PRIVATE
        FIXTURE_V1_PATH="$<TARGET_FILE:WorkerUnitTest.FixtureV1>"
        FIXTURE_V2_PATH="$<TARGET_FILE:WorkerUnitTest.FixtureV2>"
        FIXTURE_MISSING_CALL_PATH="$<TARGET_FILE:WorkerUnitTest.FixtureMissingCall>"
        SDK_FIXTURE_PATH="$<TARGET_FILE:WorkerUnitTest.SdkFixture>")

setup_options(${PROJECT_NAME})

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 OLD)
endif()

# gTest support
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME} PROPERTIES LABELS "unit")
