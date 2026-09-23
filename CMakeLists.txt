# libwii-nx: the Wii's own hardware and system software.
#
# This is not built on its own. Its modules answer a game's SDK calls, which
# means they are compiled into the runtime beside libdol-nx's own - see
# cmake/Sources.cmake, which a runtime build includes.
#
# Configuring this directory by itself does nothing but say so, which is more
# use than a build that half works.
cmake_minimum_required(VERSION 3.20)
project(libwii-nx LANGUAGES CXX VERSION 2.0.0)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sources.cmake")
list(LENGTH WIINX_WII_SOURCES wiinx_wii_count)

if(PROJECT_IS_TOP_LEVEL)
    message(STATUS "libwii-nx: ${wiinx_wii_count} source files, built by a runtime build.")
    message(STATUS "  Point a build at this with -DWIINX_WII_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
endif()
