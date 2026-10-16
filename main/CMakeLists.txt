link_directories("${CMAKE_CURRENT_LIST_DIR}")

component_register(
    COMPONENT_NAME main

    SRCS
    ${CMAKE_CURRENT_LIST_DIR}/video_stack.cpp
    ${CMAKE_CURRENT_LIST_DIR}/app_api.cpp
    ${CMAKE_CURRENT_LIST_DIR}/event_payload.cpp
    ${CMAKE_CURRENT_LIST_DIR}/main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/http_server.cpp
    ${CMAKE_CURRENT_LIST_DIR}/motion_config.cpp
    ${CMAKE_CURRENT_LIST_DIR}/motion_detector.cpp
    ${CMAKE_CURRENT_LIST_DIR}/notifier.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rtsp_demo.cpp
    ${CMAKE_CURRENT_LIST_DIR}/frame_sei.cpp
    ${CMAKE_CURRENT_LIST_DIR}/frame_sync.cpp

    PRIVATE_REQUIREDS
    cvi_rtsp
    sophgo

    INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
)
