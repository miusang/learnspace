cmake_minimum_required(VERSION 3.22)

project(ffmpeg)
# 头文件和库文件已经提前配置到/urs/local/下，此处不用再指明头文件和库文件路径
# cmake -DCMAKE_BUILD_TYPE=Debug ..
link_libraries(avutil avcodec avformat avfilter swscale SDL2 swresample)

add_executable(test test_sync.c)