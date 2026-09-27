find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)
set(GT2_GLSLC ${Vulkan_GLSLC_EXECUTABLE})
set(GT2VK_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${GT2VK_SHADER_DIR}")
set(GT2VK_SHADER_OUTPUTS)
foreach(shader scene.vert scene.frag scene_cached.frag)
  set(out "${GT2VK_SHADER_DIR}/${shader}.inc")
  add_custom_command(OUTPUT "${out}"
    COMMAND Vulkan::glslc -O -mfmt=c -o "${out}" "${CMAKE_CURRENT_SOURCE_DIR}/src/gt2view/shaders/${shader}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gt2view/shaders/${shader}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/gt2view/shaders/texture_mips.glsl" VERBATIM)
  list(APPEND GT2VK_SHADER_OUTPUTS "${out}")
endforeach()
include(cmake/stereo-shaders.cmake)
add_library(gt2vk STATIC
  src/gt2view/vk_context_sdl.cpp src/gt2view/vk_scene_renderer.cpp src/gt2view/render_capture.cpp
  src/gt2view/decoded_textures.cpp src/gt2view/hd_ui.cpp src/gt2view/stereo_foveation.cpp
  src/gt2view/hud.cpp src/gt2view/particles.cpp src/gt2view/procedural_cockpit.cpp
  src/gt2view/glow.cpp src/gt2view/menu_view.cpp src/gt2view/title_view.cpp
  src/gt2view/panel_view.cpp src/gt2view/race_overlay_screens.cpp src/gt2view/movie_view.cpp
  ${GT2VK_SHADER_OUTPUTS})
include(cmake/vr-hand-assets.cmake)
target_include_directories(gt2vk PRIVATE "${GT2VK_SHADER_DIR}")
target_link_libraries(gt2vk PUBLIC gt2export Vulkan::Vulkan PRIVATE SDL2::SDL2)
add_library(gt2os STATIC src/platform/os/paths_sdl.cpp)
target_include_directories(gt2os PUBLIC src)
target_link_libraries(gt2os PRIVATE SDL2::SDL2)
add_library(gt2inputdev STATIC src/platform/input/input_system_sdl.cpp)
target_link_libraries(gt2inputdev PUBLIC gt2input PRIVATE SDL2::SDL2)
# These mathematical helpers are used by common camera/menu code even in flat mode.
add_library(gt2flatcamera STATIC src/platform/xr/vr_rig.cpp src/platform/xr/vr_driving.cpp)
target_include_directories(gt2flatcamera PUBLIC src)
file(GLOB GT2GAME_CPP CONFIGURE_DEPENDS tools/gt2game/*.cpp)
list(FILTER GT2GAME_CPP EXCLUDE REGEX "/game_window_(win32|xr|android)\\.cpp$")
add_executable(gt2game ${GT2GAME_CPP} ${GT2_DEV_DUMP_SOURCES})
target_link_libraries(gt2game PRIVATE gt2vfs gt2formats gt2export gt2gamelib gt2audio gt2career
  gt2menu gt2shell gt2screens gt2camera gt2arcade gt2vk gt2inputdev gt2os gt2flatcamera SDL2::SDL2)
find_package(Threads REQUIRED)
target_link_libraries(gt2audio PRIVATE Threads::Threads)
if(BUILD_TESTING)
  add_executable(gt2sdlchecks tests/sdl_checks.cpp)
  target_link_libraries(gt2sdlchecks PRIVATE gt2inputdev gt2audio SDL2::SDL2)
  add_test(NAME sdl_platform_checks COMMAND gt2sdlchecks)
endif()
