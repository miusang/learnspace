cmake_minimum_required(VERSION 3.22)

include_directories(./glad/include)
link_libraries(glfw3)
add_executable(test textures_combined.cpp glad/src/glad.c)