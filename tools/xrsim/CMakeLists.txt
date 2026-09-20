# xrsim: OpenXR simulator runtime (Windows, Vulkan apps) + its self-test app. Development tools, not part of the game.
# Docs: docs/research/xrsim.md. Built from the main CMakeLists.txt when the Vulkan SDK (with glslc) is present.

set(XRSIM_OPENXR_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/openxr/include)

# --- the runtime DLL + its manifest (activate per process: XR_RUNTIME_JSON=<dir>/xrsim_runtime.json) ---
add_library(xrsim_runtime SHARED
  runtime/rt_main.cpp
  runtime/rt_session.cpp
  runtime/rt_vulkan.cpp
  runtime/rt_input.cpp
  runtime/rt_script.cpp
  runtime/rt_output.cpp
  ${CMAKE_SOURCE_DIR}/src/gt2export/png_deflate.cpp)
target_include_directories(xrsim_runtime PRIVATE ${XRSIM_OPENXR_INCLUDE} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(xrsim_runtime PRIVATE Vulkan::Headers user32 gdi32)
target_compile_definitions(xrsim_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
file(GENERATE OUTPUT $<TARGET_FILE_DIR:xrsim_runtime>/xrsim_runtime.json CONTENT
"{
    \"file_format_version\": \"1.0.0\",
    \"runtime\": {
        \"name\": \"xrsim\",
        \"library_path\": \"./$<TARGET_FILE_NAME:xrsim_runtime>\"
    }
}
")

# --- the Khronos OpenXR loader for xrsim_test's loader path: not vendored; an existing local build is copied next to
# the test (set XRSIM_OPENXR_LOADER to an x64 openxr_loader.dll; empty = direct runtime testing only). ---
set(XRSIM_OPENXR_LOADER "" CACHE FILEPATH "Optional x64 openxr_loader.dll used by xrsim_test")
if(XRSIM_OPENXR_LOADER)
  set(XRSIM_OPENXR_LOADER_USE ${XRSIM_OPENXR_LOADER})
endif()

# --- self-test / demo app ---
set(XRSIM_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${XRSIM_SHADER_DIR})
set(XRSIM_SHADERS)
foreach(shader pattern.vert pattern.frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/xrsim_test/${shader})
  set(out ${XRSIM_SHADER_DIR}/${shader}.inc)
  add_custom_command(OUTPUT ${out} COMMAND Vulkan::glslc -O -mfmt=c -o ${out} ${src} DEPENDS ${src} VERBATIM)
  list(APPEND XRSIM_SHADERS ${out})
endforeach()

add_executable(xrsim_test xrsim_test/main.cpp ${XRSIM_SHADERS})
target_include_directories(xrsim_test PRIVATE ${XRSIM_OPENXR_INCLUDE} ${XRSIM_SHADER_DIR})
target_compile_definitions(xrsim_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(xrsim_test PRIVATE gt2formats Vulkan::Vulkan)
add_dependencies(xrsim_test xrsim_runtime)
if(XRSIM_OPENXR_LOADER_USE)
  message(STATUS "xrsim_test: OpenXR loader ${XRSIM_OPENXR_LOADER_USE}")
  add_custom_command(TARGET xrsim_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XRSIM_OPENXR_LOADER_USE} $<TARGET_FILE_DIR:xrsim_test>/openxr_loader.dll VERBATIM)
else()
  message(STATUS "xrsim_test: no openxr_loader.dll found - the self-test uses the direct runtime path only")
endif()
