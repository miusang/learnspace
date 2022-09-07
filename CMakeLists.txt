cmake_minimum_required(VERSION 3.22)
project(test_opengl)
include_directories(./glad/include)
link_libraries(glfw3)
add_executable(test 
    yuv420p.cpp 
    # textures_combined.cpp
    glad/src/glad.c)