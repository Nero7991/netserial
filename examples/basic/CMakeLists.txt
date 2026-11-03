cmake_minimum_required(VERSION 3.16)
# The repository root is the netserial component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(netserial_basic)
