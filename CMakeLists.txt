cmake_minimum_required(VERSION 3.20)
project(jackoviz LANGUAGES C CXX)

# jackoviz — JACK + FFTW3 + Datoviz spectrogram with optional gRPC control plane
#
#   cmake -S . -B build -DDATOVIZ_ROOT=$HOME/work/datoviz
#   cmake --build build
#   ./build/jackoviz -s system:capture_1

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(JVZ_ENABLE_GRPC "Build and link the gRPC controller (jvzcontroller.proto)" ON)

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
  find_package(Protobuf CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)

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
    COMMAND protobuf::protoc
      ARGS
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
        --cpp_out=${JVZ_GEN_DIR}
        --grpc_out=${JVZ_GEN_DIR}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
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

add_executable(jackoviz ${JVZ_SOURCES})

target_include_directories(jackoviz PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${DATOVIZ_INCLUDE_DIR}
)

if(JVZ_ENABLE_GRPC)
  target_include_directories(jackoviz PRIVATE ${JVZ_GEN_DIR})
  target_compile_definitions(jackoviz PRIVATE JVZ_HAS_GRPC=1)
endif()

target_compile_options(jackoviz PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wno-unused-parameter -Wno-typedef-redefinition>
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>
)

target_link_libraries(jackoviz PRIVATE
  PkgConfig::JACK
  PkgConfig::FFTW3
  Threads::Threads
  m
)

if(DATOVIZ_LIB_DIR)
  target_link_directories(jackoviz PRIVATE ${DATOVIZ_LIB_DIR})
  target_link_libraries(jackoviz PRIVATE datoviz)
  set_target_properties(jackoviz PROPERTIES
    BUILD_RPATH "${DATOVIZ_LIB_DIR}"
    INSTALL_RPATH "${DATOVIZ_LIB_DIR}"
  )
else()
  target_link_libraries(jackoviz PRIVATE ${DATOVIZ_LIBRARY})
endif()

if(JVZ_ENABLE_GRPC)
  target_link_libraries(jackoviz PRIVATE
    protobuf::libprotobuf
    gRPC::grpc++
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(jackoviz PRIVATE rt)
endif()

# Convenience: cmake --build build --target run
add_custom_target(run
  COMMAND $<TARGET_FILE:jackoviz> -s system:capture_1
  DEPENDS jackoviz
  USES_TERMINAL
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

message(STATUS "jackoviz: DATOVIZ_INCLUDE_DIR=${DATOVIZ_INCLUDE_DIR}")
message(STATUS "jackoviz: JVZ_ENABLE_GRPC=${JVZ_ENABLE_GRPC}")
