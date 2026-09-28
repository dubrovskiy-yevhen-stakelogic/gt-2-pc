# 0.8.0: compile the native product and shared scene builders to WebAssembly.
# Nothing from gt2interp is linked into the product.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GT2_WEB_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/web-generated")
add_custom_command(OUTPUT "${GT2_WEB_GENERATED}/web_shaders.h"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/web-shaders.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gt2view/shaders" "${GT2_WEB_GENERATED}/web_shaders.h"
  DEPENDS scripts/web-shaders.py src/gt2view/shaders/scene.vert
    src/gt2view/shaders/scene.frag src/gt2view/shaders/texture_mips.glsl VERBATIM)
add_library(gt2webgl STATIC src/gt2view/webgl_scene_renderer.cpp
  src/gt2view/hd_ui.cpp src/gt2view/hud.cpp src/gt2view/particles.cpp
  src/gt2view/procedural_cockpit.cpp src/gt2view/glow.cpp src/gt2view/menu_view.cpp
  src/gt2view/title_view.cpp src/gt2view/panel_view.cpp src/gt2view/race_overlay_screens.cpp
  src/gt2view/movie_view.cpp "${GT2_WEB_GENERATED}/web_shaders.h")
target_include_directories(gt2webgl PRIVATE "${GT2_WEB_GENERATED}")
target_link_libraries(gt2webgl PUBLIC gt2export)
add_library(gt2os STATIC src/platform/os/paths_web.cpp)
target_include_directories(gt2os PUBLIC src)
add_library(gt2inputdev STATIC src/platform/input/input_system_sdl.cpp)
target_link_libraries(gt2inputdev PUBLIC gt2input)
add_library(gt2flatcamera STATIC src/platform/xr/vr_rig.cpp src/platform/xr/vr_driving.cpp)
target_include_directories(gt2flatcamera PUBLIC src)

# Like macOS, use the existing product entry point and every common game screen.
file(GLOB GT2GAME_CPP CONFIGURE_DEPENDS tools/gt2game/*.cpp)
list(FILTER GT2GAME_CPP EXCLUDE REGEX "/game_window_(win32|xr|android|sdl)\\.cpp$")
add_executable(gt2web ${GT2GAME_CPP} ${GT2_DEV_DUMP_SOURCES})
target_link_libraries(gt2web PRIVATE gt2vfs gt2formats gt2export gt2gamelib gt2audio
  gt2career gt2menu gt2shell gt2screens gt2camera gt2arcade gt2webgl
  gt2inputdev gt2os gt2flatcamera)
set_target_properties(gt2web PROPERTIES OUTPUT_NAME gt2 SUFFIX ".js"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/site")
target_link_options(gt2web PRIVATE
  -sUSE_SDL=2 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
  -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=262144 -sSTACK_SIZE=8388608
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sMAXIMUM_MEMORY=2147483648
  -sFORCE_FILESYSTEM=1 -lidbfs.js -sEXIT_RUNTIME=0
  "-sEXPORTED_RUNTIME_METHODS=['callMain','FS','IDBFS']"
  -sMODULARIZE=1 -sEXPORT_NAME=createGT2 -sENVIRONMENT=web
  -sASSERTIONS=1)
set_property(TARGET gt2web APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/gt2web/launcher.js")
foreach(asset index.html launcher.js)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/tools/gt2web/${asset}"
    "${CMAKE_CURRENT_BINARY_DIR}/site/${asset}" COPYONLY)
endforeach()
