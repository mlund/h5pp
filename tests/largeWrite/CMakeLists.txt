cmake_minimum_required(VERSION 3.10)
project(test-largeWrite)
add_executable(${PROJECT_NAME} largeWrite.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE h5pp)
add_test(NAME test-largeWrite WORKING_DIRECTORY ${CMAKE_BINARY_DIR} COMMAND ${PROJECT_NAME})
