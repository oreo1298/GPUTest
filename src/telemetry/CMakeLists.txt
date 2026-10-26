add_library(gt_telemetry STATIC
  telemetry.cpp
  dynlib.cpp
  nvml.cpp
  linux_sysfs.cpp
  fake.cpp
)
target_include_directories(gt_telemetry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gt_telemetry PUBLIC gt_common PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
gt_set_warnings(gt_telemetry)
