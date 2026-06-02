cmake_minimum_required(VERSION 3.4.1)

project(${TARGET_NAME})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,--allow-shlib-undefined")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -Wl,--allow-shlib-undefined")

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${TARGET_NAME})

set(CMAKE_SKIP_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

# rknn api
set(RKNN_API_PATH ${CMAKE_SOURCE_DIR}/../runtime/librknn_api)

if (CMAKE_C_COMPILER MATCHES "aarch64")
  set(LIB_ARCH aarch64)
else()
  set(LIB_ARCH armhf)
endif()
set(RKNN_RT_LIB ${RKNN_API_PATH}/${LIB_ARCH}/librknnrt.so)
include_directories(${RKNN_API_PATH}/include)
include_directories(${CMAKE_SOURCE_DIR}/../3rdparty)


#rga
set(RGA_PATH ${CMAKE_SOURCE_DIR}/../3rdparty/rga/)

if (CMAKE_C_COMPILER MATCHES "aarch64")
  set(LIB_ARCH aarch64)
else()
  set(LIB_ARCH armhf)
endif()
set(RGA_LIB ${RGA_PATH}/lib/Linux//${LIB_ARCH}/librga.so)
include_directories( ${RGA_PATH}/include)

set(CMAKE_INSTALL_RPATH "lib")

# GStreamer (for RTSP server)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-rtsp-server-1.0
)
include_directories(${GST_INCLUDE_DIRS})

# demo
include_directories( ${CMAKE_SOURCE_DIR}/include)
include_directories( ${CMAKE_SOURCE_DIR}/src)

add_executable(${TARGET_NAME}
        src/main.cc
        src/model_utils.cc
        src/postprocess.cc
        src/camera_util.cc
        src/rtsp_stream.cc
        src/local_display.cc
        src/rga_func.cc
        # IPC layer
        src/ipc/unix_control_server.cc
        src/ipc/unix_data_publisher.cc
)

# ── Tool executables ─────────────────────────────────────────────────────────
# Minimal binaries: only standard C++ + POSIX, no RKNN/RGA/GStreamer needed.
# They can be built natively on x86 or cross-compiled for aarch64.

add_executable(control_client    tools/control_client.cc)
add_executable(data_receiver     tools/data_receiver.cc)
add_executable(tracks_receiver   tools/tracks_receiver.cc)
add_executable(events_receiver   tools/events_receiver.cc)
add_executable(event_summarizer  tools/event_summarizer.cc)

# ── ByteTrack service ────────────────────────────────────────────────────────
# Separate process that consumes raw detections from the YOLO data socket
# and republishes tracked detections (with track_id) on a tracks socket.
# Independent of RKNN/RGA/GStreamer — only standard C++ + POSIX.
add_executable(bytetrack_service
    tracker/src/bytetrack_service.cc
    tracker/src/iou_tracker.cc
)
target_include_directories(bytetrack_service PRIVATE
    ${CMAKE_SOURCE_DIR}/tracker/include
    ${CMAKE_SOURCE_DIR}/include
)

# ── Temporal-features service ───────────────────────────────────────────────
# Consumes tracked detections from bytetrack_service, computes per-track
# temporal features (velocity, heading, loiter, ROI events), publishes
# WireTrackSummary on the events socket.
add_executable(temporal_service
    temporal/src/temporal_service.cc
    temporal/src/track_state.cc
    temporal/src/track_manager.cc
)
target_include_directories(temporal_service PRIVATE
    ${CMAKE_SOURCE_DIR}/temporal/include
    ${CMAKE_SOURCE_DIR}/include
)

install(TARGETS control_client data_receiver tracks_receiver events_receiver
                event_summarizer bytetrack_service temporal_service
        DESTINATION ./)

target_link_libraries(${TARGET_NAME}
  ${RKNN_RT_LIB}
  ${RGA_LIB}
  ${GST_LIBRARIES}
  pthread
)

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${TARGET_NAME})
install(TARGETS ${TARGET_NAME} DESTINATION ./)

install(PROGRAMS ${RKNN_RT_LIB} DESTINATION lib)
install(PROGRAMS ${RGA_LIB} DESTINATION lib)

install(DIRECTORY data DESTINATION ./)

# Board-side helper scripts (run_qwen.sh, etc.). Installed as PROGRAMS so the
# executable bit is preserved; PATTERN keeps only shell scripts.
install(DIRECTORY utility_board_scripts DESTINATION ./
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE
        FILES_MATCHING PATTERN "*.sh")
