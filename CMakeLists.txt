# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.10)
project(rockchip-smart-gateway VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP_NAME "${PROJECT_NAME}")
set(APP_VERSION "${PROJECT_VERSION}")
set(APP_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT APP_BUILD_TYPE)
    set(APP_BUILD_TYPE "Release")
endif()

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT APP_GIT_COMMIT)
    set(APP_GIT_COMMIT "")
endif()

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT APP_GIT_BRANCH)
    set(APP_GIT_BRANCH "")
endif()

execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(APP_GIT_STATUS)
    set(APP_GIT_DIRTY 1)
else()
    set(APP_GIT_DIRTY 0)
endif()

string(TIMESTAMP APP_BUILD_DATE "%Y-%m-%d %H:%M:%S" UTC)

file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/include/core")
file(WRITE "${PROJECT_SOURCE_DIR}/include/core/version.h"
    "/* SPDX-License-Identifier: GPL-3.0-or-later */\n"
    "#pragma once\n\n"
    "#define APP_NAME \"${APP_NAME}\"\n"
    "#define APP_VERSION \"${APP_VERSION}\"\n"
    "#define APP_GIT_COMMIT \"${APP_GIT_COMMIT}\"\n"
    "#define APP_GIT_BRANCH \"${APP_GIT_BRANCH}\"\n"
    "#define APP_GIT_DIRTY ${APP_GIT_DIRTY}\n"
    "#define APP_BUILD_TYPE \"${APP_BUILD_TYPE}\"\n"
    "#define APP_BUILD_DATE \"${APP_BUILD_DATE}\"\n"
)

set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Release flags")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g"      CACHE STRING "Debug flags")

# APP_BUILD_TYPE 已写入生成的 version.h，勿重复 add_compile_definitions（会触发重定义告警）

option(DEBUG "Enable debug output to terminal" OFF)
if(DEBUG)
    add_compile_definitions(DEBUG)
endif()

# 通配符收集源文件与头文件（新增模块无需再改 CMakeLists）
file(GLOB APP_SOURCES CONFIGURE_DEPENDS source/**/*.c source/**/*.cpp)
file(GLOB APP_HEADERS CONFIGURE_DEPENDS include/**/*.h)

add_executable(rockchip-smart-gateway ${APP_SOURCES} ${APP_HEADERS})

target_include_directories(rockchip-smart-gateway PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)

# libnl3 headers are under /usr/include/libnl3
target_include_directories(rockchip-smart-gateway PRIVATE /usr/include/libnl3)

set_target_properties(rockchip-smart-gateway PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
)

target_link_libraries(rockchip-smart-gateway PRIVATE pthread crypt m)

find_library(LIBSYSTEMD systemd REQUIRED)
target_link_libraries(rockchip-smart-gateway PRIVATE ${LIBSYSTEMD})

find_library(LIBNL3 nl-3 REQUIRED)
target_link_libraries(rockchip-smart-gateway PRIVATE ${LIBNL3})

find_library(LIBNLROUTE3 nl-route-3 REQUIRED)
target_link_libraries(rockchip-smart-gateway PRIVATE ${LIBNLROUTE3})

# RKNN NPU 运行时（librknnrt.so，板载 /usr/lib）与 libjpeg-turbo（帧编解码）
find_library(LIBRKNNRT rknnrt REQUIRED)
target_link_libraries(rockchip-smart-gateway PRIVATE ${LIBRKNNRT})

find_library(LIBJPEG jpeg REQUIRED)
target_link_libraries(rockchip-smart-gateway PRIVATE ${LIBJPEG})

# OpenCV（core + imgproc）：检测框画框 / 标签文字渲染（cv::rectangle / cv::putText）
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
target_link_libraries(rockchip-smart-gateway PRIVATE ${OpenCV_LIBS})

# FFmpeg（可选）：提供 h264_rkmpp 硬件编码后端（Rockchip MPP）。
# 系统装有带 rkmpp 的 libavcodec 时自动启用；否则编译 V4L2 后端。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AVCODEC QUIET IMPORTED_TARGET libavcodec libavutil)
endif()
if(AVCODEC_FOUND)
    add_compile_definitions(HAVE_AVCODEC)
    target_link_libraries(rockchip-smart-gateway PRIVATE PkgConfig::AVCODEC)
    message(STATUS "FFmpeg found: h264_rkmpp backend enabled")
else()
    message(STATUS "FFmpeg not found: fallback to V4L2 rkvenc backend")
endif()

# 视频帧环形队列单元测试（tests/ 独立 target，不入主程序 GLOB）
add_executable(frame_ring_test tests/frame_ring_test.c source/video/frame_ring.c)
target_include_directories(frame_ring_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(frame_ring_test PRIVATE pthread)

# Exercise the real fMP4 stream client over a local socketpair. Keep assertions
# enabled in Release builds because they validate emitted sample timestamps.
add_executable(h264_timeline_test tests/h264_timeline_test.c)
target_include_directories(h264_timeline_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/source/video)
target_compile_options(h264_timeline_test PRIVATE -UNDEBUG)
target_link_libraries(h264_timeline_test PRIVATE pthread)

enable_testing()
add_test(NAME frame_ring COMMAND frame_ring_test)
add_test(NAME h264_timeline COMMAND h264_timeline_test)

add_executable(realtime_queue_test tests/realtime_queue_test.c source/video/frame_ring.c)
target_include_directories(realtime_queue_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(realtime_queue_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
set_property(TARGET realtime_queue_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
target_link_libraries(realtime_queue_test PRIVATE pthread m)
add_test(NAME realtime_queue COMMAND realtime_queue_test)

add_executable(recording_split_test tests/recording_split_test.c)
target_include_directories(recording_split_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(recording_split_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
set_property(TARGET recording_split_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
target_link_libraries(recording_split_test PRIVATE pthread)
add_test(NAME recording_split COMMAND recording_split_test)
