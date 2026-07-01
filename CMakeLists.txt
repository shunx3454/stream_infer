cmake_minimum_required(VERSION 3.4.1)
project(stream_infer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
find_package(glog REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
)
pkg_check_modules(OpenCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(Mpp REQUIRED IMPORTED_TARGET rockchip_mpp)
pkg_check_modules(FMT REQUIRED IMPORTED_TARGET fmt)
#pkg_check_modules(RGA REQUIRED IMPORTED_TARGET librga)

message(STATUS "Mpp include dirs: ${Mpp_INCLUDE_DIRS}")
message(STATUS "Mpp library dirs: ${Mpp_LIBRARY_DIRS}")
message(STATUS "Mpp libraries: ${Mpp_LIBRARIES}")

add_executable(${PROJECT_NAME}
    main.cpp
    postprocess.cpp
    preprocess.cpp
    dma_alloc.cpp
    utils.cpp
    mpp_enc.cpp
    dmabuf.cpp
    rtsp.cpp
    v4l2_cap.cpp
    img_dmabuf.cpp
    rknn.cpp
    rga.cpp
    avfmt_img.cpp
    bufpool.cpp
    anime_postprocess.cpp
)

include_directories(${PROJECT_NAME}
    ${CMAKE_SOURCE_DIR}
    /usr/include/
    /home/rock/stb
    /home/rock/librga/include
)

target_link_libraries(${PROJECT_NAME} 
    PkgConfig::OpenCV
    PkgConfig::FFMPEG
    PkgConfig::FMT
    PkgConfig::Mpp
    #PkgConfig::RGA
        glog::glog
    /usr/lib/librknn_api.so
    /usr/lib/librknnrt.so
    /home/rock/librga/libs/Linux/gcc-aarch64/librga.so
)

# 内存检测选项
target_compile_options(${PROJECT_NAME} PRIVATE
    #-fsanitize=address
    #-fno-omit-frame-pointer
    -g
    #-gdwarf-4 -fno-omit-frame-pointer
)

target_link_options(${PROJECT_NAME} PRIVATE
    #-fsanitize=address
)
