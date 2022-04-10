cmake_minimum_required (VERSION 3.1)

project(MIMUW_SIK_UDP_SOCKETS)

set(CMAKE_C_STANDARD 11)
set(CXX_STANDARD 20)
set(CMAKE_C_FLAGS "-g -Wall -Wextra")

add_executable(echo-client echo-client.c)
add_executable(ticket-server ticket_server.cpp)

install(TARGETS DESTINATION .)
