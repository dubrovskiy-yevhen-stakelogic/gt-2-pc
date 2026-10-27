cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(gt4import LANGUAGES CXX)
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
endif()

add_library(gt4importformats STATIC
  native/gt4formats/common.cpp
  native/gt4formats/car4.cpp
  native/gt4formats/mdls.cpp
  native/gt4formats/tex1.cpp
  native/game/math/vu0_matrix.cpp
  native/ee/fpu.cpp
  native/ee/vu_float.cpp
  native/specdb/specdb.cpp)
target_include_directories(gt4importformats PUBLIC native)
if(MSVC)
  target_compile_options(gt4importformats PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(gt4importformats PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(gt4assettool asset_main.cpp)
add_executable(gt4nativemetadata ../../scripts/gt4_native_metadata.cpp)
add_executable(gt4materialmetadata ../../scripts/gt4_material_metadata.cpp)
add_executable(gt4specdbexport specdb_export.cpp)
add_library(gt4showroomphysics STATIC
  native/game/menu/garage_car.cpp
  native/game/physics/carparams.cpp
  native/game/physics/carparams_init.cpp
  native/game/physics/carparams_specdb.cpp
  native/game/physics/wheel_ground.cpp
  native/game/physics/steering.cpp
  native/game/physics/drivetrain.cpp
  native/game/physics/engine.cpp
  native/game/math/libm.cpp
  native/game/math/geom.cpp
  native/game/math/trig.cpp
  native/game/math/quat.cpp)
target_link_libraries(gt4showroomphysics PUBLIC gt4importformats)
add_executable(gt4showroomexport showroom_export.cpp)
target_link_libraries(gt4showroomexport PRIVATE gt4showroomphysics)
set_target_properties(gt4showroomexport PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(target IN ITEMS gt4assettool gt4nativemetadata gt4materialmetadata gt4specdbexport)
  target_link_libraries(${target} PRIVATE gt4importformats)
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

add_custom_target(gt4-import-tools DEPENDS gt4assettool gt4nativemetadata gt4materialmetadata gt4specdbexport gt4showroomexport)
