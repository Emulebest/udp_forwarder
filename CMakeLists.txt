cmake_minimum_required(VERSION 3.12)
project(UdpForwarder C)

set(CMAKE_C_STANDARD 11)

include_directories(rs)

add_subdirectory(rs)
link_directories(rs)

add_executable(UdpForwarder main.c)

target_link_libraries(UdpForwarder rs)
target_link_libraries(UdpForwarder pthread)