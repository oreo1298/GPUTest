# Shader build pipeline: one Slang source language compiled at build time to
#   SPIR-V (Vulkan, all platforms), DXIL (D3D12) and DXBC (D3D11) on Windows,
#   GLSL (OpenGL) via SPIRV-Cross from SPIR-V,
# then embedded into the runner via gt_embed.
#
# shaders/manifest.txt lists one entry point per line:
#   <file.slang> <entry> <stage> <apis> [key=value ...]
# apis: comma list of vk,d3d12,d3d11,gl
# keys: vk=<slang spirv profile, default spirv_1_3>  d3d12=<sm_6_0>  d3d11=<sm_5_0>
#       glsl=<330|430>  defines=<A=1,B>  name=<variant suffix>
# Blob keys at runtime: "<file-without-.slang>:<entry>[@<name>]:<format>".
#
# HLSL sources (<file.hlsl>, apis d3d12 only) hold what Slang cannot express (work-graph node
# shaders). They are compiled with DXC on Windows when one new enough is found ($DXC_DIR first);
# d3d12=<DXC target profile, e.g. lib_6_8 or cs_6_8>; a lib_* target exports every entry point.

find_program(GT_SLANGC slangc HINTS "$ENV{SLANG_DIR}/bin" /opt/slang/current/bin REQUIRED)
message(STATUS "Slang compiler: ${GT_SLANGC}")

set(GT_DXC "")
if(WIN32 AND GT_ENABLE_D3D12)
  find_program(GT_DXC_EXE dxc HINTS "$ENV{DXC_DIR}" "$ENV{DXC_DIR}/bin/x64" NO_DEFAULT_PATH)
  if(NOT GT_DXC_EXE)
    find_program(GT_DXC_EXE dxc)
  endif()
  if(GT_DXC_EXE)
    execute_process(COMMAND "${GT_DXC_EXE}" --version OUTPUT_VARIABLE _dxc_ver ERROR_VARIABLE _dxc_ver)
    # "dxcompiler.dll: 1.9 - 1.9.2609.5 ..."; shader model 6.8 (work graphs) needs 1.8 or later.
    set(_dxc_ok OFF)
    if(_dxc_ver MATCHES "dxcompiler\\.dll: ([0-9]+)\\.([0-9]+)")
      set(_dxc_major ${CMAKE_MATCH_1})
      set(_dxc_minor ${CMAKE_MATCH_2})
      if(_dxc_major GREATER 1 OR _dxc_minor GREATER_EQUAL 8)
        set(_dxc_ok ON)
      endif()
    endif()
    if(_dxc_ok)
      set(GT_DXC "${GT_DXC_EXE}")
      message(STATUS "DXC (HLSL shaders): ${GT_DXC} (${_dxc_major}.${_dxc_minor})")
    else()
      string(STRIP "${_dxc_ver}" _dxc_ver)
      message(STATUS "DXC at ${GT_DXC_EXE} ('${_dxc_ver}') is older than 1.8: HLSL shaders (work graphs) are not built")
    endif()
  else()
    message(STATUS "DXC not found (set DXC_DIR): HLSL shaders (work graphs) are not built")
  endif()
endif()

set(GT_VK_SHIFTS -fvk-b-shift 0 0 -fvk-t-shift 16 0 -fvk-u-shift 32 0 -fvk-s-shift 48 0)

function(gt_compile_shaders)
  cmake_parse_arguments(ARG "" "TARGET;MANIFEST;SOURCE_DIR;OUTPUT_DIR" "" ${ARGN})
  file(STRINGS "${ARG_MANIFEST}" _lines)
  set(_embed_args)
  set(_outputs)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ARG_MANIFEST}")
  file(GLOB_RECURSE _all_sources CONFIGURE_DEPENDS "${ARG_SOURCE_DIR}/*.slang")

  foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "" OR _line MATCHES "^#")
      continue()
    endif()
    string(REGEX REPLACE "[ \t]+" ";" _parts "${_line}")
    list(LENGTH _parts _n)
    if(_n LESS 4)
      message(FATAL_ERROR "shader manifest: malformed line '${_line}'")
    endif()
    list(GET _parts 0 _file)
    list(GET _parts 1 _entry)
    list(GET _parts 2 _stage)
    list(GET _parts 3 _apis)
    string(REPLACE "," ";" _apis "${_apis}")
    set(_vk spirv_1_3)
    set(_d3d12 sm_6_0)
    set(_d3d11 sm_5_0)
    set(_glsl 330)
    set(_defines "")
    set(_variant "")
    set(_opts)
    if(_n GREATER 4)
      list(SUBLIST _parts 4 -1 _opts)
    endif()
    foreach(_opt IN LISTS _opts)
      if(_opt MATCHES "^([a-z0-9]+)=(.*)$")
        set(_k "${CMAKE_MATCH_1}")
        set(_v "${CMAKE_MATCH_2}")
        if(_k STREQUAL "vk")
          set(_vk "${_v}")
        elseif(_k STREQUAL "d3d12")
          set(_d3d12 "${_v}")
        elseif(_k STREQUAL "d3d11")
          set(_d3d11 "${_v}")
        elseif(_k STREQUAL "glsl")
          set(_glsl "${_v}")
        elseif(_k STREQUAL "defines")
          set(_defines "${_v}")
        elseif(_k STREQUAL "name")
          set(_variant "@${_v}")
        else()
          message(FATAL_ERROR "shader manifest: unknown option '${_opt}'")
        endif()
      endif()
    endforeach()

    if(_file MATCHES "\\.hlsl$")
      string(REGEX REPLACE "\\.hlsl$" "" _module "${_file}")
      string(REPLACE "/" "_" _flat "${_module}")
      set(_base "${ARG_OUTPUT_DIR}/${_flat}.${_entry}")
      if(GT_DXC AND "d3d12" IN_LIST _apis)
        set(_dxc_entry -E ${_entry})
        if(_d3d12 MATCHES "^lib_")
          set(_dxc_entry)
        endif()
        add_custom_command(OUTPUT "${_base}.dxil"
          COMMAND "${GT_DXC}" "${ARG_SOURCE_DIR}/${_file}" -T ${_d3d12} ${_dxc_entry} -HV 2021 -O3
                  -I "${ARG_SOURCE_DIR}" -Fo "${_base}.dxil"
          DEPENDS "${ARG_SOURCE_DIR}/${_file}"
          COMMENT "dxc ${_module}:${_entry} -> DXIL" VERBATIM)
        list(APPEND _embed_args "${_module}:${_entry}:dxil=${_base}.dxil")
        list(APPEND _outputs "${_base}.dxil")
      endif()
      continue()
    endif()
    string(REGEX REPLACE "\\.slang$" "" _module "${_file}")
    set(_key "${_module}:${_entry}${_variant}")
    string(REPLACE "/" "_" _flat "${_module}")
    string(REPLACE "@" "_" _vflat "${_variant}")
    set(_base "${ARG_OUTPUT_DIR}/${_flat}.${_entry}${_vflat}")
    set(_src "${ARG_SOURCE_DIR}/${_file}")
    set(_defargs)
    if(_defines)
      string(REPLACE "," ";" _deflist "${_defines}")
      foreach(_d IN LISTS _deflist)
        list(APPEND _defargs "-D${_d}")
      endforeach()
    endif()
    set(_common -I "${ARG_SOURCE_DIR}" -entry ${_entry} -stage ${_stage} -matrix-layout-column-major
                ${_defargs} -warnings-disable 39001,41012)

    # SPIR-V: always for vk and gl (GL goes through SPIR-V -> SPIRV-Cross).
    if("vk" IN_LIST _apis OR "gl" IN_LIST _apis)
      add_custom_command(OUTPUT "${_base}.spv"
        COMMAND "${GT_SLANGC}" "${_src}" ${_common} -target spirv -profile ${_vk} ${GT_VK_SHIFTS}
                -O2 -o "${_base}.spv" -depfile "${_base}.spv.d"
        DEPENDS "${_src}" ${_all_sources}
        DEPFILE "${_base}.spv.d"
        COMMENT "slangc ${_key} -> SPIR-V" VERBATIM)
      if("vk" IN_LIST _apis)
        list(APPEND _embed_args "${_key}:spirv=${_base}.spv")
        list(APPEND _outputs "${_base}.spv")
      endif()
    endif()
    if(WIN32 AND "d3d12" IN_LIST _apis AND GT_ENABLE_D3D12)
      add_custom_command(OUTPUT "${_base}.dxil"
        COMMAND "${GT_SLANGC}" "${_src}" ${_common} -target dxil -profile ${_d3d12} -O3
                -o "${_base}.dxil" -depfile "${_base}.dxil.d"
        DEPENDS "${_src}" ${_all_sources}
        DEPFILE "${_base}.dxil.d"
        COMMENT "slangc ${_key} -> DXIL" VERBATIM)
      list(APPEND _embed_args "${_key}:dxil=${_base}.dxil")
      list(APPEND _outputs "${_base}.dxil")
    endif()
    if(WIN32 AND "d3d11" IN_LIST _apis AND GT_ENABLE_D3D11)
      add_custom_command(OUTPUT "${_base}.dxbc"
        COMMAND "${GT_SLANGC}" "${_src}" ${_common} -target dxbc -profile ${_d3d11} -O3
                -o "${_base}.dxbc" -depfile "${_base}.dxbc.d"
        DEPENDS "${_src}" ${_all_sources}
        DEPFILE "${_base}.dxbc.d"
        COMMENT "slangc ${_key} -> DXBC" VERBATIM)
      list(APPEND _embed_args "${_key}:dxbc=${_base}.dxbc")
      list(APPEND _outputs "${_base}.dxbc")
    endif()
    if("gl" IN_LIST _apis AND GT_ENABLE_OPENGL AND GT_SPIRV_CROSS)
      add_custom_command(OUTPUT "${_base}.glsl"
        COMMAND "${GT_SPIRV_CROSS}" "${_base}.spv" --version ${_glsl} --no-es --output "${_base}.glsl"
        DEPENDS "${_base}.spv" "${GT_SPIRV_CROSS}"
        COMMENT "spirv-cross ${_key} -> GLSL ${_glsl}" VERBATIM)
      list(APPEND _embed_args "${_key}:glsl=${_base}.glsl")
      list(APPEND _outputs "${_base}.glsl")
    endif()
  endforeach()

  set(_gen "${ARG_OUTPUT_DIR}/embedded_shaders.cpp")
  # Arguments go through a response-file-free command line; keep keys short.
  add_custom_command(OUTPUT "${_gen}"
    COMMAND gt_embed "${_gen}" gt::shaders::detail kBlobs ${_embed_args}
    DEPENDS gt_embed ${_outputs} "${ARG_MANIFEST}"
    COMMENT "Embedding shader blobs" VERBATIM)
  target_sources(${ARG_TARGET} PRIVATE "${_gen}")
endfunction()
