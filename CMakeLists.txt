cmake_minimum_required(VERSION 3.10)

# 1. 定义编译目标选项（必须在 project() 之前定义，以便配置工具链）
# 编译时可以通过 cmake -DPLATFORM=RK3566 .. 来切换
set(PLATFORM "RK3566" CACHE STRING "Target platform: WSL or RK3566")

if(PLATFORM STREQUAL "RK3566")
    # 兼容旧的 cmake -DPLATFORM=RK3566 调用。IDE 建议使用 rk3566 preset，
    # 由 cmake/rk3566-toolchain.cmake 在 project() 前配置交叉编译器。
    if(NOT CMAKE_TOOLCHAIN_FILE)
        set(TOOLCHAIN_DIR "/home/lzb/rk3566_taishan/buildroot/output/rockchip_rk3566/host")
        set(CMAKE_SYSTEM_NAME Linux)
        set(CMAKE_SYSTEM_PROCESSOR aarch64)
        set(CMAKE_C_COMPILER   ${TOOLCHAIN_DIR}/bin/aarch64-linux-gcc)
        set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/bin/aarch64-linux-g++)
        set(CMAKE_FIND_ROOT_PATH ${TOOLCHAIN_DIR}/aarch64-buildroot-linux-gnu/sysroot)
    endif()
endif()

project(my_move_camera)

# 2. 根据平台注入宏定义
if(PLATFORM STREQUAL "RK3566")
    message(STATUS "Configuring for RK3566 Cross-Compilation")
    # 强制设置颜色深度为 32 位以匹配板载 FB 格式，提升性能
    add_definitions(-DRK3566 -DLV_COLOR_DEPTH=32)
else()
    message(STATUS "Configuring for WSL (Native)")
    add_definitions(-DWSL -DLV_COLOR_DEPTH=32)
endif()

# ----------------------------------------------------------------
# 源代码配置
# ----------------------------------------------------------------

# 递归搜索 src 和 lvgl 文件夹中所有的 .c 文件
# 核心修正 1：只搜索 lvgl/src 目录，避免把 examples 里的多个 main 函数加进来
# 核心修正 2：请检查你的文件夹名是 Lvgl 还是 lvgl，必须与下方路径完全一致
file(GLOB_RECURSE SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/lvgl/src/*.c"
)

# 检查是否找到了源文件，防止 add_executable 报错
if(NOT SOURCES)
    message(FATAL_ERROR "No source files found in src/ or lvgl/!")
endif()

add_executable(${PROJECT_NAME} ${SOURCES})

# 添加头文件搜索路径
target_include_directories(${PROJECT_NAME} PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/inc"
    "${CMAKE_CURRENT_SOURCE_DIR}"      # 用于找到 lv_conf.h 和 lvgl/lvgl.h
)

if(PLATFORM STREQUAL "WSL")
    target_include_directories(${PROJECT_NAME} PRIVATE "/usr/include/SDL2")
    # The WSL image contains the JPEG runtime but not its development headers.
    # Reuse the declarations from the board sysroot; camera_media.c selects ABI 80 on WSL.
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../buildroot/output/rockchip_rk3566/host/aarch64-buildroot-linux-gnu/sysroot/usr/include"
    )
else()
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_FIND_ROOT_PATH}/usr/include/libdrm"
        "${CMAKE_FIND_ROOT_PATH}/usr/include/gstreamer-1.0"
        "${CMAKE_FIND_ROOT_PATH}/usr/include/glib-2.0"
        "${CMAKE_FIND_ROOT_PATH}/usr/lib/glib-2.0/include"
    )
endif()

# 核心修正 3：在 WSL 平台链接 SDL2，在 RK3566 平台链接普通库
if(PLATFORM STREQUAL "WSL")
    find_library(JPEG_LIBRARY
        NAMES libjpeg.so.8 jpeg
        PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu
    )
    if(NOT JPEG_LIBRARY)
        message(FATAL_ERROR "libjpeg runtime not found")
    endif()
    target_link_libraries(${PROJECT_NAME} m pthread SDL2 ${JPEG_LIBRARY})
else()
    target_link_libraries(${PROJECT_NAME}
        m pthread drm jpeg
        gstreamer-1.0 gstapp-1.0 gstbase-1.0 gobject-2.0 glib-2.0
    )
endif()

message(STATUS "Build Platform: ${PLATFORM}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER}")
