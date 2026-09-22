cmake_minimum_required(VERSION 3.22)
project(gt2pc LANGUAGES C CXX)
option(GT2_BUILD_XRSIM "Build the optional OpenXR development simulator" OFF)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/W4 /WX /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-const-variable)
endif()

# --- asset access and formats (no game code involved) ---
add_library(gt2vfs STATIC
  src/gt2vfs/inflate.cpp
  src/gt2vfs/disc_image.cpp
  src/gt2vfs/gtfs.cpp)
target_include_directories(gt2vfs PUBLIC src)

add_library(gt2formats STATIC
  src/gt2formats/boot_images.cpp
  src/gt2formats/hd_media.cpp
  src/gt2formats/backdrop.cpp
  src/gt2formats/car_info.cpp
  src/gt2formats/car_model.cpp
  src/gt2formats/car_params.cpp
  src/gt2formats/car_texture.cpp
  src/gt2formats/psx_vram.cpp
  src/gt2formats/track.cpp
  src/gt2formats/course_data.cpp
  src/gt2formats/course_map.cpp
  src/gt2formats/overlay_data.cpp
  src/gt2formats/sound_bank.cpp
  src/gt2formats/hud_assets.cpp
  src/gt2formats/sponsor_boards.cpp
  src/gt2formats/json.cpp
  src/gt2formats/car_json.cpp
  src/gt2formats/track_json.cpp
  src/gt2formats/png_reader.cpp
  src/gt2formats/gltf_reader.cpp
  src/gt2formats/license_data.cpp
  src/gt2formats/gtmode_tables.cpp
  src/gt2formats/save_data.cpp
  src/gt2formats/xa_audio.cpp
  src/gt2formats/str_video.cpp
  src/gt2formats/gt_menu.cpp
  src/gt2formats/gt_menu_images.cpp
  src/gt2formats/gt_menu_list.cpp
  src/gt2formats/seqg.cpp
  src/gt2formats/title_assets.cpp
  src/gt2formats/replay.cpp
  src/gt2formats/replay_card.cpp
  src/gt2formats/exe_map.cpp
  src/gt2formats/exe_profile.cpp
  src/gt2formats/arcade_data.cpp
  src/gt2formats/arcade_menu_data.cpp
  src/gt2formats/sha1.cpp)
target_include_directories(gt2formats PUBLIC src)
target_link_libraries(gt2formats PUBLIC gt2vfs)

add_library(gt2export STATIC
  src/gt2export/png_writer.cpp
  src/gt2export/car_mesh.cpp
  src/gt2export/car_gltf.cpp
  src/gt2export/car_preview.cpp
  src/gt2export/track_preview.cpp
  src/gt2export/track_gltf.cpp
  src/gt2export/png_deflate.cpp)
target_include_directories(gt2export PUBLIC src)
target_link_libraries(gt2export PUBLIC gt2formats)

add_executable(gt2tool tools/gt2tool/main.cpp tools/gt2tool/menu_cmds.cpp tools/gt2tool/profile_cmds.cpp tools/gt2tool/arcade_cmds.cpp tools/gt2tool/track_cmds.cpp tools/gt2tool/movie_cmds.cpp tools/gt2tool/replay_cmds.cpp)
target_link_libraries(gt2tool PRIVATE gt2vfs gt2formats gt2export gt2career)
add_executable(gt2install tools/gt2install/main.cpp)
target_link_libraries(gt2install PRIVATE gt2vfs gt2formats)
add_executable(gt2media tools/gt2media/main.cpp)
target_link_libraries(gt2media PRIVATE gt2formats gt2export)
if(WIN32)
  add_executable(gt2bootcapture tools/gt2bootcapture/main.cpp)
  target_link_libraries(gt2bootcapture PRIVATE gt2formats)
endif()

# --- guest runtime: interpreter, HLE kernel, hardware boundary ---
add_library(gt2interp STATIC
  src/interp/r3000.cpp
  src/interp/gte.cpp
  src/machine/machine.cpp
  src/machine/cdrom.cpp
  src/machine/gpu.cpp
  src/machine/spu.cpp
  src/machine/memcard.cpp
  src/machine/mdec.cpp)
target_link_libraries(gt2interp PUBLIC gt2vfs gt2formats)
target_include_directories(gt2interp PUBLIC src)

# --- scene extraction: running game -> camera + car transforms for the native renderer ---
add_library(gt2scene STATIC src/scene/scene_extractor.cpp)
target_link_libraries(gt2scene PUBLIC gt2vfs gt2formats gt2export gt2interp)

add_executable(gt2run tools/gt2run/main.cpp tools/gt2run/scene_probe.cpp tools/gt2run/car_probe.cpp tools/gt2run/watch.cpp tools/gt2run/calltrace.cpp tools/gt2run/session.cpp tools/gt2run/movie_check.cpp)
target_link_libraries(gt2run PRIVATE gt2vfs gt2interp gt2export gt2scene)

# --- the game's own code (product). Must never link gt2interp. ---
add_library(gt2gamelib STATIC
  src/game/sim/car_body.cpp
  src/game/sim/track_collision.cpp
  src/game/sim/drive_shafts.cpp
  src/game/sim/tyres.cpp
  src/game/sim/drivetrain.cpp
  src/game/sim/ground.cpp
  src/game/sim/car_contact.cpp
  src/game/sim/physics_core.cpp
  src/game/sim/car_setup.cpp
  src/game/sim/race_sim.cpp
  src/game/sim/race_shell.cpp
  src/game/sim/ai_driver.cpp
  src/game/sim/disc_data.cpp)
target_link_libraries(gt2gamelib PUBLIC gt2formats)

# The game's sound (product): the ported car sound logic, the native mixer, the Windows device. Never links gt2interp.
if(ANDROID)
  set(GT2_AUDIO_DEVICE src/game/audio/audio_device_aaudio.cpp)
else()
  set(GT2_AUDIO_DEVICE src/game/audio/audio_device_win32.cpp)
endif()
add_library(gt2audio STATIC
  src/game/audio/car_sound.cpp
  src/game/audio/mixer.cpp
  ${GT2_AUDIO_DEVICE}
  src/game/audio/race_audio.cpp
  src/game/audio/music_player.cpp
  src/game/audio/race_music.cpp
  src/game/audio/sequencer.cpp
  src/game/audio/menu_audio.cpp
  src/game/audio/menu_music.cpp)
target_link_libraries(gt2audio PUBLIC gt2gamelib gt2camera gt2vfs)
if(WIN32)
  target_link_libraries(gt2audio PUBLIC winmm)
elseif(ANDROID)
  target_link_libraries(gt2audio PUBLIC aaudio)
endif()

# The GT-mode career (product): career state + save game, garage / money / parts rules, results, events. Never links gt2interp.
add_library(gt2career STATIC
  src/game/career/career_state.cpp
  src/game/career/cheats.cpp
  src/game/career/garage.cpp
  src/game/career/results.cpp
  src/game/career/events.cpp
  src/game/career/tuning.cpp
  src/game/career/championship.cpp
  src/game/career/machine_test.cpp)
target_link_libraries(gt2career PUBLIC gt2gamelib gt2vfs)

# The GT-mode menu runtime (product): pages, cursor navigation, actions, dynamic texts. No rendering API, never gt2interp.
add_library(gt2menu STATIC
  src/game/menu/menu_nav.cpp
  src/game/menu/menu_runtime.cpp
  src/game/menu/menu_car.cpp)
target_link_libraries(gt2menu PUBLIC gt2career gt2formats)

# The title overlay's shell (product): title menu, options, memory-card manager rules and their frames. Never gt2interp.
add_library(gt2shell STATIC
  src/game/shell/title_options.cpp
  src/game/shell/title_menu.cpp
  src/game/shell/title_attract.cpp
  src/game/shell/title_copy.cpp
  src/game/shell/title_draw.cpp
  src/game/shell/title_screens.cpp
  src/game/shell/title_replay.cpp
  src/game/shell/title_transfer.cpp
  src/game/shell/key_config.cpp
  src/game/shell/analog_config.cpp
  src/game/shell/arcade_title.cpp)
target_link_libraries(gt2shell PUBLIC gt2career gt2formats)

# The arcade disc's menus (product): the race build of the arcade menus and their availability rules (US Arcade v1.1
# member 2), the menus' state machine and frames. Never gt2interp.
add_library(gt2arcade STATIC
  src/game/arcade/arcade_setup.cpp
  src/game/arcade/arcade_results.cpp
  src/game/arcade/arcade_menus.cpp
  src/game/arcade/arcade_widgets.cpp
  src/game/arcade/arcade_battle.cpp
  src/game/arcade/arcade_car_page.cpp
  src/game/arcade/arcade_course_page.cpp
  src/game/arcade/arcade_bonus.cpp
  src/game/arcade/arcade_card.cpp)
target_link_libraries(gt2arcade PUBLIC gt2shell gt2menu gt2career gt2formats gt2screens)

# The race overlay's full-screen menus (product): assets + CPU frame builders (licence menu, event menu, settings). Never gt2interp.
add_library(gt2screens STATIC src/gt2view/race_menus.cpp src/gt2view/race_record_screens.cpp src/gt2view/race_result_screens.cpp src/gt2view/race_session_screens.cpp src/gt2view/race_card_screens.cpp src/gt2view/power_graph.cpp src/gt2view/change_parts.cpp src/gt2view/machine_test_views.cpp src/gt2formats/race_menu_assets.cpp)
target_link_libraries(gt2screens PUBLIC gt2shell gt2career gt2menu gt2formats)

# The race cameras (product): the race overlay's camera object and the executable's camera library it uses. Never gt2interp.
add_library(gt2camera STATIC src/game/camera/race_camera.cpp src/game/camera/game_camera.cpp)
target_link_libraries(gt2camera PUBLIC gt2gamelib gt2formats)

# The controller (product): the PS1 pad as the game reads it (libpad handlers, the race's logical pad, the actuators),
# ported from the original, and the PC devices (XInput, DirectInput, the scripted fake pad). Never gt2interp.
add_library(gt2input STATIC src/platform/input/ps1_pad.cpp src/platform/input/dualsense_report.cpp src/platform/input/wheel.cpp src/platform/input/wheel_profiles.cpp src/platform/input/wheel_setup.cpp src/platform/input/wheel_hardware.cpp)
if(WIN32)
  target_compile_definitions(gt2input PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(gt2input PUBLIC dinput8 dxguid user32)
endif()
target_link_libraries(gt2input PUBLIC gt2formats)
if(BUILD_TESTING)
  add_executable(gt2checks tests/checks.cpp)
  target_link_libraries(gt2checks PRIVATE gt2input gt2arcade gt2gamelib gt2audio gt2shell)
  add_test(NAME regression_checks COMMAND gt2checks)
  add_executable(gt2wheelchecks tests/wheel_checks.cpp)
  target_link_libraries(gt2wheelchecks PRIVATE gt2input gt2gamelib)
  add_test(NAME wheel_checks COMMAND gt2wheelchecks)
endif()
if(WIN32)
  add_library(gt2inputdev STATIC src/platform/input/input_system_win32.cpp src/platform/input/dualsense.cpp)
  target_compile_definitions(gt2inputdev PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(gt2inputdev PUBLIC gt2input dinput8 dxguid hid)
endif()

# DEVELOPMENT ONLY: the simulation constants / race data / car records of a RAM dump, for cross-checking the
# disc-derived data (src/game/sim/disc_data.*). Not part of gt2gamelib: only the programs that opt in compile it.
set(GT2_DEV_DUMP_SOURCES src/game/sim/dev_dump_constants.cpp)

# Differential testing of ported simulation code (src/game/sim) against the original in the interpreter.
add_executable(gt2verify
  tools/gt2verify/main.cpp
  tools/gt2verify/verify_drive_shafts.cpp
  tools/gt2verify/verify_tyres.cpp
  tools/gt2verify/verify_drivetrain.cpp
  tools/gt2verify/verify_ground.cpp
  tools/gt2verify/verify_contact.cpp
  tools/gt2verify/verify_core.cpp
  tools/gt2verify/verify_setup.cpp
  tools/gt2verify/verify_params.cpp
  tools/gt2verify/verify_race.cpp
  tools/gt2verify/verify_ai.cpp
  tools/gt2verify/verify_data.cpp
  tools/gt2verify/verify_shell.cpp
  tools/gt2verify/verify_sound.cpp
  tools/gt2verify/verify_license.cpp
  tools/gt2verify/verify_career.cpp
  tools/gt2verify/verify_menu.cpp
  tools/gt2verify/verify_title.cpp
  tools/gt2verify/verify_title_replay.cpp
  tools/gt2verify/verify_title_transfer.cpp
  tools/gt2verify/verify_camera.cpp
  tools/gt2verify/verify_grid.cpp
  tools/gt2verify/verify_replay.cpp
  tools/gt2verify/verify_arcade_menu.cpp
  tools/gt2verify/verify_arcade_results.cpp
  tools/gt2verify/verify_arcade_title.cpp
  tools/gt2verify/verify_pad.cpp
  tools/gt2verify/verify_wheels.cpp
  tools/gt2verify/verify_battle.cpp
  tools/gt2verify/verify_machine_test.cpp
  ${GT2_DEV_DUMP_SOURCES})
target_link_libraries(gt2verify PRIVATE gt2interp gt2gamelib gt2audio gt2career gt2menu gt2vfs gt2shell gt2camera gt2scene gt2arcade gt2input)

# --- Vulkan renderer and the programs that use it (need the Vulkan SDK: headers, loader, glslc) ---
find_package(Vulkan COMPONENTS glslc)
if(Vulkan_FOUND AND Vulkan_glslc_FOUND AND WIN32)
  set(GT2VK_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
  file(MAKE_DIRECTORY ${GT2VK_SHADER_DIR})
  set(GT2VK_SHADER_OUTPUTS)
  foreach(shader scene.vert scene.frag scene_cached.frag)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/gt2view/shaders/${shader})
    set(out ${GT2VK_SHADER_DIR}/${shader}.inc)
    add_custom_command(OUTPUT ${out}
      COMMAND Vulkan::glslc -O -mfmt=c -o ${out} ${src}
      DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/src/gt2view/shaders/texture_mips.glsl VERBATIM)
    list(APPEND GT2VK_SHADER_OUTPUTS ${out})
  endforeach()

  set(GT2_GLSLC ${Vulkan_GLSLC_EXECUTABLE})
  include(cmake/stereo-shaders.cmake)
  add_library(gt2vk STATIC src/gt2view/vk_context.cpp src/gt2view/vk_scene_renderer.cpp src/gt2view/render_capture.cpp src/gt2view/decoded_textures.cpp src/gt2view/hd_ui.cpp src/gt2view/stereo_foveation.cpp src/gt2view/hud.cpp src/gt2view/particles.cpp src/gt2view/glow.cpp src/gt2view/menu_view.cpp src/gt2view/title_view.cpp src/gt2view/panel_view.cpp src/gt2view/race_overlay_screens.cpp src/gt2view/movie_view.cpp ${GT2VK_SHADER_OUTPUTS})
  include(cmake/vr-hand-assets.cmake)
  target_include_directories(gt2vk PRIVATE ${GT2VK_SHADER_DIR})
  target_compile_definitions(gt2vk PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(gt2vk PUBLIC gt2export Vulkan::Vulkan)

  add_executable(gt2renderbench tools/gt2renderbench/main.cpp)
  target_link_libraries(gt2renderbench PRIVATE gt2vk gt2xr)
  add_executable(gt2view src/gt2view/main.cpp)
  target_link_libraries(gt2view PRIVATE gt2vfs gt2formats gt2export gt2vk)

  # The product: native game. Must never link gt2interp (no PS1 code execution inside the game).
  add_executable(gt2game tools/gt2game/main.cpp tools/gt2game/menu_mode.cpp tools/gt2game/title_mode.cpp tools/gt2game/game_window.cpp tools/gt2game/race_common.cpp tools/gt2game/split_race.cpp
  tools/gt2game/race_view.cpp tools/gt2game/career_race.cpp tools/gt2game/panel.cpp tools/gt2game/settings_screen.cpp tools/gt2game/race_menu_check.cpp tools/gt2game/machine_test_check.cpp tools/gt2game/change_parts_check.cpp tools/gt2game/race_result_check.cpp tools/gt2game/arcade_race.cpp tools/gt2game/arcade_mode.cpp tools/gt2game/arcade_title.cpp tools/gt2game/title_attract.cpp tools/gt2game/arcade_post_race.cpp tools/gt2game/ghost_replay.cpp tools/gt2game/arcade_menu_check.cpp tools/gt2game/graphics_options.cpp tools/gt2game/frame_interp.cpp tools/gt2game/mods.cpp tools/gt2game/mod_scene.cpp tools/gt2game/movie_player.cpp ${GT2_DEV_DUMP_SOURCES})
  target_sources(gt2game PRIVATE tools/gt2game/pc_overlay.cpp tools/gt2game/boot_screen.cpp tools/gt2game/game_main.cpp tools/gt2game/game_window_win32.cpp tools/gt2game/game_window_xr.cpp)
  add_library(gt2os STATIC src/platform/os/paths_win32.cpp)
  target_include_directories(gt2os PUBLIC src)
  add_library(gt2xr STATIC src/platform/xr/xr_actions.cpp src/platform/xr/xr_session.cpp src/platform/xr/vr_rig.cpp src/platform/xr/vr_driving.cpp)
  target_include_directories(gt2xr PUBLIC src PRIVATE third_party/openxr/include)
  target_link_libraries(gt2xr PUBLIC Vulkan::Vulkan)
  target_compile_definitions(gt2xr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(gt2game PRIVATE gt2xr gt2os)
  target_link_libraries(gt2game PRIVATE gt2vfs gt2formats gt2export gt2gamelib gt2audio gt2career gt2menu gt2shell gt2screens gt2camera gt2arcade gt2vk gt2inputdev user32 gdi32 winmm dwmapi)

  add_executable(gt2play tools/gt2play/main.cpp)
  target_link_libraries(gt2play PRIVATE gt2vfs gt2interp gt2export gt2scene gt2vk user32 gdi32 winmm)

  # DEVELOPMENT ONLY: xrsim, an OpenXR simulator runtime + self-test (docs/research/xrsim.md). Not part of the game.
  if(GT2_BUILD_XRSIM)
    add_subdirectory(tools/xrsim)
  endif()
else()
  if(WIN32)
    message(FATAL_ERROR "The game requires the Vulkan SDK including glslc. Run INSTALL.bat to install dependencies.")
  endif()
endif()

if(ANDROID)
  include(cmake/android.cmake)
endif()

if(BUILD_TESTING AND NOT ANDROID)
  add_executable(gt2assetchecks tests/asset_checks.cpp)
  target_link_libraries(gt2assetchecks PRIVATE gt2export gt2audio gt2shell)
  add_test(NAME asset_geometry_checks COMMAND gt2assetchecks)
  add_executable(gt2hdchecks tests/hd_checks.cpp)
  target_link_libraries(gt2hdchecks PRIVATE gt2formats gt2shell)
  add_test(NAME hd_media_checks COMMAND gt2hdchecks)
  add_executable(gt2vrchecks tests/vr_checks.cpp src/platform/xr/vr_rig.cpp src/platform/xr/vr_driving.cpp)
  target_include_directories(gt2vrchecks PRIVATE src)
  add_test(NAME vr_rig_checks COMMAND gt2vrchecks)
  add_executable(gt2drivingchecks tests/driving_checks.cpp src/platform/xr/vr_driving.cpp)
  target_link_libraries(gt2drivingchecks PRIVATE gt2formats)
  add_test(NAME vr_driving_checks COMMAND gt2drivingchecks)
  add_executable(gt2questchecks tests/quest_features_checks.cpp)
  target_link_libraries(gt2questchecks PRIVATE gt2career gt2formats)
  add_test(NAME quest_career_checks COMMAND gt2questchecks)
endif()
