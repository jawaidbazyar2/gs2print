cmake_minimum_required(VERSION 3.24)
project(gs2print LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This CMake build is the Windows app. Open gs2print.xcodeproj on macOS.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  # Release is /MT so the zip is one file. Debug keeps the DLL debug CRT.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")
  add_compile_options(/utf-8)
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

set(ZLIB_SOURCES
  third_party/zlib/adler32.c
  third_party/zlib/compress.c
  third_party/zlib/crc32.c
  third_party/zlib/deflate.c
  third_party/zlib/inffast.c
  third_party/zlib/inflate.c
  third_party/zlib/inftrees.c
  third_party/zlib/trees.c
  third_party/zlib/uncompr.c
  third_party/zlib/zutil.c
)

file(GLOB LIBHARU_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/third_party/libharu/src/*.c")
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/core/src/*.cpp")

add_library(gs2print_core STATIC
  ${CORE_SOURCES}
  core/fonts/glyphs.cpp
  ${LIBHARU_SOURCES}
  ${ZLIB_SOURCES}
)

target_include_directories(gs2print_core
  PUBLIC core/include
  PRIVATE
    core/src
    core/fonts
    third_party/libharu/include
    third_party/zlib
)
target_compile_definitions(gs2print_core PRIVATE LIBHPDF_HAVE_ZLIB=1)

if(MSVC)
  set_source_files_properties(${CORE_SOURCES} core/fonts/glyphs.cpp
    PROPERTIES COMPILE_OPTIONS "/W4")
  set_source_files_properties(${LIBHARU_SOURCES} ${ZLIB_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS")
endif()

add_executable(gs2print_tests tests/main.cpp)
target_link_libraries(gs2print_tests PRIVATE gs2print_core)
if(MSVC)
  target_compile_options(gs2print_tests PRIVATE /W4)
endif()

set(WIN_SOURCES
  win/main.cpp
  win/app.cpp
  win/session.cpp
  win/window.cpp
  win/preview.cpp
  win/server.cpp
  win/ticket.cpp
  win/settings.cpp
  win/associate.cpp
  win/text.cpp
  win/gs2print.rc
)

set_source_files_properties(win/gs2print.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/win/app.manifest")

add_executable(gs2print WIN32 ${WIN_SOURCES})
target_link_libraries(gs2print PRIVATE
  gs2print_core
  ws2_32
  gdi32
  user32
  comctl32
  shell32
  ole32
  uuid
  iphlpapi
)
target_include_directories(gs2print PRIVATE win)
target_compile_definitions(gs2print PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(gs2print PRIVATE /W4)
  target_link_options(gs2print PRIVATE "/ENTRY:wWinMainCRTStartup" "/MANIFEST:NO")
endif()
