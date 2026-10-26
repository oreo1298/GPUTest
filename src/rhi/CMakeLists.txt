set(_rhi_sources rhi.cpp)
set(_rhi_defs)
set(_rhi_libs gt_common gt_shaders SDL3::SDL3-static)

if(GT_ENABLE_VULKAN)
  list(APPEND _rhi_sources
    vulkan/vk_util.cpp
    vulkan/vk_instance.cpp
    vulkan/vk_device.cpp
    vulkan/vk_resources.cpp
    vulkan/vk_pipeline.cpp
    vulkan/vk_commandlist.cpp
    vulkan/vk_swapchain.cpp
    vulkan/vk_caps.cpp
    vulkan/vk_vma.cpp)
  list(APPEND _rhi_defs GT_HAS_VULKAN=1)
  list(APPEND _rhi_libs gt_volk_headers gt_vma)
endif()

add_library(gt_rhi STATIC ${_rhi_sources})
target_include_directories(gt_rhi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gt_rhi PUBLIC ${_rhi_defs})
target_link_libraries(gt_rhi PUBLIC ${_rhi_libs} PRIVATE ${CMAKE_DL_LIBS})
gt_set_warnings(gt_rhi)

if(GT_ENABLE_VULKAN)
  # volk is third-party C code: compile it without our warning flags.
  add_library(gt_volk STATIC "${GT_VOLK_SOURCE}")
  target_link_libraries(gt_volk PUBLIC gt_volk_headers PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(gt_volk PUBLIC VK_ENABLE_BETA_EXTENSIONS=1)
  target_link_libraries(gt_rhi PUBLIC gt_volk)
  # VMA implementation unit is third-party code too.
  set_source_files_properties(vulkan/vk_vma.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
endif()
