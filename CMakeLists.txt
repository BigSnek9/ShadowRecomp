cmake_minimum_required(VERSION 3.25)
project(ShadowRecomp C CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --- Aurora (GX renderer, window, input) --------------------------------------------------
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
file(TO_CMAKE_PATH "$ENV{USERPROFILE}/miniconda3" _conda)   # keep a user conda out of the package search
set(CMAKE_IGNORE_PREFIX_PATH "${_conda}" CACHE STRING "" FORCE)
add_subdirectory(third_party/aurora ${CMAKE_BINARY_DIR}/aurora EXCLUDE_FROM_ALL)

include(generated/files.cmake)
file(GLOB RT_SOURCES ${CMAKE_CURRENT_LIST_DIR}/runtime/*.cpp)
# The exe icon and launcher art (runtime/shadow.ico, runtime/assets/*.png) come from the game, so they are not in git:
# a fresh clone builds with the neutral placeholders instead. An existing file is never replaced.
foreach(pair "shadow.ico=icon.ico" "assets/emblem.png=emblem.png" "assets/hero.png=hero.png")
  string(REPLACE "=" ";" pair "${pair}")
  list(GET pair 0 dst)
  list(GET pair 1 src)
  if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/runtime/${dst})
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/runtime/assets)
    file(COPY_FILE ${CMAKE_CURRENT_LIST_DIR}/tools/release/placeholder/${src} ${CMAKE_CURRENT_LIST_DIR}/runtime/${dst})
  endif()
endforeach()
# WIN32: a normal Windows program (no console window when double-clicked); runtime/shadow.rc embeds the icon.
# #137 build number: version (VERSION file) + configure time + short git hash -> one generated source file
set(SHADOW_VERSION "dev")
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/VERSION)
  file(READ ${CMAKE_CURRENT_LIST_DIR}/VERSION SHADOW_VERSION)
  string(STRIP "${SHADOW_VERSION}" SHADOW_VERSION)
endif()
string(TIMESTAMP SHADOW_BUILD_DATE "%Y-%m-%d %H:%M")
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE SHADOW_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SHADOW_GIT_HASH)
  set(SHADOW_GIT_HASH "nogit")
else()
  execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                  OUTPUT_VARIABLE SHADOW_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(SHADOW_GIT_DIRTY)
    set(SHADOW_GIT_HASH "${SHADOW_GIT_HASH}+")
  endif()
endif()
configure_file(${CMAKE_CURRENT_LIST_DIR}/runtime/build_info.cpp.in ${CMAKE_BINARY_DIR}/build_info.cpp @ONLY)
add_executable(shadow WIN32 ${RT_SOURCES} ${GEN_SOURCES} runtime/shadow.rc ${CMAKE_BINARY_DIR}/build_info.cpp)
target_include_directories(shadow PRIVATE runtime generated third_party/aurora/lib third_party/aurora/include)
target_compile_options(shadow PRIVATE -O2 -gcodeview -gline-tables-only -fno-strict-aliasing -fno-asynchronous-unwind-tables -w)
target_link_options(shadow PRIVATE -Wl,/STACK:268435456 -Wl,/DEBUG -Wl,/ENTRY:mainCRTStartup)
target_compile_definitions(shadow PRIVATE WEBGPU_DAWN)
target_link_libraries(shadow PRIVATE aurora_gx aurora_core aurora_vi dwmapi dbghelp winmm
    absl::btree absl::flat_hash_map Tracy::TracyClient dawn::webgpu_dawn sqlite3 PNG::PNG)
