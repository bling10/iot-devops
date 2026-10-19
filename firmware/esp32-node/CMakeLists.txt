# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{FIRMWARE_VERSION} AND NOT "$ENV{FIRMWARE_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{FIRMWARE_VERSION}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sample_project)
