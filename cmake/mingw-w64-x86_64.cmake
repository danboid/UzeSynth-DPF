# Cross-compile UzeSynth for 64-bit Windows with MinGW-w64 (e.g. from Linux):
#
#   sudo apt install mingw-w64
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win
#
# DPF's OpenGL UI needs <GL/glext.h>, which MinGW provides (MSVC does not).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX x86_64-w64-mingw32)

# The "-posix" compilers use the POSIX thread model, which std::mutex needs
find_program(MINGW_CC  NAMES ${MINGW_PREFIX}-gcc-posix ${MINGW_PREFIX}-gcc REQUIRED)
find_program(MINGW_CXX NAMES ${MINGW_PREFIX}-g++-posix ${MINGW_PREFIX}-g++ REQUIRED)
set(CMAKE_C_COMPILER   ${MINGW_CC})
set(CMAKE_CXX_COMPILER ${MINGW_CXX})
set(CMAKE_RC_COMPILER  ${MINGW_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Link libgcc, libstdc++ and winpthread statically so the plugins don't need
# any MinGW DLLs on the user's machine
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static")

# Building the LV2 version runs a small generator program on the freshly built
# plugin. When cross-compiling that is a Windows .exe, so run it with Wine if
# installed (sudo apt install wine64). Without Wine, LV2 is skipped.
find_program(WINE_EXECUTABLE NAMES wine64 wine PATHS /usr/lib/wine /usr/lib/wine64 /opt/wine-stable/bin)
if(WINE_EXECUTABLE)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${WINE_EXECUTABLE})
endif()
