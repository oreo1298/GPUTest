cmake_minimum_required(VERSION 3.25)

include(cmake/AppInfo.cmake)

project(${APP_ID}
  VERSION ${APP_VERSION}
  DESCRIPTION "${APP_DESCRIPTION}"
  HOMEPAGE_URL "${APP_HOMEPAGE}"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GT_BUILD_RUNNER "Build the benchmark runner (${APP_RUNNER_NAME})" ON)
option(GT_BUILD_GUI "Build the Qt GUI (${APP_GUI_NAME})" ON)
option(GT_BUILD_TESTS "Build unit tests" ON)
option(GT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(GT_SANITIZE "" CACHE STRING "Sanitizers to enable (e.g. address,undefined)")
option(GT_ENABLE_VULKAN "Build the Vulkan backend" ON)
option(GT_ENABLE_OPENGL "Build the OpenGL backend" ON)
if(WIN32)
  option(GT_ENABLE_D3D12 "Build the Direct3D 12 backend" ON)
  option(GT_ENABLE_D3D11 "Build the Direct3D 11 backend" ON)
else()
  set(GT_ENABLE_D3D12 OFF)
  set(GT_ENABLE_D3D11 OFF)
endif()

if(WIN32)
  set(GT_INSTALL_BINDIR ".")
else()
  include(GNUInstallDirs)
  set(GT_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

find_package(Threads REQUIRED)
include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

find_package(Git QUIET)
set(GT_GIT_REVISION "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE _rev
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rev_res)
  if(_rev_res EQUAL 0 AND _rev)
    set(GT_GIT_REVISION "${_rev}")
  endif()
endif()
configure_file(src/common/app_info.h.in "${CMAKE_BINARY_DIR}/generated/gt/app_info.h" @ONLY)

add_subdirectory(src/tools)
add_subdirectory(src/common)
if(GT_BUILD_RUNNER)
  foreach(_dir shaders src/telemetry src/rhi src/bench)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
      add_subdirectory(${_dir})
    endif()
  endforeach()
  add_subdirectory(src/runner)
  add_subdirectory(src/plugins)
endif()
if(GT_BUILD_GUI)
  add_subdirectory(src/gui)
endif()
if(GT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

add_subdirectory(packaging)

message(STATUS "${APP_DISPLAY_NAME} ${APP_VERSION}: runner=${GT_BUILD_RUNNER} gui=${GT_BUILD_GUI} tests=${GT_BUILD_TESTS} "
               "vulkan=${GT_ENABLE_VULKAN} d3d12=${GT_ENABLE_D3D12} d3d11=${GT_ENABLE_D3D11} gl=${GT_ENABLE_OPENGL} "
               "werror=${GT_WARNINGS_AS_ERRORS} sanitize='${GT_SANITIZE}'")
