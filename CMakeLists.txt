# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.10)
project(autonomous-driving-car VERSION 1.1.0 LANGUAGES C CXX)

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

add_executable(autonomous-driving-car ${APP_SOURCES} ${APP_HEADERS})

target_include_directories(autonomous-driving-car PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)

# libnl3 headers are under /usr/include/libnl3
target_include_directories(autonomous-driving-car PRIVATE /usr/include/libnl3)

set_target_properties(autonomous-driving-car PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
)

target_link_libraries(autonomous-driving-car PRIVATE pthread crypt m)

find_library(LIBSYSTEMD systemd REQUIRED)
target_link_libraries(autonomous-driving-car PRIVATE ${LIBSYSTEMD})

find_library(LIBNL3 nl-3 REQUIRED)
target_link_libraries(autonomous-driving-car PRIVATE ${LIBNL3})

find_library(LIBNLROUTE3 nl-route-3 REQUIRED)
target_link_libraries(autonomous-driving-car PRIVATE ${LIBNLROUTE3})

# RKNN NPU 运行时（librknnrt.so，板载 /usr/lib）与 libjpeg-turbo（帧编解码）
find_library(LIBRKNNRT rknnrt REQUIRED)
target_link_libraries(autonomous-driving-car PRIVATE ${LIBRKNNRT})

find_library(LIBJPEG jpeg REQUIRED)
target_link_libraries(autonomous-driving-car PRIVATE ${LIBJPEG})

# OpenCV（core + imgproc）：检测框画框 / 标签文字渲染（cv::rectangle / cv::putText）
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
target_link_libraries(autonomous-driving-car PRIVATE ${OpenCV_LIBS})

# FFmpeg（可选）：提供 h264_rkmpp 硬件编码后端（Rockchip MPP）。
# 系统装有带 rkmpp 的 libavcodec 时自动启用；否则编译 V4L2 后端。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AVCODEC QUIET IMPORTED_TARGET libavcodec libavutil)
endif()
if(AVCODEC_FOUND)
    add_compile_definitions(HAVE_AVCODEC)
    target_link_libraries(autonomous-driving-car PRIVATE PkgConfig::AVCODEC)
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

add_executable(color_conversion_test tests/color_conversion_test.cpp source/yolo/yolo_draw.cpp source/yolo/yolo_image.c source/yolo/yolo_postprocess.c)
target_include_directories(color_conversion_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(color_conversion_test PRIVATE -UNDEBUG)
target_link_libraries(color_conversion_test PRIVATE ${OpenCV_LIBS} ${LIBJPEG})
add_test(NAME color_conversion COMMAND color_conversion_test)



add_executable(render_ownership_test tests/render_ownership_test.c source/video/frame_ring.c source/control/car_sport_ctrl.c)
target_include_directories(render_ownership_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(render_ownership_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
set_property(TARGET render_ownership_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
target_link_libraries(render_ownership_test PRIVATE pthread m)
add_test(NAME render_ownership COMMAND render_ownership_test)

add_executable(video_focus_metadata_test tests/video_focus_metadata_test.c)
target_include_directories(video_focus_metadata_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(video_focus_metadata_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
set_property(TARGET video_focus_metadata_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
target_link_libraries(video_focus_metadata_test PRIVATE pthread)
add_test(NAME video_focus_metadata COMMAND video_focus_metadata_test)

add_executable(yolo_events_test tests/yolo_events_test.c)
target_include_directories(yolo_events_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(yolo_events_test PRIVATE -UNDEBUG)
target_link_libraries(yolo_events_test PRIVATE pthread m)
add_test(NAME yolo_events COMMAND yolo_events_test)

add_executable(decoder_config_test tests/decoder_config_test.c source/http/http_util.c)
target_include_directories(decoder_config_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(decoder_config_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
set_property(TARGET decoder_config_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
target_link_libraries(decoder_config_test PRIVATE pthread)
add_test(NAME decoder_config COMMAND decoder_config_test)

add_executable(model_path_test tests/model_path_test.c)
target_include_directories(model_path_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(model_path_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
set_property(TARGET model_path_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
target_link_libraries(model_path_test PRIVATE pthread m)
add_test(NAME model_path COMMAND model_path_test)
add_test(NAME model_sync COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/model_sync_test.py)

add_executable(yolo_bulk_test tests/yolo_bulk_test.c)
target_include_directories(yolo_bulk_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(yolo_bulk_test PRIVATE -UNDEBUG)
target_link_libraries(yolo_bulk_test PRIVATE pthread m)
add_test(NAME yolo_bulk COMMAND yolo_bulk_test)
add_test(NAME detection_excel COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/detection_excel_test.py)
add_test(NAME detection_txt COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/detection_txt_test.py)
add_test(NAME coordinates_repair COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/coordinates_repair_test.py)

add_executable(detection_download_test tests/detection_download_test.c)
target_include_directories(detection_download_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(detection_download_test PRIVATE -UNDEBUG)
target_link_libraries(detection_download_test PRIVATE pthread)
add_test(NAME detection_download COMMAND detection_download_test)

add_executable(cpufreq_test tests/cpufreq_test.c source/http/http_util.c)
target_include_directories(cpufreq_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(cpufreq_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
set_property(TARGET cpufreq_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
target_link_libraries(cpufreq_test PRIVATE pthread)
add_test(NAME cpufreq COMMAND cpufreq_test)

add_executable(cpu_affinity_test tests/cpu_affinity_test.c)
target_include_directories(cpu_affinity_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(cpu_affinity_test PRIVATE -UNDEBUG)
target_link_libraries(cpu_affinity_test PRIVATE pthread)
add_test(NAME cpu_affinity COMMAND cpu_affinity_test)

add_executable(file_timestamps_test tests/file_timestamps_test.c)
target_include_directories(file_timestamps_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(file_timestamps_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
set_property(TARGET file_timestamps_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
target_link_libraries(file_timestamps_test PRIVATE pthread)
add_test(NAME file_timestamps COMMAND file_timestamps_test)

add_executable(http_files_test tests/http_files_test.c source/http/http_util.c)
target_include_directories(http_files_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)
target_compile_options(http_files_test PRIVATE -UNDEBUG)
target_link_libraries(http_files_test PRIVATE pthread)
add_test(NAME http_files COMMAND http_files_test)
add_test(NAME file_names COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/file_names_test.py)

add_executable(can_feature_test tests/can_feature_test.c source/can/can_feature.c source/can/can_queue.c)
target_include_directories(can_feature_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(can_feature_test PRIVATE -UNDEBUG)
target_link_libraries(can_feature_test PRIVATE pthread)
add_test(NAME can_feature COMMAND can_feature_test)

add_executable(tcp_vofa_test tests/tcp_vofa_test.c source/tcp/tcp_queue.c)
target_include_directories(tcp_vofa_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(tcp_vofa_test PRIVATE -UNDEBUG)
target_link_libraries(tcp_vofa_test PRIVATE pthread)
add_test(NAME tcp_vofa COMMAND tcp_vofa_test)
set_tests_properties(tcp_vofa PROPERTIES TIMEOUT 15)
