include(Catch)

add_executable(gt_unit_tests
  test_stats.cpp
  test_duration.cpp
  test_results.cpp
  test_diagnostics.cpp
  test_scene.cpp
  test_dxbc.cpp
  test_h264.cpp
  test_h265.cpp
  test_av1.cpp
)
target_link_libraries(gt_unit_tests PRIVATE gt_common gt_bench gt_rhi Catch2::Catch2WithMain)
gt_set_warnings(gt_unit_tests)
target_compile_definitions(gt_unit_tests PRIVATE GT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
catch_discover_tests(gt_unit_tests DISCOVERY_MODE PRE_TEST)
