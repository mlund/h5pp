cmake_minimum_required(VERSION 3.10)
project(writescalar)
add_executable(${PROJECT_NAME} writescalar.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE h5pp)

