cmake_minimum_required(VERSION 3.20)
project(jackoviz LANGUAGES C CXX)

# jackoviz — JACK + FFTW3 + Datoviz spectrogram with optional gRPC control plane
#
#   cmake -S . -B build -DDATOVIZ_ROOT=$HOME/work/datoviz
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug    # -g
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release  # -O3
#   cmake --build build
#   ./build/jackoviz-cli -s system:capture_1
#
# Linux fully-static musl build (both jackoviz-cli and jackoviz-gui):
#   cmake -S . -B build-musl -DJVZ_STATIC_MUSL=ON \
#     --toolchain cmake/toolchain-musl-static.cmake \
#     -DDATOVIZ_ROOT=$HOME/work/datoviz-musl \
#     -DCMAKE_PREFIX_PATH=/path/to/qt6-static-musl
#   cmake --build build-musl --target static-musl
#
# Requires a musl toolchain and *static* (.a) builds of every dependency
# (JACK, FFTW3, Datoviz, protobuf/gRPC, Qt 6 Quick). Vulkan/JACK still need
# their usual runtime services (ICD / jackd) on the target host.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug: -g | Release: -O3  (single-config generators: Makefile, Ninja)
set(JVZ_BUILD_TYPES Debug Release RelWithDebInfo MinSizeRel)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Debug (-g) or Release (-O3)" FORCE)
endif()
if(CMAKE_BUILD_TYPE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${JVZ_BUILD_TYPES})
endif()

# Explicit debug/release flags for GCC/Clang-compatible compilers.
add_compile_options(
  $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:-g>
  $<$<CONFIG:Release>:-O3>
)

option(JVZ_ENABLE_GRPC "Build and link the gRPC controller (jvzcontroller.proto)" ON)
option(JVZ_STATIC_MUSL
  "Linux: fully static executables linked against musl (prefer .a, -static)" OFF)

# ---------------------------------------------------------------------------
# Fully-static musl mode (configure-time; must run before find_package/pkg-config)
# ---------------------------------------------------------------------------
function(jvz_apply_static_musl_exe target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "jvz_apply_static_musl_exe: unknown target '${target}'")
  endif()
  set_target_properties(${target} PROPERTIES
    LINK_SEARCH_START_STATIC ON
    LINK_SEARCH_END_STATIC ON
    BUILD_RPATH ""
    INSTALL_RPATH ""
    SKIP_BUILD_RPATH TRUE
    SKIP_INSTALL_RPATH TRUE
  )
  target_link_options(${target} PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
  )
endfunction()

if(JVZ_STATIC_MUSL)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "JVZ_STATIC_MUSL is only supported on Linux")
  endif()
  if(CMAKE_VERSION VERSION_LESS "3.24")
    message(FATAL_ERROR "JVZ_STATIC_MUSL requires CMake ≥ 3.24 (for PKG_CONFIG_USE_STATIC_LIBS)")
  endif()

  # Prefer static archives everywhere.
  set(BUILD_SHARED_LIBS OFF)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  set(CMAKE_LINK_SEARCH_START_STATIC TRUE)
  set(CMAKE_LINK_SEARCH_END_STATIC TRUE)
  set(PKG_CONFIG_USE_STATIC_LIBS ON)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")

  # Refuse accidental glibc toolchains; use Alpine or cmake/toolchain-musl-static.cmake.
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -dumpmachine
    OUTPUT_VARIABLE JVZ_C_DUMPMACHINE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(NOT JVZ_C_DUMPMACHINE MATCHES "musl")
    message(FATAL_ERROR
      "JVZ_STATIC_MUSL requires a musl C toolchain (got '${JVZ_C_DUMPMACHINE}').\n"
      "  cmake -S . -B build-musl -DJVZ_STATIC_MUSL=ON \\\n"
      "    --toolchain ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-musl-static.cmake \\\n"
      "    -DDATOVIZ_ROOT=… -DCMAKE_PREFIX_PATH=…/qt6-static-musl")
  endif()

  message(STATUS "jackoviz: JVZ_STATIC_MUSL=ON (toolchain ${JVZ_C_DUMPMACHINE})")
  message(STATUS "jackoviz: all dependencies must provide musl-compatible static (.a) libraries")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
pkg_check_modules(FFTW3 REQUIRED IMPORTED_TARGET fftw3)

find_package(Threads REQUIRED)

# Prefer an explicit Datoviz source/build tree (common during v0.4 development).
set(DATOVIZ_ROOT "$ENV{HOME}/work/datoviz" CACHE PATH "Datoviz checkout with include/ and build/src/")
if(EXISTS "${DATOVIZ_ROOT}/include/datoviz.h")
  set(DATOVIZ_INCLUDE_DIR "${DATOVIZ_ROOT}/include")
  set(DATOVIZ_LIB_DIR "${DATOVIZ_ROOT}/build/src")
elseif(DEFINED ENV{DATOVIZ_ROOT} AND EXISTS "$ENV{DATOVIZ_ROOT}/include/datoviz.h")
  set(DATOVIZ_INCLUDE_DIR "$ENV{DATOVIZ_ROOT}/include")
  set(DATOVIZ_LIB_DIR "$ENV{DATOVIZ_ROOT}/build/src")
else()
  find_path(DATOVIZ_INCLUDE_DIR datoviz.h)
  find_library(DATOVIZ_LIBRARY datoviz)
  if(NOT DATOVIZ_INCLUDE_DIR OR NOT DATOVIZ_LIBRARY)
    message(FATAL_ERROR "Datoviz not found. Set -DDATOVIZ_ROOT=/path/to/datoviz")
  endif()
endif()

set(JVZ_SOURCES
  jackoviz.c
  jvz_jack_ringbuffer.c
)

if(JVZ_ENABLE_GRPC)
  # Module mode (CMake's FindProtobuf.cmake) — required for Debian/Ubuntu
  # system packages that do not ship protobuf-config.cmake.
  find_package(Protobuf REQUIRED MODULE)

  if(TARGET protobuf::protoc)
    set(JVZ_PROTOC protobuf::protoc)
  elseif(Protobuf_PROTOC_EXECUTABLE)
    set(JVZ_PROTOC "${Protobuf_PROTOC_EXECUTABLE}")
  else()
    message(FATAL_ERROR "protoc not found (install protobuf-compiler)")
  endif()

  # Do not use find_package(gRPC CONFIG): Homebrew/Debian gRPC configs often
  # call find_dependency(Protobuf CONFIG), which conflicts with module-mode
  # FindProtobuf. Use pkg-config instead (libgrpc++-dev / brew grpc).
  pkg_check_modules(GRPCXX REQUIRED IMPORTED_TARGET grpc++)
  find_program(JVZ_GRPC_CPP_PLUGIN NAMES grpc_cpp_plugin REQUIRED)
  set(JVZ_GRPC_LINK_LIBS PkgConfig::GRPCXX)

  set(JVZ_PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/jvzcontroller.proto")
  set(JVZ_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY "${JVZ_GEN_DIR}")

  set(JVZ_PROTO_SRCS "${JVZ_GEN_DIR}/jvzcontroller.pb.cc")
  set(JVZ_PROTO_HDRS "${JVZ_GEN_DIR}/jvzcontroller.pb.h")
  set(JVZ_GRPC_SRCS "${JVZ_GEN_DIR}/jvzcontroller.grpc.pb.cc")
  set(JVZ_GRPC_HDRS "${JVZ_GEN_DIR}/jvzcontroller.grpc.pb.h")

  add_custom_command(
    OUTPUT
      ${JVZ_PROTO_SRCS} ${JVZ_PROTO_HDRS}
      ${JVZ_GRPC_SRCS} ${JVZ_GRPC_HDRS}
    COMMAND ${JVZ_PROTOC}
      ARGS
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
        --cpp_out=${JVZ_GEN_DIR}
        --grpc_out=${JVZ_GEN_DIR}
        --plugin=protoc-gen-grpc=${JVZ_GRPC_CPP_PLUGIN}
        ${JVZ_PROTO_FILE}
    DEPENDS ${JVZ_PROTO_FILE}
    COMMENT "Generating gRPC C++ sources from jvzcontroller.proto"
    VERBATIM
  )

  list(APPEND JVZ_SOURCES
    grpcstub.cpp
    ${JVZ_PROTO_SRCS}
    ${JVZ_GRPC_SRCS}
  )
endif()
add_executable(jackoviz-cli ${JVZ_SOURCES})

target_include_directories(jackoviz-cli PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${DATOVIZ_INCLUDE_DIR}
)

if(JVZ_ENABLE_GRPC)
  target_include_directories(jackoviz-cli PRIVATE ${JVZ_GEN_DIR})
  target_compile_definitions(jackoviz-cli PRIVATE JVZ_HAS_GRPC=1)
endif()

target_compile_options(jackoviz-cli PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unused-parameter -Wno-typedef-redefinition>
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>
)

target_link_libraries(jackoviz-cli PRIVATE
  PkgConfig::JACK
  PkgConfig::FFTW3
  Threads::Threads
  m
)

if(DATOVIZ_LIB_DIR)
  target_link_directories(jackoviz-cli PRIVATE ${DATOVIZ_LIB_DIR})
  if(JVZ_STATIC_MUSL)
    find_library(JVZ_DATOVIZ_STATIC datoviz
      PATHS "${DATOVIZ_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(jackoviz-cli PRIVATE ${JVZ_DATOVIZ_STATIC})
  else()
    target_link_libraries(jackoviz-cli PRIVATE datoviz)
    set_target_properties(jackoviz-cli PROPERTIES
      BUILD_RPATH "${DATOVIZ_LIB_DIR}"
      INSTALL_RPATH "${DATOVIZ_LIB_DIR}"
    )
  endif()
else()
  target_link_libraries(jackoviz-cli PRIVATE ${DATOVIZ_LIBRARY})
endif()

if(JVZ_ENABLE_GRPC)
  if(TARGET protobuf::libprotobuf)
    target_link_libraries(jackoviz-cli PRIVATE protobuf::libprotobuf)
  else()
    target_include_directories(jackoviz-cli PRIVATE ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(jackoviz-cli PRIVATE ${Protobuf_LIBRARIES})
  endif()
  target_link_libraries(jackoviz-cli PRIVATE ${JVZ_GRPC_LINK_LIBS})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT JVZ_STATIC_MUSL)
  # shm_open lives in librt on glibc; musl folds it into libc.a.
  target_link_libraries(jackoviz-cli PRIVATE rt)
endif()

if(JVZ_STATIC_MUSL)
  jvz_apply_static_musl_exe(jackoviz-cli)
endif()

# Convenience: cmake --build build --target run
add_custom_target(run
  COMMAND $<TARGET_FILE:jackoviz-cli> -s system:capture_1
  DEPENDS jackoviz-cli
  USES_TERMINAL
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

message(STATUS "jackoviz: CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message(STATUS "jackoviz: DATOVIZ_INCLUDE_DIR=${DATOVIZ_INCLUDE_DIR}")
message(STATUS "jackoviz: JVZ_ENABLE_GRPC=${JVZ_ENABLE_GRPC}")

# ---------------------------------------------------------------------------
# Datoviz bug repro: surface grid shrink leaves stale index buffer
# ---------------------------------------------------------------------------
add_executable(surface_grid_shrink_repro repro/surface_grid_shrink.c)
target_include_directories(surface_grid_shrink_repro PRIVATE ${DATOVIZ_INCLUDE_DIR})
target_compile_options(surface_grid_shrink_repro PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Wno-typedef-redefinition)
target_link_libraries(surface_grid_shrink_repro PRIVATE m)
if(DATOVIZ_LIB_DIR)
  target_link_directories(surface_grid_shrink_repro PRIVATE ${DATOVIZ_LIB_DIR})
  if(JVZ_STATIC_MUSL)
    target_link_libraries(surface_grid_shrink_repro PRIVATE ${JVZ_DATOVIZ_STATIC})
    jvz_apply_static_musl_exe(surface_grid_shrink_repro)
  else()
    target_link_libraries(surface_grid_shrink_repro PRIVATE datoviz)
    set_target_properties(surface_grid_shrink_repro PROPERTIES
      BUILD_RPATH "${DATOVIZ_LIB_DIR}"
      INSTALL_RPATH "${DATOVIZ_LIB_DIR}"
    )
  endif()
else()
  target_link_libraries(surface_grid_shrink_repro PRIVATE ${DATOVIZ_LIBRARY})
  if(JVZ_STATIC_MUSL)
    jvz_apply_static_musl_exe(surface_grid_shrink_repro)
  endif()
endif()

# ---------------------------------------------------------------------------
# jackoviz-gui — Qt Quick controller (fork/exec launch + JACK list + gRPC)
# ---------------------------------------------------------------------------
option(JVZ_BUILD_REMOTE "Build the Qt Quick jackoviz-gui controller" ON)
# macOS .app bundle (Jackoviz.app). Ignored on other platforms.
if(APPLE)
  option(JVZ_REMOTE_MACOS_BUNDLE "Build jackoviz-gui as Jackoviz.app on macOS" ON)
else()
  set(JVZ_REMOTE_MACOS_BUNDLE OFF CACHE BOOL "Build jackoviz-gui as Jackoviz.app on macOS" FORCE)
endif()

if(JVZ_BUILD_REMOTE)
  find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)
  if(JVZ_STATIC_MUSL)
    get_target_property(_jvz_qt_core_type Qt6::Core TYPE)
    if(_jvz_qt_core_type STREQUAL "SHARED_LIBRARY")
      message(WARNING
        "JVZ_STATIC_MUSL: Qt6::Core is a shared library. "
        "Point CMAKE_PREFIX_PATH at a Qt 6 build configured with -static for musl.")
    endif()
  endif()

  qt_standard_project_setup(REQUIRES 6.5)

  qt_add_executable(jackoviz-gui
    jackoviz-remote.cpp
  )
  add_dependencies(jackoviz-gui jackoviz-cli)

  qt_add_qml_module(jackoviz-gui
    URI JackovizRemote
    VERSION 1.0
    QML_FILES
      jackoviz-remote.qml
    RESOURCE_PREFIX /
  )

  set_target_properties(jackoviz-gui PROPERTIES
    WIN32_EXECUTABLE TRUE
  )
  if(JVZ_REMOTE_MACOS_BUNDLE)
    set_target_properties(jackoviz-gui PROPERTIES
      OUTPUT_NAME "Jackoviz"
      MACOSX_BUNDLE TRUE
      MACOSX_BUNDLE_BUNDLE_NAME "Jackoviz"
      MACOSX_BUNDLE_GUI_IDENTIFIER "org.jackoviz.remote"
      MACOSX_BUNDLE_BUNDLE_VERSION "0.9.2"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "0.9.2"
    )
  else()
    set_target_properties(jackoviz-gui PROPERTIES
      OUTPUT_NAME "jackoviz-gui"
      MACOSX_BUNDLE FALSE
    )
  endif()

  target_link_libraries(jackoviz-gui PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    PkgConfig::JACK
  )
  target_include_directories(jackoviz-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  if(JVZ_ENABLE_GRPC)
    target_sources(jackoviz-gui PRIVATE
      ${JVZ_PROTO_SRCS}
      ${JVZ_GRPC_SRCS}
    )
    target_include_directories(jackoviz-gui PRIVATE ${JVZ_GEN_DIR})
    target_compile_definitions(jackoviz-gui PRIVATE JVZ_HAS_GRPC=1)
    if(TARGET protobuf::libprotobuf)
      target_link_libraries(jackoviz-gui PRIVATE protobuf::libprotobuf)
    else()
      target_include_directories(jackoviz-gui PRIVATE ${Protobuf_INCLUDE_DIRS})
      target_link_libraries(jackoviz-gui PRIVATE ${Protobuf_LIBRARIES})
    endif()
    target_link_libraries(jackoviz-gui PRIVATE ${JVZ_GRPC_LINK_LIBS})
  else()
    message(WARNING "JVZ_BUILD_REMOTE is ON but JVZ_ENABLE_GRPC is OFF — JACK port connect RPC disabled")
  endif()

  if(JVZ_STATIC_MUSL)
    jvz_apply_static_musl_exe(jackoviz-gui)
  endif()

  add_custom_target(run-remote
    COMMAND $<TARGET_FILE:jackoviz-gui>
    DEPENDS jackoviz-gui
    USES_TERMINAL
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )

  message(STATUS "jackoviz-gui: JVZ_REMOTE_MACOS_BUNDLE=${JVZ_REMOTE_MACOS_BUNDLE}")
endif()

# ---------------------------------------------------------------------------
# Convenience target: fully static musl build of both shipping binaries
# ---------------------------------------------------------------------------
if(JVZ_STATIC_MUSL)
  set(_jvz_static_deps jackoviz-cli)
  if(JVZ_BUILD_REMOTE)
    list(APPEND _jvz_static_deps jackoviz-gui)
  endif()
  add_custom_target(static-musl
    DEPENDS ${_jvz_static_deps}
    COMMENT "Fully static musl binaries: ${_jvz_static_deps}"
  )
  message(STATUS "jackoviz: build with: cmake --build <dir> --target static-musl")
else()
  add_custom_target(static-musl
    COMMAND ${CMAKE_COMMAND} -E echo
      "static-musl: reconfigure with -DJVZ_STATIC_MUSL=ON on Linux (see cmake/toolchain-musl-static.cmake)"
    VERBATIM
  )
endif()

message(STATUS "jackoviz: JVZ_STATIC_MUSL=${JVZ_STATIC_MUSL}")
