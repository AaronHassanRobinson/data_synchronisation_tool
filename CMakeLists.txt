cmake_minimum_required(VERSION 4.3)
project(data_synchronisation_tool C)

set(CMAKE_C_STANDARD 23)
add_subdirectory(shared)
add_subdirectory(client)
add_subdirectory(server)

