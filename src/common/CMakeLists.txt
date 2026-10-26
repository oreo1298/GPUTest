add_library(gt_common STATIC
  log.cpp
  stats.cpp
  duration.cpp
  paths.cpp
  report.cpp
  diagnostics.cpp
  results.cpp
  scoring.cpp
  sysinfo.cpp
)
target_include_directories(gt_common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(gt_common PUBLIC nlohmann_json::nlohmann_json)
gt_set_warnings(gt_common)
