add_executable(gt_embed embed.cpp)
gt_set_warnings(gt_embed)

if(TARGET spirv-cross-glsl)
  # SPIR-V -> desktop GLSL for the OpenGL backend (cmake/Shaders.cmake).
  add_executable(gt_glslgen glslgen.cpp)
  target_link_libraries(gt_glslgen PRIVATE spirv-cross-glsl)
  gt_set_warnings(gt_glslgen)
endif()
