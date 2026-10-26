# gt_set_warnings(<target>): project warning policy for first-party targets.
function(gt_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
      /wd4324  # structure padded due to alignment specifier
      /wd4127  # conditional expression is constant
    )
    if(GT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wnon-virtual-dtor
      -Woverloaded-virtual -Wformat=2 -Wimplicit-fallthrough -Wmissing-declarations
      -Wno-missing-field-initializers)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      target_compile_options(${target} PRIVATE -Wno-dangling-reference)
    endif()
    if(GT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(WIN32)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  endif()
endfunction()
