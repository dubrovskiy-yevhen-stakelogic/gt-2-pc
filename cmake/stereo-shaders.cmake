foreach(mode stereo stereo_mv stereo_cached stereo_cached_mv)
  set(output "${GT2VK_SHADER_DIR}/scene_${mode}.vert.inc")
  set(defines)
  if(mode MATCHES "_mv$")
    set(defines -DGT2_MULTIVIEW)
  endif()
  if(mode MATCHES "cached")
    list(APPEND defines -DGT2_CACHED)
  endif()
  add_custom_command(OUTPUT "${output}"
    COMMAND "${GT2_GLSLC}" -O -mfmt=c ${defines} -fshader-stage=vertex -o "${output}" "${CMAKE_CURRENT_SOURCE_DIR}/src/gt2view/shaders/scene_stereo.vert"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gt2view/shaders/scene_stereo.vert" VERBATIM)
  list(APPEND GT2VK_SHADER_OUTPUTS "${output}")
endforeach()
