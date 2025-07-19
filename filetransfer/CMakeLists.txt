cmake_minimum_required(VERSION 3.14)
project(filetransfer)

find_package(Boost REQUIRED COMPONENTS system thread)
find_package(Protobuf CONFIG REQUIRED)

include(${Protobuf_INCLUDE_DIRS})

