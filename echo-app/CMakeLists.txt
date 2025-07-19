cmake_minimum_required(VERSION 3.14)
project(echo-app)

find_package(Boost REQUIRED COMPONENTS system thread)

add_executable(echo_server_sync
  echo_server_sync.cpp
)

target_link_libraries(echo_server_sync
  ${Boost_LIBRARIES}
)

add_executable(echo_client_sync
  echo_client_sync.cpp
)

target_link_libraries(echo_client_sync
  ${Boost_LIBRARIES}
)

add_executable(echo_server_async
  echo_server_async.cpp
)

target_link_libraries(echo_server_async
  ${Boost_LIBRARIES}
)

add_executable(echo_client_async
  echo_client_async.cpp
)

target_link_libraries(echo_client_async
  ${Boost_LIBRARIES}
)

add_executable(echo_server_async_multithreaded
  echo_server_async_multithreaded.cpp
)

target_link_libraries(echo_server_async_multithreaded
  ${Boost_LIBRARIES}
)