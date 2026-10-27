# Third-party dependencies. Everything is fetched at configure time (shallow git
# clones of pinned tags) so no SDK has to be installed to build. Versions and
# licenses are recorded in THIRD_PARTY.md — keep them in sync.
#
# Set GT_DEPS_CACHE (env or cache var) to a directory containing pre-cloned
# sources (<dir>/<name>) to avoid re-cloning for every build directory;
# scripts/fetch-deps.sh populates it.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

if(NOT DEFINED GT_DEPS_CACHE AND DEFINED ENV{GT_DEPS_CACHE})
  set(GT_DEPS_CACHE "$ENV{GT_DEPS_CACHE}")
endif()

# gt_declare(<name> <git-url> <tag>)
macro(gt_declare name url tag)
  string(TOUPPER "${name}" _gt_upper)
  if(GT_DEPS_CACHE AND EXISTS "${GT_DEPS_CACHE}/${name}/.gt-tag")
    file(READ "${GT_DEPS_CACHE}/${name}/.gt-tag" _gt_cached_tag)
    string(STRIP "${_gt_cached_tag}" _gt_cached_tag)
    if(_gt_cached_tag STREQUAL "${tag}")
      set(FETCHCONTENT_SOURCE_DIR_${_gt_upper} "${GT_DEPS_CACHE}/${name}")
    endif()
  endif()
  FetchContent_Declare(${name} GIT_REPOSITORY ${url} GIT_TAG ${tag} GIT_SHALLOW TRUE GIT_PROGRESS FALSE
                       SYSTEM EXCLUDE_FROM_ALL)
  list(APPEND GT_DEPENDENCIES "${name}|${url}|${tag}")
endmacro()

# --- pinned versions (see THIRD_PARTY.md) -------------------------------------
gt_declare(nlohmann_json https://github.com/nlohmann/json.git v3.12.0)
gt_declare(cli11 https://github.com/CLIUtils/CLI11.git v2.7.2)
gt_declare(catch2 https://github.com/catchorg/Catch2.git v3.16.1)
gt_declare(vulkan_headers https://github.com/KhronosGroup/Vulkan-Headers.git vulkan-sdk-1.4.363.0)
gt_declare(volk https://github.com/zeux/volk.git vulkan-sdk-1.4.363.0)
gt_declare(vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.5.0)
gt_declare(sdl3 https://github.com/libsdl-org/SDL.git release-3.4.18)
gt_declare(directx_headers https://github.com/microsoft/DirectX-Headers.git v1.619.5)
gt_declare(spirv_cross https://github.com/KhronosGroup/SPIRV-Cross.git vulkan-sdk-1.4.363.0)
set_property(GLOBAL PROPERTY GT_DEPENDENCIES "${GT_DEPENDENCIES}")

# --- always needed ----------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# --- runner -----------------------------------------------------------------
if(GT_BUILD_RUNNER)
  set(CLI11_PRECOMPILED OFF CACHE INTERNAL "")
  set(CLI11_BUILD_TESTS OFF CACHE INTERNAL "")
  set(CLI11_BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(CLI11_BUILD_DOCS OFF CACHE INTERNAL "")
  set(CLI11_INSTALL OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(cli11)

  # SDL3: static, video only.
  set(SDL_SHARED OFF CACHE INTERNAL "")
  set(SDL_STATIC ON CACHE INTERNAL "")
  set(SDL_TEST_LIBRARY OFF CACHE INTERNAL "")
  set(SDL_EXAMPLES OFF CACHE INTERNAL "")
  set(SDL_TESTS OFF CACHE INTERNAL "")
  set(SDL_INSTALL OFF CACHE INTERNAL "")
  set(SDL_UNINSTALL OFF CACHE INTERNAL "")
  foreach(_sub AUDIO CAMERA JOYSTICK HAPTIC HIDAPI SENSOR POWER DIALOG GPU RENDER TRAY)
    set(SDL_${_sub} OFF CACHE INTERNAL "")
  endforeach()
  set(SDL_VIDEO ON CACHE INTERNAL "")
  # Not used (input injection / console KMS output); skipping them drops build dependencies
  # that desktop distributions do not always install (Arch has no libxtst by default).
  set(SDL_X11_XTEST OFF CACHE INTERNAL "")
  set(SDL_KMSDRM OFF CACHE INTERNAL "")
  set(SDL_VULKAN ON CACHE INTERNAL "")
  set(SDL_OPENGL ON CACHE INTERNAL "")
  set(SDL_WERROR OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(sdl3)

  if(GT_ENABLE_VULKAN)
    FetchContent_MakeAvailable(vulkan_headers)
    FetchContent_GetProperties(volk)
    if(NOT volk_POPULATED)
      FetchContent_Populate(volk)
    endif()
    add_library(gt_volk_headers INTERFACE)
    target_include_directories(gt_volk_headers SYSTEM INTERFACE "${volk_SOURCE_DIR}")
    target_link_libraries(gt_volk_headers INTERFACE Vulkan::Headers)
    target_compile_definitions(gt_volk_headers INTERFACE VK_NO_PROTOTYPES)
    set(GT_VOLK_SOURCE "${volk_SOURCE_DIR}/volk.c" CACHE INTERNAL "")

    FetchContent_GetProperties(vma)
    if(NOT vma_POPULATED)
      FetchContent_Populate(vma)
    endif()
    add_library(gt_vma INTERFACE)
    target_include_directories(gt_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")
  endif()
endif()

# --- OpenGL: SPIRV-Cross (build-time GLSL generator, src/tools/glslgen.cpp) ------
if(GT_ENABLE_OPENGL AND GT_BUILD_RUNNER)
  foreach(_opt CLI ENABLE_TESTS ENABLE_HLSL ENABLE_MSL ENABLE_CPP ENABLE_REFLECT ENABLE_C_API ENABLE_UTIL SHARED)
    set(SPIRV_CROSS_${_opt} OFF CACHE INTERNAL "")
  endforeach()
  set(SPIRV_CROSS_STATIC ON CACHE INTERNAL "")
  set(SPIRV_CROSS_ENABLE_GLSL ON CACHE INTERNAL "")
  set(SPIRV_CROSS_SKIP_INSTALL ON CACHE INTERNAL "")
  FetchContent_MakeAvailable(spirv_cross)
endif()

# --- Direct3D 12 (Windows) ------------------------------------------------------
if(GT_ENABLE_D3D12)
  set(DXHEADERS_BUILD_TEST OFF CACHE INTERNAL "")
  set(DXHEADERS_BUILD_GOOGLE_TEST OFF CACHE INTERNAL "")
  set(DXHEADERS_INSTALL OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(directx_headers)
endif()

# --- tests ------------------------------------------------------------------
if(GT_BUILD_TESTS)
  set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
  set(CATCH_INSTALL_EXTRAS OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
