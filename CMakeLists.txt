cmake_minimum_required(VERSION 3.24)
project(mxl-fabrics-agent VERSION 1.7.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MFA_BUILD_TESTS "Build unit tests" ON)
option(MFA_WITH_NMOS "Link Sony nmos-cpp for the agent Node" OFF)
set(NMOS_CPP_DIR "" CACHE PATH "Path to sony/nmos-cpp Development/")

find_package(Threads REQUIRED)
# rdma_cm headers (librdmacm-dev) for the queue pair settings (src/fabric/qpcontrol.hpp).
find_path(RDMACM_INCLUDE_DIR rdma/rdma_cma.h)

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
    src/mirror/health.cpp
    src/mirror/lifecycle.cpp
    src/mirror/readers.cpp
    src/replication/handshake.cpp
    src/replication/policy.cpp
    src/ops/metrics.cpp
    src/ops/lanemetrics.cpp
    src/ops/httpserver.cpp
    src/peer/manager.cpp
    src/nmos/observer.cpp
)

if(RDMACM_INCLUDE_DIR)
    list(APPEND MFA_CORE_SOURCES src/fabric/qpcontrol.cpp)
endif()

add_library(mfa-core STATIC ${MFA_CORE_SOURCES})
target_include_directories(mfa-core PUBLIC src third_party)
if(RDMACM_INCLUDE_DIR)
    target_include_directories(mfa-core PUBLIC ${RDMACM_INCLUDE_DIR})
endif()
target_link_libraries(mfa-core PUBLIC Threads::Threads)
target_compile_options(mfa-core PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(MFA_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_config.cpp
        tests/unit/test_demand.cpp
        tests/unit/test_lifecycle.cpp
        tests/unit/test_mirror_cleanup.cpp
        tests/unit/test_mirror_health.cpp
        tests/unit/test_handshake.cpp
        tests/unit/test_inventory.cpp
        tests/unit/test_metrics.cpp
        tests/unit/test_policy.cpp
    )
    if(RDMACM_INCLUDE_DIR)
        target_sources(unit-tests PRIVATE tests/unit/test_qpcontrol.cpp)
    endif()
    target_include_directories(unit-tests PRIVATE src third_party)
    target_link_libraries(unit-tests PRIVATE mfa-core)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

find_package(mxl CONFIG)
if(mxl_FOUND)
find_package(PkgConfig REQUIRED)
pkg_check_modules(libfabric REQUIRED IMPORTED_TARGET libfabric)
if(NOT RDMACM_INCLUDE_DIR)
    message(FATAL_ERROR "rdma/rdma_cma.h not found (librdmacm-dev)")
endif()

# The binary's own rdma_connect, rdma_accept, rdma_get_cm_event, rdma_destroy_id and rdma_destroy_ep (src/fabric/rdmahook.cpp):
# exported, so that the dynamic linker binds libfabric's calls to them.
function(mfa_rdma_hooks target)
    target_sources(${target} PRIVATE src/fabric/rdmahook.cpp)
    target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
    foreach(symbol rdma_connect rdma_accept rdma_get_cm_event rdma_destroy_id rdma_destroy_ep)
        target_link_options(${target} PRIVATE "LINKER:--export-dynamic-symbol=${symbol}")
    endforeach()
endfunction()

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
mfa_rdma_hooks(mxl-fabrics-agent)

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

if(MFA_BUILD_TESTS)
    # Lane lifecycle against a real MXL domain.
    add_executable(lane-tests tests/lanes/test_lanes.cpp src/fabric/domain.cpp src/replication/engine.cpp)
    target_include_directories(lane-tests PRIVATE src third_party)
    target_link_libraries(lane-tests PRIVATE mfa-core mxl::mxl mxl::mxl-fabrics PkgConfig::libfabric)
    mfa_rdma_hooks(lane-tests)
    add_test(NAME lane-tests COMMAND lane-tests)
endif()

add_executable(mxl-pattern-writer tests/tools/pattern_writer.cpp)
target_link_libraries(mxl-pattern-writer PRIVATE mxl::mxl)
target_compile_features(mxl-pattern-writer PRIVATE cxx_std_20)

# LD_PRELOAD fault injection for tests/integration/accept_fail.sh (not installed).
add_library(mfa-accept-fault SHARED tests/tools/accept_fault.cpp)
target_link_libraries(mfa-accept-fault PRIVATE ${CMAKE_DL_LIBS})

install(TARGETS mxl-fabrics-agent mxl-pattern-writer RUNTIME DESTINATION bin)
else()
    message(STATUS "mxl not found; building unit tests only")
endif()
