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
    vulkan/vk_raytracing.cpp
    vulkan/vk_bindless.cpp
    vulkan/vk_caps.cpp
    vulkan/vk_vma.cpp)
  list(APPEND _rhi_defs GT_HAS_VULKAN=1)
  list(APPEND _rhi_libs gt_volk_headers gt_vma)
endif()

if(GT_ENABLE_D3D12)
  list(APPEND _rhi_sources
    d3d12/d3d12_util.cpp
    d3d12/d3d12_device.cpp
    d3d12/d3d12_resources.cpp
    d3d12/d3d12_pipeline.cpp
    d3d12/d3d12_commandlist.cpp
    d3d12/d3d12_raytracing.cpp
    d3d12/d3d12_swapchain.cpp)
  list(APPEND _rhi_defs GT_HAS_D3D12=1)
  # Headers only: d3d12.dll and dxgi.dll are loaded at run time.
  list(APPEND _rhi_libs Microsoft::DirectX-Headers Microsoft::DirectX-Guids)
endif()

if(GT_ENABLE_OPENGL)
  list(APPEND _rhi_sources
    gl/gl_util.cpp
    gl/gl_device.cpp
    gl/gl_resources.cpp
    gl/gl_pipeline.cpp
    gl/gl_commandlist.cpp
    gl/gl_swapchain.cpp)
  # Entry points are loaded at run time through SDL; nothing links against libGL / opengl32.
  list(APPEND _rhi_defs GT_HAS_OPENGL=1)
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
