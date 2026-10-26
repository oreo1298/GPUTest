include(Shaders)

add_library(gt_shaders STATIC registry.cpp)
target_include_directories(gt_shaders PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
gt_set_warnings(gt_shaders)
gt_compile_shaders(TARGET gt_shaders
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/manifest.txt"
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src"
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
