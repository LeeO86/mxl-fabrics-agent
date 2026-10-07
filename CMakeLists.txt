cmake_minimum_required(VERSION 3.24)
project(mxl-fabrics-agent VERSION 1.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MFA_BUILD_TESTS "Build unit tests" ON)
option(MFA_WITH_NMOS "Link Sony nmos-cpp for the agent Node" OFF)
set(NMOS_CPP_DIR "" CACHE PATH "Path to sony/nmos-cpp Development/")

find_package(Threads REQUIRED)

set(MFA_CORE_SOURCES
    src/util/logging.cpp
    src/util/uuid.cpp
    src/util/net.cpp
    src/util/cidr.cpp
    src/util/threading.cpp
    src/util/httpclient.cpp
    src/config/config.cpp
    src/config/store.cpp
    src/inventory/model.cpp
    src/inventory/scanner.cpp
    src/demand/derive.cpp
    src/mirror/lifecycle.cpp
    src/replication/handshake.cpp
    src/ops/metrics.cpp
    src/ops/httpserver.cpp
    src/peer/manager.cpp
    src/nmos/observer.cpp
)

add_library(mfa-core STATIC ${MFA_CORE_SOURCES})
target_include_directories(mfa-core PUBLIC src third_party)
target_link_libraries(mfa-core PUBLIC Threads::Threads)
target_compile_options(mfa-core PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(MFA_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_config.cpp
        tests/unit/test_demand.cpp
        tests/unit/test_lifecycle.cpp
        tests/unit/test_handshake.cpp
        tests/unit/test_inventory.cpp
        tests/unit/test_metrics.cpp
    )
    target_include_directories(unit-tests PRIVATE src third_party)
    target_link_libraries(unit-tests PRIVATE mfa-core)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

find_package(mxl CONFIG)
if(mxl_FOUND)
find_package(PkgConfig REQUIRED)
pkg_check_modules(libfabric REQUIRED IMPORTED_TARGET libfabric)

set(MFA_APP_SOURCES
    src/fabric/domain.cpp
    src/replication/engine.cpp
    src/reconcile/controller.cpp
    src/main.cpp
)

add_executable(mxl-fabrics-agent ${MFA_APP_SOURCES})
target_include_directories(mxl-fabrics-agent PRIVATE src third_party ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(mxl-fabrics-agent PRIVATE mfa-core mxl::mxl mxl::mxl-fabrics PkgConfig::libfabric)
target_compile_options(mxl-fabrics-agent PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html")
    include(cmake/EmbedFile.cmake)
    mfa_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html" kIndexHtml "${CMAKE_CURRENT_BINARY_DIR}/index_html.hpp")
    target_compile_definitions(mxl-fabrics-agent PRIVATE MFA_HAS_UI=1)
endif()

if(MFA_WITH_NMOS)
    if(NOT NMOS_CPP_DIR)
        message(FATAL_ERROR "MFA_WITH_NMOS=ON requires -DNMOS_CPP_DIR")
    endif()
    set(NMOS_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NMOS_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NMOS_CPP_USE_SUPPLIED_JSON_SCHEMA_VALIDATOR ON CACHE BOOL "" FORCE)
    set(NMOS_CPP_USE_SUPPLIED_JWT_CPP ON CACHE BOOL "" FORCE)
    set(CMAKE_CXX_STANDARD 17)
    add_subdirectory(${NMOS_CPP_DIR} ${CMAKE_BINARY_DIR}/nmos-cpp EXCLUDE_FROM_ALL)
    set(CMAKE_CXX_STANDARD 20)
    target_sources(mxl-fabrics-agent PRIVATE src/nmos/node.cpp)
    target_compile_definitions(mxl-fabrics-agent PRIVATE MFA_WITH_NMOS=1)
    target_link_libraries(mxl-fabrics-agent PRIVATE nmos-cpp::nmos-cpp)
    set_source_files_properties(src/nmos/node.cpp PROPERTIES CXX_STANDARD 17 COMPILE_OPTIONS "-w")
else()
    target_sources(mxl-fabrics-agent PRIVATE src/nmos/node.cpp)
endif()

add_executable(mxl-pattern-writer tests/tools/pattern_writer.cpp)
target_link_libraries(mxl-pattern-writer PRIVATE mxl::mxl)
target_compile_features(mxl-pattern-writer PRIVATE cxx_std_20)

install(TARGETS mxl-fabrics-agent mxl-pattern-writer RUNTIME DESTINATION bin)
else()
    message(STATUS "mxl not found; building unit tests only")
endif()
