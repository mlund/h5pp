cmake_minimum_required(VERSION 3.10)
project(test-copySwap)
set(CMAKE_VERBOSE_MAKEFILE OFF)

add_executable(${PROJECT_NAME} copySwap.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE h5pp)
add_test(NAME test-copySwap WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} COMMAND ${PROJECT_NAME})
