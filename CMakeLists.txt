cmake_minimum_required(VERSION 3.15)
project(uzesynth VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dpf/CMakeLists.txt")
  message(FATAL_ERROR "DPF not found. Run: git submodule update --init --recursive")
endif()
add_subdirectory(dpf)

# Plugin formats. AU is only available on macOS.
set(UZESYNTH_TARGETS vst2 vst3 clap lv2 jack)
if(APPLE)
  list(APPEND UZESYNTH_TARGETS au)
endif()

set(UZESYNTH_ENGINE
  src/engine/PatchEdit.cpp
  src/engine/PatchParser.cpp
  src/engine/UzeboxSynth.cpp
  src/engine/UzeboxVoice.cpp
  src/engine/Wavetable.cpp
)

dpf_add_plugin(uzesynth
  TARGETS ${UZESYNTH_TARGETS}
  UI_TYPE opengl
  USE_FILE_BROWSER
  FILES_COMMON ${UZESYNTH_ENGINE}
  FILES_DSP src/plugin/UzeSynthPlugin.cpp
  FILES_UI  src/plugin/UzeSynthUI.cpp
)
target_include_directories(uzesynth PUBLIC src/plugin)

# Command-line renderer (no GUI / plugin dependencies)
option(UZESYNTH_BUILD_TOOLS "Build the uzerender command-line tool" ON)
if(UZESYNTH_BUILD_TOOLS)
  add_executable(uzerender tools/uzerender.cpp ${UZESYNTH_ENGINE})
  set_target_properties(uzerender PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Tests: parse -> write -> parse must not change any patch
enable_testing()
add_executable(uzesynth-roundtrip tests/roundtrip.cpp ${UZESYNTH_ENGINE})
file(GLOB UZESYNTH_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/*.inc")
foreach(f ${UZESYNTH_TEST_FILES})
  get_filename_component(n "${f}" NAME_WE)
  add_test(NAME roundtrip_${n} COMMAND uzesynth-roundtrip "${f}")
endforeach()
