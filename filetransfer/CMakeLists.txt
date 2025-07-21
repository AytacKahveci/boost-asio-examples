cmake_minimum_required(VERSION 3.14)
project(filetransfer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED)

include("${CMAKE_BINARY_DIR}/conan_toolchain.cmake")

find_package(Boost REQUIRED COMPONENTS system thread filesystem)
find_package(protobuf CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

get_filename_component(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto ABSOLUTE)
set(PROTO_FILE "${PROTO_DIR}/filetransfer.proto")
set(PROTO_GENERATED_DIR "${CMAKE_BINARY_DIR}")

add_custom_command(
  OUTPUT "${PROTO_GENERATED_DIR}/filetransfer.pb.cc" "${PROTO_GENERATED_DIR}/filetransfer.pb.h"
  COMMAND "protoc"
          "--proto_path=${PROTO_DIR}"
          "--cpp_out=${PROTO_GENERATED_DIR}"
          "${PROTO_FILE}"
  DEPENDS "${PROTO_FILE}"
  COMMENT "Generating Protobuf C++ code for ${PROTO_FILE}"
)

set(PROTO_GENERATED_SRCS
  "${PROTO_GENERATED_DIR}/filetransfer.pb.cc"
)

set(PROTO_GENERATED_HDRS
  "${PROTO_GENERATED_DIR}/filetransfer.pb.h"
)

include_directories(
  ${PROTO_GENERATED_DIR}
  ${protobuf_INCLUDE_DIRS}
)

add_executable(file_server
  src/file_server.cpp
  src/common.h
  ${PROTO_GENERATED_SRCS}
)

target_link_libraries(file_server
  ${protobuf_LIBRARIES}
  ${Boost_LIBRARIES}
  ${ZLIB_LIBRARIES}
)

add_custom_target(generate_proto_files
  DEPENDS
    ${PROTO_GENERATED_SRCS}
    ${PROTO_GENERATED_HDRS}
)
add_dependencies(file_server generate_proto_files)

add_executable(file_client
  src/file_client.cpp
  src/common.h
  ${PROTO_GENERATED_SRCS}
)

target_link_libraries(file_client
  ${protobuf_LIBRARIES}
  ${Boost_LIBRARIES}
  ${ZLIB_LIBRARIES}
)

add_dependencies(file_client generate_proto_files)
