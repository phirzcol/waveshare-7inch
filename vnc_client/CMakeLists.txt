# ESP32-P4 VNC Client - Standalone App
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vnc_client)
