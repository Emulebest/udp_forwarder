cmake_minimum_required(VERSION 3.12)
project(rs C)

set(CMAKE_C_STANDARD 11)
set(rs_files rs.c rs.h)

add_library(rs ${rs_files})