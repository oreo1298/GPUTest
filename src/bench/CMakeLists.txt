add_library(gt_bench STATIC
  registry.cpp
  context.cpp
  micro.cpp
  presenter.cpp
  plugins.cpp
  upscale.cpp
  h264_params.cpp
  h265_params.cpp
  video_streams.cpp
  suites.cpp
  scene.cpp
  stb_impl.cpp
)
set_source_files_properties(stb_impl.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
target_include_directories(gt_bench PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${CMAKE_SOURCE_DIR}/third_party/stb")
target_link_libraries(gt_bench PUBLIC gt_common gt_rhi gt_telemetry)
gt_set_warnings(gt_bench)

# Reference video streams for feature.video (data/video, made by scripts/make-video-streams.py).
set(_video_dir "${CMAKE_SOURCE_DIR}/data/video")
set(_video_files h264=h264_1080p.h264 h265=h265_1080p.h265 av1=av1_1080p.obu streams.json=streams.json)
set(_video_args)
set(_video_deps)
foreach(_entry IN LISTS _video_files)
  string(REPLACE "=" ";" _kv "${_entry}")
  list(GET _kv 0 _key)
  list(GET _kv 1 _file)
  list(APPEND _video_args "${_key}=${_video_dir}/${_file}")
  list(APPEND _video_deps "${_video_dir}/${_file}")
endforeach()
set(_video_cpp "${CMAKE_CURRENT_BINARY_DIR}/embedded_video.cpp")
add_custom_command(OUTPUT "${_video_cpp}"
  COMMAND gt_embed "${_video_cpp}" gt::bench::video_data kStreams ${_video_args}
  DEPENDS gt_embed ${_video_deps}
  COMMENT "Embedding reference video streams" VERBATIM)
target_sources(gt_bench PRIVATE "${_video_cpp}")

# Tests register themselves statically; an OBJECT library keeps every object file.
add_library(gt_bench_tests OBJECT
  tests/scenes/lattice.cpp
  tests/scenes/courtyard.cpp
  tests/scenes/atrium.cpp
  tests/scenes/skyline.cpp
  tests/scenes/glasshouse.cpp
  tests/scenes/gallery.cpp
  tests/scenes/rt_scene.cpp
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
  tests/features/rt_common.cpp
  tests/features/rt_bvh.cpp
  tests/features/rt_rays.cpp
  tests/features/rt_ser.cpp
  tests/features/rt_omm.cpp
  tests/features/mesh_shaders.cpp
  tests/features/vrs.cpp
  tests/features/upscale.cpp
  tests/features/hdr.cpp
  tests/features/video.cpp
  tests/features/video_vk.cpp
  tests/features/video_h264_vk.cpp
  tests/features/video_h265_vk.cpp
  tests/features/sampler_feedback.cpp
  tests/features/work_graphs.cpp
  tests/features/async_queues.cpp
  tests/features/subgroups.cpp
  tests/features/bindless.cpp
  tests/features/pipelines.cpp
  tests/features/drawcalls.cpp
  tests/features/coopmat.cpp
)
target_link_libraries(gt_bench_tests PUBLIC gt_bench)
gt_set_warnings(gt_bench_tests)
