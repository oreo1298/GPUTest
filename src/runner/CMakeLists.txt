add_executable(gt_runner
  main.cpp
  runner.cpp
  events.cpp
  sysreport.cpp
  $<TARGET_OBJECTS:gt_bench_tests>
)
set_target_properties(gt_runner PROPERTIES OUTPUT_NAME "${APP_RUNNER_NAME}")
target_link_libraries(gt_runner PRIVATE gt_bench gt_common gt_rhi gt_telemetry CLI11::CLI11 SDL3::SDL3-static)
gt_set_warnings(gt_runner)
install(TARGETS gt_runner RUNTIME DESTINATION "${GT_INSTALL_BINDIR}")
