cmake_minimum_required(VERSION 3.22)
set(PROJECT_NAME ArmoniK.SDK.Client.Test)
set(UNIT_TEST_TARGET ArmoniK.SDK.Client.UnitTest)
set(E2E_TEST_TARGET ArmoniK.SDK.Client.E2ETest)

project(${PROJECT_NAME})

SET(SOURCES_FILES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
SET(HEADER_FILES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Unit tests: no ArmoniK cluster required
set(UNIT_TEST_SOURCES
	${SOURCES_FILES_DIR}/SerializationTest.cpp
	${SOURCES_FILES_DIR}/ThreadPoolTest.cpp
)

# End-to-end tests: require a deployed ArmoniK (GrpcClient__Endpoint), skipped otherwise
set(E2E_TEST_SOURCES
	${SOURCES_FILES_DIR}/ArmoniK.SDK.Client.Test.cpp
	${SOURCES_FILES_DIR}/SessionHandlingTest.cpp
	${SOURCES_FILES_DIR}/BudgetTests.cpp
	${SOURCES_FILES_DIR}/End2EndHandlers.cpp
)

find_package(ArmoniK.Api.Client CONFIG REQUIRED)

if(NOT TARGET ArmoniK.SDK.Common)
	find_package(ArmoniK.SDK.Common CONFIG REQUIRED)
endif()
if(NOT TARGET ArmoniK.SDK.Client)
	find_package(ArmoniK.SDK.Client CONFIG REQUIRED)
endif()

if(POLICY CMP0135)
	cmake_policy(SET CMP0135 OLD)
endif()

# gTest support
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
include(${CMAKE_CURRENT_SOURCE_DIR}/../Utils.cmake)
split_version(${VERSION})

add_executable(${UNIT_TEST_TARGET} ${UNIT_TEST_SOURCES})
add_executable(${E2E_TEST_TARGET} ${E2E_TEST_SOURCES})

foreach(test_target ${UNIT_TEST_TARGET} ${E2E_TEST_TARGET})
	target_link_libraries(${test_target} PUBLIC ArmoniK.Api.Common ArmoniK.Api.Client ArmoniK.SDK.Common ArmoniK.SDK.Client)
	target_link_libraries(${test_target} PRIVATE GTest::gtest_main)
	target_include_directories(${test_target}
			PUBLIC
			"$<BUILD_INTERFACE:${HEADER_FILES_DIR}>"
	)
	setup_options(${test_target})
	setup_lib_version(${test_target})
endforeach()

FILE(COPY appsettings.json DESTINATION "${PROJECT_BINARY_DIR}")

gtest_discover_tests(${UNIT_TEST_TARGET} PROPERTIES LABELS "unit" TIMEOUT 60)
gtest_discover_tests(${E2E_TEST_TARGET} PROPERTIES LABELS "e2e")

install(TARGETS ${UNIT_TEST_TARGET} ${E2E_TEST_TARGET}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_BINDIR})
