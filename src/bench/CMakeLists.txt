add_library(gt_bench STATIC
  registry.cpp
  context.cpp
  micro.cpp
  presenter.cpp
  suites.cpp
  stb_impl.cpp
)
set_source_files_properties(stb_impl.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
target_include_directories(gt_bench PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${CMAKE_SOURCE_DIR}/third_party/stb")
target_link_libraries(gt_bench PUBLIC gt_common gt_rhi gt_telemetry)
gt_set_warnings(gt_bench)

# Tests register themselves statically; an OBJECT library keeps every object file.
add_library(gt_bench_tests OBJECT
  tests/scenes/lattice.cpp
  tests/debug/device_lost.cpp
  tests/diagnostics/vram.cpp
  tests/diagnostics/stress_common.cpp
  tests/diagnostics/artifacts.cpp
  tests/diagnostics/thermal.cpp
  tests/diagnostics/compute_check.cpp
  tests/diagnostics/pcie.cpp
  tests/synthetic/alu.cpp
  tests/synthetic/memory.cpp
  tests/synthetic/transfer.cpp
  tests/synthetic/raster.cpp
)
target_link_libraries(gt_bench_tests PUBLIC gt_bench)
gt_set_warnings(gt_bench_tests)
