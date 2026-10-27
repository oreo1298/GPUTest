# Upscaler plugins (gt/bench/upscaler_plugin.h). Each builds a shared library into bin/plugins/.
function(gt_upscaler_plugin target id)
  add_library(${target} MODULE ${ARGN})
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME "upscaler-${id}" PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")
  foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_cfg} _cfg_upper)
    set_target_properties(${target} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_cfg_upper} "${CMAKE_BINARY_DIR}/bin/plugins"
      RUNTIME_OUTPUT_DIRECTORY_${_cfg_upper} "${CMAKE_BINARY_DIR}/bin/plugins")
  endforeach()
  target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src/bench/gt/bench")
  gt_set_warnings(${target})
endfunction()

# Test plugin: a Vulkan blit, so CI exercises loading, native handles and evaluation (not packaged).
if(GT_ENABLE_VULKAN AND GT_BUILD_TESTS)
  gt_upscaler_plugin(gt_upscaler_example example example/upscaler_example.cpp)
  target_link_libraries(gt_upscaler_example PRIVATE Vulkan::Headers)
  target_compile_definitions(gt_upscaler_example PRIVATE VK_NO_PROTOTYPES)
endif()

# Intel XeSS (Direct3D 12) from the MIT public headers in third_party/xess. Inert until the user enables
# plugins and installs libxess.dll (docs/PLUGINS.md); the XeSS runtime is not distributed.
if(WIN32 AND GT_ENABLE_D3D12)
  option(GT_BUILD_XESS_PLUGIN "Build the Intel XeSS upscaler plugin" ON)
  if(GT_BUILD_XESS_PLUGIN)
    gt_upscaler_plugin(gt_upscaler_xess xess xess/upscaler_xess.cpp)
    target_include_directories(gt_upscaler_xess SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/xess")
    target_link_libraries(gt_upscaler_xess PRIVATE Microsoft::DirectX-Headers)
    install(TARGETS gt_upscaler_xess LIBRARY DESTINATION "${GT_INSTALL_BINDIR}/plugins")
  endif()
endif()
